<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# LLD-009: Data-Driven Workload — Phase 0 상세 설계

**작성일:** 2026-10-27  
**최종 수정:** 2026-11-20 (Phase 0 내부 기술 리뷰 `I-01`~`I-09` 보완 반영)  
**상태:** Draft — Phase 0 조건부 설계 승인 (`OI-DD-01` 인터페이스 승인, `OI-DD-02`/`OI-DD-03` 조건부 승인; 구현 완료 아님)  
**상위 기준:** [REQ-009](REQ-009-data-driven-workload.md) §1.5, §6, §7.1, §8.2, [SRS-009](SRS-009-data-driven-workload.md), [HLD-009](HLD-009-data-driven-workload.md)  
**설계 결정:** [DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01`..`05`, [DDR-011](../DDR-011-runtime-table-update/DDR-011-runtime-table-update.md) `D-P0-05`, `D-P0-06`

---

## 1. 설계 경계, 사전 승인 대상 및 기존 접점

본 문서는 Phase 0의 공통 DAG 자료구조, ready/dispatch 분리, TT/CBS 실행 어댑터, 사전 확인과 시험 접점만 상세화한다. Phase 1의 Window/혼합 `E2E` 및 Phase 2의 `SKIP`/복구 상태 기계는 포함하지 않는다.

| 기존 접점 | Phase 0 상세 설계 의도 | 승인·호환 제약 |
|:---|:---|:---|
| `timpani-o/proto/*.proto` | optional Task class override, 구조화 `TaskReference`, workload/task hash 쌍 공통 DAG metadata | **`OI-DD-01` 인터페이스 승인** |
| `sample-apps/src/libttsched.h/c` | versioned Pipeline extension(`40B` 헤더), packed `ready_state`, dispatch-only futex, `current_job_token`/`wait_data`/`signal_ready`, 읽기 전용 seqlock 및 무-VFS RT 검증 | **`OI-DD-02` 조건부 승인** |
| `timpani-o/src/global_scheduler.*` | Task class를 explicit-over-inherit로 해석하고 DAG 검증 후 TT/CBS 배치와 graph meta를 분리 생성 | 기존 API 보존 |
| `timpani-n/src/main.cpp` | TT/CBS entry와 공통 graph meta를 함께 로컬 확인·등록 | 계층별 가능성 재계산 없음 |
| `timpani-n/src/timer_master.*` | L1 Successor는 허용 slot에서 `pending ready` 확인 후에만 `current_slot_map` 활성화·`dispatch_counter` 증가·`kick_cpu` 수행 | L2 release는 하지 않음 |
| `timpani-n/src/bpf/timpani.bpf.c` | 기존 `SCHED_TYPE_TT`/`CBS` dispatch gate 재사용 | 별도 DSQ 없음 |

---

## 2. L-DD-P0-01 — `/timpani_ttsched` 공유 메모리 레이아웃 (`libttsched.h`)

[DDR-009](DDR-009-data-driven-workload.md) `D-DD-P0-01` 및 [DDR-011](../DDR-011-runtime-table-update/DDR-011-runtime-table-update.md) `D-P0-05`에 따라 기존 헤더와 `tasks[TIMPANI_MAX_TASKS]`(오프셋 `0..783`)를 바이트 단위로 그대로 보존한다. 그 뒤에 버전이 있는 Pipeline extension header와 `pipeline[TIMPANI_MAX_TASKS]` 영역을 추가한다.

```c
#define TIMPANI_TTSCHED_NAME        "/timpani_ttsched"
#define TIMPANI_TTSCHED_PREFIX_SIZE 784U
#define TIMPANI_TTSCHED_MAGIC       0x54494D50U /* "TIMP" */
#define TIMPANI_PIPELINE_MAGIC      0x50495045U /* "PIPE" */
#define TIMPANI_PIPELINE_ABI_VERSION 1U
#define TIMPANI_MAX_TASKS           32
#define TIMPANI_MAX_OUT_EDGES       8

#define TIMPANI_PUB_WRITER_PENDING    0x80000000U
#define TIMPANI_PUB_ACTIVE_MASK       0x7FFFFFFFU
#define TIMPANI_PUB_DRAIN_TIMEOUT_US  5000U /* 정상 게시/갱신 drain 상한: 5 ms (또는 min(5 ms, H/4)) */
#define TIMPANI_PUB_MAX_DEFERS        2U    /* Phase 1+ 연속 drain 타임아웃 이연 상한 */
#define TIMPANI_PUB_SHUTDOWN_DRAIN_MS 50U   /* 종료/교체 강제 무효화(Safe Abandonment) 상한 */

#define TIMPANI_TRIG_ALL 0U /* Phase 0 유일한 유효 trigger */
#define TIMPANI_TRIG_ANY 1U /* Phase 1 이후 확장 예약값; Phase 0 게시 금지 */

#define TIMPANI_READY_MASK         0x0000FFFFULL
#define TIMPANI_SOURCE_RELEASE_VALID 0x00010000ULL
#define TIMPANI_READY_SEQUENCE_SHIFT 32
#define TIMPANI_READY_SEQUENCE_MASK  0xFFFFFFFFULL

/* 기존 슬롯 구조체 — 크기(24B) 및 필드 오프셋 변경 없음 */
struct timpani_task_slot {
    char     name[16]; /* task comm 이름 (PR_SET_NAME, <=15자 + NUL) */
    uint32_t counter;  /* Timer Master 주기 슬롯 발화 카운터 */
    uint32_t _pad;
};

struct timpani_out_edge {
    uint16_t dst_slot_idx; /* 후행 태스크의 SHM 인덱스 (0..TIMPANI_MAX_TASKS-1) */
    uint16_t bit_idx;      /* 후행 ready_mask에서 켤 비트 위치 (0..15) */
};

#define TIMPANI_EXEC_TT  0U
#define TIMPANI_EXEC_CBS 1U

struct timpani_pipeline_slot {
    /* Timpani-N이 TtSlot/CbsConfig 결합 검증 및 진단용으로 기록·보관한다.
     * Phase 0 libttsched는 노드 내 유일 1..15B ASCII tasks[i].name으로 슬롯을 조회한다. */
    uint64_t workload_id_hash;
    uint64_t task_id_hash;
    uint64_t ready_state;
    /* Successor: [63:32] ready sequence, [31:16] reserved, [15:0] ready mask.
     * Source: [63:32] released activation ordinal, bit 16 = release-valid,
     * [15:0] = 0. */
    uint32_t required_mask;    /* 필요한 선행 비트 (0이면 Source/일반 Task) */
    uint32_t dispatch_counter; /* TT slot 또는 CBS release가 허용한 실행 수, 유일한 futex word */
    uint16_t num_out_edges;
    uint16_t trigger_mode;     /* Phase 0: TIMPANI_TRIG_ALL */
    uint16_t execution_model;  /* TT/CBS; Source/Successor 역할과 독립 */
    uint16_t reserved;
    struct timpani_out_edge out_edges[TIMPANI_MAX_OUT_EDGES];
};

/* 후방 확장 header (40B) — tasks[]가 끝나는 byte 784에 배치 */
struct timpani_pipeline_header {
    uint32_t magic;              /* TIMPANI_PIPELINE_MAGIC (무효화/종료 시 0) */
    uint16_t abi_version;        /* TIMPANI_PIPELINE_ABI_VERSION */
    uint16_t reserved;
    uint32_t abi_size;           /* 전체 struct timpani_ttsched_shm 크기 (3128B) */
    uint32_t publication_guard;  /* bit31=writer pending, 하위 31bit=활성 write commit 수 */
    uint64_t instance_id;        /* SHM 객체 고유 식별자 (0 불가; 교체·재생성 시 신규 발급) */
    uint64_t origin_epoch_ns;    /* schedule/diagnostic epoch; not the job_token source */
    uint64_t hyperperiod_us;     /* fixed schedule hyperperiod (us, > 0) */
};

struct timpani_ttsched_shm {
    uint32_t magic;      /* 안정 게시 완료 시 TIMPANI_TTSCHED_MAGIC, 게시 중/종료 시 0 */
    uint32_t n_tasks;    /* 점유 슬롯 상한 인덱스 + 1 */
    uint32_t generation; /* 홀수 = 게시 중, 짝수(>0) = 안정 게시 상태 */
    uint32_t _pad;
    struct timpani_task_slot tasks[TIMPANI_MAX_TASKS];
    struct timpani_pipeline_header pipeline_header;
    struct timpani_pipeline_slot pipeline[TIMPANI_MAX_TASKS];
};

_Static_assert(offsetof(struct timpani_ttsched_shm, pipeline_header) ==
                   TIMPANI_TTSCHED_PREFIX_SIZE,
               "preserve the existing 784-byte SHM prefix");
_Static_assert(sizeof(struct timpani_pipeline_header) == 40,
               "keep the pipeline slots 8-byte aligned");
_Static_assert(sizeof(struct timpani_pipeline_slot) % sizeof(uint64_t) == 0,
               "pipeline slot must be 64-bit aligned");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0),
               "64-bit ready_state atomic access must be lock-free");
```

새 client는 비RT 초기화 경로인 `ttsched_init()`에서만 `fstat()`로 SHM 크기를 확인한다. 파일 크기가 기존 784바이트 prefix보다 짧으면 TT 경로도 `EINVAL`로 거절하고, prefix는 유효하지만 extension 전체(`sizeof(struct timpani_ttsched_shm)`)가 없으면 `g_shm_pipeline_capable = 0`으로 설정해 기존 `ttsched_wait_next_period()`만 계속 사용할 수 있게 한다. 이후 RT critical path(`signal_ready`, `wait_data`, `current_job_token`)에서는 `shm_open`/`fstat`/`close` 같은 VFS/fd 시스템 콜을 일절 호출하지 않고, 순수 사용자 공간 원자 로드(`g_shm_pipeline_capable`, `shm->magic`, 짝수 `generation`, `pipeline_header.{magic, abi_version, abi_size, instance_id}`, `publication_guard`)만으로 ABI·세대·교체/무효화를 검증한다.

live SHM object는 수명 동안 고정 크기다. `Timpani-N`은 full-layout mapping을 게시하기 전에 전체 ABI 크기(`3,128`B)로 생성하고, client가 mapping할 수 있는 동안 크기를 그 아래로 `ftruncate()`해서는 안 된다(no-shrink 불변량). 교체·종료 시에는 `WRITER_PENDING` 설정, active commit drain(최대 `TIMPANI_PUB_SHUTDOWN_DRAIN_MS = 50 ms`), `shm->magic = 0`·`pipeline_header.magic = 0` 무효화 뒤 구 object를 `ftruncate()`하거나 in-place 덮어쓰지 않고 `shm_unlink()`하며, 신규 객체는 `O_CREAT | O_EXCL`과 새 `instance_id`로 생성한다. 구 mapping을 보유한 client는 다음 RT 호출에서 `magic == 0`·`WRITER_PENDING` 또는 `instance_id` 불일치를 원자 로드로 즉시 탐지해 `ESTALE`을 반환하고, 비RT 제어 경로에서 `ttsched_init()`을 재호출해 새 객체로 재연결한다.

`publication_guard`는 extension header 안에서 쓰기 commit(`ttsched_signal_ready()` 및 `Timer Master` L1 어댑터)과 게시 세대 전환 사이의 TOCTOU를 막는다. 게시자는 `TIMPANI_PUB_WRITER_PENDING` 비트를 원자적으로 켜 새 쓰기 commit을 차단하고, 기존 활성 commit이 `0`이 될 때까지 제한된 시간(`TIMPANI_PUB_DRAIN_TIMEOUT_US = 5 ms` 또는 `min(5 ms, H/4)`, 종료/교체 시 `50 ms`)만 대기한 뒤 `magic = 0`·홀수 `generation`으로 SHM을 갱신한다. 반면 SHM에 쓰지 않는 순수 읽기 소비자 API(`ttsched_wait_data()`, `ttsched_current_job_token()`)는 `publication_guard`에 쓰지 않는 **읽기 전용 seqlock** 경로를 사용하여 `O_RDONLY`(`PROT_READ`) 매핑에서도 `SIGSEGV`나 소비자 크래시에 의한 guard 누수 없이 안전하게 동작한다.

---

## 3. L-DD-P0-02 — `libttsched` 동기화 및 안정 게시 확인 (`libttsched.c`)

### 3.1 SHM 매핑, 배포 권한 모델 및 공개 API 계약

- 기존 공개 API는 유지한다: `ttsched_init()`은 기존 TT prefix를 위한 `O_RDONLY` fallback을 계속 지원하고 `ttsched_wait_next_period()`의 signature와 의미를 바꾸지 않는다.
- `ttsched_init()`은 비RT 초기화 경로에서 `shm_open()` → `fstat()`로 실제 객체 크기를 1회 확인한다. `st_size < 784`이면 `EINVAL`로 실패한다. `st_size >= 784`이면 `mmap()`으로 매핑하고 **`mmap()` 직후 SHM 파일 디스크립터를 즉시 `close()`** 한다. 매핑 크기가 전체 `sizeof(struct timpani_ttsched_shm)` 이상이면 `g_shm_pipeline_capable = 1`로 설정하고, 구 784B 객체나 짧은 매핑이면 `g_shm_pipeline_capable = 0`으로 두어 TT 경로만 유지한다.
- Pipeline-capable `ttsched_init()`은 자기 Task slot과 안정 세대를 확인한 뒤 최초 attach cursor로 해당 slot의 현재 `dispatch_counter`를 acquire load해 `g_last_dispatch_counter = cur`, `g_dispatch_counter_synced = 1`로 기록한다. attach 이전 dispatch는 replay하지 않는다. Phase 0 정상 시험에서는 모든 Successor가 최초 dispatch 전에 이 초기화를 완료해야 하므로 `cur=0`인 attach가 token `0`를 보존하고, 늦게 붙은 `cur=N` consumer는 이미 지난 token을 반환하지 않는다.
- **스레드 종료 시 리소스 해제**: thread-local 매핑 상태(`g_shm`, `g_shm_map_size`)는 프로세스당 1회 `pthread_once`로 등록한 `pthread_key_create()` destructor에 연결해, 워커 스레드 종료 시 자동으로 `munmap(g_shm, g_shm_map_size)`를 수행함으로써 스레드 생성·종료 반복에 따른 VMA/fd 누수를 원천 차단한다.
- RT 경로(`ttsched_signal_ready()`, `ttsched_wait_data()`, `ttsched_current_job_token()`)에서는 `shm_open`/`fstat`/`close`를 호출하지 않는다. 먼저 `g_shm && g_shm_pipeline_capable`을 확인해 legacy/짧은 매핑에서는 extension dereference 없이 즉시 `ENOTSUP`을 반환하고, 이어서 순수 원자 로드로 `publication_guard`의 `WRITER_PENDING`, `shm->magic`, 짝수 `generation`, `pipeline_header.{magic, abi_version, abi_size, instance_id}`를 검증한다.
- 실패한 Pipeline 호출은 RT 경로에서 silently remap/reopen하지 않는다. `ESTALE`을 받은 caller는 비RT 제어 경로에서 `ttsched_init()`을 다시 호출하며, 재초기화는 기존 매핑을 `munmap()`하고 현재 `/timpani_ttsched` 객체를 다시 `shm_open()`/`fstat()`/`mmap()`/`close()`한 뒤 새 세대·신원과 현재 `dispatch_counter`를 attach cursor로 다시 snapshot한다. 과거 token은 replay하지 않는다. 재초기화 전 반복 호출은 안전하게 `ESTALE`을 반환한다.
- **읽기 전용(`O_RDONLY`) vs 쓰기(`O_RDWR`) 권한 및 배포 모델**:
  - Data-Driven 완료 신호를 기록하는 writer(Source 및 중계 Successor)는 `O_RDWR` + `PROT_READ|PROT_WRITE`로 매핑하며, 쓰기 권한이 없으면 `g_shm_writable = 0`으로 `O_RDONLY` fallback한다.
  - 읽기 전용 소비자(일반 TT 및 Sink/read-only Successor)는 `O_RDONLY` + `PROT_READ` 매핑으로 `ttsched_wait_data()`와 `ttsched_current_job_token()`을 정상 호출할 수 있으며(읽기 전용 seqlock 사용, `publication_guard` 미기록), `ttsched_signal_ready()` 호출 시에만 `EACCES`를 반환한다.
  - `Timpani-N`만 SHM 생성·`ftruncate()`·`shm_unlink()` 수명 주기를 소유한다. 생산 배포용 `/dev/shm/timpani_ttsched` 권한 모델은 **소유자 UID `timpani-n`(`rw-`) + POSIX ACL/전용 그룹 분리(`g:timpani-pub:rw-`는 신뢰 writer 컨테이너, `g:timpani-sub:r--`는 일반 TT·read-only Successor/Sink 컨테이너, other `---`)** 를 표준으로 한다(ACL 미지원 경량 환경에서는 `0660` + `timpani-pub` 그룹과 컨테이너 마운트/읽기 전용 바인드로 동등한 격리를 적용한다). 기존 PoC의 `0666`은 로컬 회귀 시험 전용으로 한정하며 생산 ABI 계약으로 간주하지 않는다.
  - **Phase 0 trust boundary**: `O_RDWR`/`PROT_WRITE`는 전체 SHM mapping이므로 `timpani-pub` 그룹 구성원은 OS 수준에서 header와 다른 Task slot도 변경할 수 있다. 이 그룹은 상호 신뢰된 cooperative writer만 포함해야 하며, `libttsched`의 자기 slot/세대 검사는 우발적 오용과 corruption 탐지용이지 악의적 writer 격리 또는 memory integrity guarantee가 아니다. 비신뢰 workload는 반드시 `timpani-sub`의 read-only 경로를 사용한다. slot별 capability, per-slot SHM, kernel mediator 기반 강한 격리는 Phase 0 범위가 아니다.
- 공개 API 시그니처 *(조건부 승인, `OI-DD-02`)*:
  ```c
  int ttsched_wait_next_period(void);              /* 기존 TT API, 변경 없음 */
  int ttsched_current_job_token(uint32_t *job_token); /* L1/L2 Source 공통 활성화 token 취득 (O_RDONLY 가능) */
  int ttsched_wait_data(uint32_t *job_token);      /* 실제 dispatch wake만 0, FIFO 순차 소비 (O_RDONLY 가능) */
  int ttsched_signal_ready(uint32_t job_token);    /* 같은 job의 완료 commit (O_RDWR 필요) */
  ```
- 신규 API는 실패 시 `-1`과 `errno`를 반환한다: `EACCES`(write 권한 없음), `ENOTSUP`(legacy/짧은 mapping 또는 extension magic은 유효하지만 미지원 ABI version), `EINVAL`(NULL 인자, extension magic/ABI size, Source-role/release-state metadata 오류 또는 outgoing-edge metadata 오류), `ESTALE`(종료/교체로 인한 `magic=0`/`WRITER_PENDING`/`instance_id` 변경, publish/control wake, 세대 전환 또는 Successor의 활성 ready sequence와 다른 job token), `EALREADY`(활성 job token의 같은 선행 비트가 이미 commit됨), `EAGAIN`(commit contention 또는 Source release가 아직 stamp되지 않음), `ENOENT`(Task/후행 참조 없음)다.
- **Sink 태스크의 `ttsched_signal_ready()` 호출 규약**: `num_out_edges == 0`인 Sink 태스크가 완료 직후 관행적으로 `ttsched_signal_ready(job_token)`를 호출할 경우, 쓰기 권한·SHM ABI/세대·`n_tasks`·자기 슬롯 신원(`tasks[g_task_idx].name`)을 정상 검증한 뒤 outgoing-edge 변경 없이 `0`(안전한 no-op)을 반환한다. 단, `O_RDONLY` 매핑 Sink는 `signal_ready()`를 호출하지 않으며 호출 시 `EACCES`를 반환한다.
- `ttsched_signal_ready()`는 첫 `ready_state` 변경 전에 제한된 stack plan에 전체 outgoing-edge 목록을 snapshot·검증한다. 검증 대상은 `n_tasks`, `num_out_edges`, `dst_slot_idx`, self/중복 destination, `bit_idx`, `required_mask` 하위 16비트 유효성 및 소속, `trigger_mode`, `execution_model`, 각 destination의 token/중복 bit 상태다. 어느 edge에서라도 실패하면 predecessor mask, dispatch counter, futex wake가 하나도 바뀌지 않은 채 `-1`을 반환하며, CBS dispatch counter와 wake는 모든 edge CAS가 끝나거나 동시 동일 호출에 의해 완료된 뒤에만 수행한다.
- **공통 `job_token` 도메인 및 호출 시점 규약**: Phase 0에서 `job_token`은 현재 `ready_sequence`에 대응하는 32비트 논리 activation ordinal이며 timestamp나 Task-local 호출 횟수가 아니다. Timpani-N의 schedule release authority는 실제 Source activation 직전에 공통 ordinal을 해당 Source slot의 `ready_state` high 32bit와 `TIMPANI_SOURCE_RELEASE_VALID` 표식으로 함께 기록한 뒤 Task를 wake한다. 같은 고정 주기 `H` Pipeline의 L1/L2 Source는 **활성화 직후** `ttsched_current_job_token(&token)`을 1회 호출해 이 release snapshot을 읽고 처리 완료 후 `ttsched_signal_ready(token)`에 전달한다. `origin_epoch_ns`, `hyperperiod_us`, `CLOCK_REALTIME` 또는 앱-local counter로 token을 다시 계산해서는 안 되며, release 표식이 없는 상태에서 `current_job_token()`을 호출하면 상태 변경 없이 `EAGAIN`을 반환한다. Successor는 `ttsched_wait_data(&token)`이 FIFO 순차로 반환한 `token`을 그대로 다음 단계 `ttsched_signal_ready(token)`에 전달한다. Harmonic 주기-token 매핑은 Phase 1에서 정의한다.
- **애플리케이션 소유 payload 계약**:
  - 009/libttsched는 payload를 저장·복사·파싱하지 않으며 payload 위치, 자료 구조, producer/consumer ownership acknowledgement API를 소유하지 않는다.
  - Producer는 `job_token`에 대응하는 payload slot에 최종 내용을 모두 기록하고 필요한 application memory barrier를 완료한 뒤에만 `ttsched_signal_ready(job_token)`을 호출한다.
  - Source/Successor의 ready CAS와 `dispatch_counter`의 release/acquire 순서가 이후 `wait_data(token)` 또는 TT/CBS dispatch 소비에 대한 visibility edge를 형성한다. Consumer는 token 취득 전 payload를 읽어서는 안 된다.
  - Producer는 해당 token을 받아 처리 중인 모든 직속 Successor가 완료됨을 애플리케이션 소유 ack/ownership 메커니즘으로 확인하기 전 동일 payload slot을 overwrite하지 않는다. Phase 0 PoC는 pending `<= 1` 전제를 반영해 최소 2개의 token-addressed slot을 사용한다.
  - `signal_ready()` 거절(`EALREADY`, `ESTALE`, `EINVAL` 등)은 새 payload를 승인하지 않는다. Producer는 거절된 payload가 활성 token의 승인된 payload를 덮어쓰지 않도록 애플리케이션 버퍼를 격리한다.
  - 009는 payload 내용·generation 일치·overwrite를 직접 검증하지 않는다. 이 애플리케이션 계약 위반은 Phase 0 시험 전제 이탈이자 payload correctness 미보장 사례다.

### 3.2 안정 세대 확인, `ttsched_current_job_token()`, `ttsched_signal_ready()` 및 `ttsched_wait_data()` 상세 로직

```c
/* 스레드 종료 시 thread-local SHM 매핑을 자동 해제하는 destructor */
struct ttsched_tls_mapping {
    void*  addr;
    size_t size;
};

static pthread_once_t g_tls_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t  g_tls_unmap_key;

static void ttsched_tls_unmap_dtor(void* arg)
{
    struct ttsched_tls_mapping* m = (struct ttsched_tls_mapping*)arg;
    if (m && m->addr && m->size > 0) {
        munmap(m->addr, m->size);
        m->addr = NULL;
        m->size = 0;
    }
}

static void ttsched_tls_key_init(void)
{
    pthread_key_create(&g_tls_unmap_key, ttsched_tls_unmap_dtor);
}

static __thread volatile struct timpani_ttsched_shm* g_shm = NULL;
static __thread struct ttsched_tls_mapping g_shm_mapping = { NULL, 0 };
static __thread size_t   g_shm_map_size = 0;
static __thread int      g_shm_pipeline_capable = 0;
static __thread int      g_shm_writable = 0;
static __thread uint64_t g_shm_instance_id = 0;
static __thread uint32_t g_shm_generation = 0;
static __thread int      g_task_idx = -1;
static __thread char     g_my_name[16] = {};
static __thread int      g_name_cached = 0;
static __thread uint32_t g_last_dispatch_counter = 0;
static __thread int      g_dispatch_counter_synced = 0;

/* 참고: ttsched_init()은 비RT 경로에서 shm_open() -> fstat()로 크기를 1회
 * 검증한 뒤 mmap() 직후 SHM fd를 즉시 close()하고,
 * pthread_once(&g_tls_key_once, ttsched_tls_key_init) 및
 * pthread_setspecific(g_tls_unmap_key, &g_shm_mapping)로 스레드 종료 해제를
 * 등록한다. g_shm_map_size >= sizeof(struct timpani_ttsched_shm)이면
 * g_shm_pipeline_capable = 1, 아니면 0으로 설정하고 g_shm_instance_id = 0으로
 * 초기화한다. */

/* acquire 순서로 안정 게시 상태(유효 magic + 0이 아닌 짝수 generation) 확인 */
static inline int read_stable_gen(const volatile struct timpani_ttsched_shm* shm,
                                  uint32_t* out_gen)
{
    uint32_t magic = __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE);
    uint32_t gen   = __atomic_load_n(&shm->generation, __ATOMIC_ACQUIRE);
    if (magic != TIMPANI_TTSCHED_MAGIC || gen == 0U || (gen & 1U) != 0U) {
        errno = ESTALE;
        return -1;
    }
    *out_gen = gen;
    return 0;
}

/* RT 경로 무-VFS 순수 원자 검증: ABI·세대·WRITER_PENDING·instance_id 확인 */
static inline int read_stable_pipeline_gen(
    const volatile struct timpani_ttsched_shm* shm,
    uint32_t* out_gen, uint64_t* out_instance_id)
{
    if (!shm || !g_shm_pipeline_capable) {
        errno = ENOTSUP;
        return -1;
    }
    uint32_t guard = __atomic_load_n(
        &shm->pipeline_header.publication_guard, __ATOMIC_ACQUIRE);
    if ((guard & TIMPANI_PUB_WRITER_PENDING) != 0U) {
        errno = ESTALE;
        return -1;
    }
    if (read_stable_gen(shm, out_gen) != 0)
        return -1;

    uint32_t pmagic = __atomic_load_n(
        &shm->pipeline_header.magic, __ATOMIC_ACQUIRE);
    if (pmagic == 0U) {
        errno = ESTALE;
        return -1;
    }
    if (pmagic != TIMPANI_PIPELINE_MAGIC) {
        errno = EINVAL;
        return -1;
    }
    if (__atomic_load_n(&shm->pipeline_header.abi_version,
                        __ATOMIC_ACQUIRE) != TIMPANI_PIPELINE_ABI_VERSION) {
        errno = ENOTSUP;
        return -1;
    }
    if (__atomic_load_n(&shm->pipeline_header.abi_size,
                        __ATOMIC_ACQUIRE) != sizeof(*shm)) {
        errno = EINVAL;
        return -1;
    }
    uint64_t inst_id = __atomic_load_n(
        &shm->pipeline_header.instance_id, __ATOMIC_ACQUIRE);
    if (inst_id == 0ULL ||
        (g_shm_instance_id != 0ULL && inst_id != g_shm_instance_id)) {
        errno = ESTALE;
        return -1;
    }
    if (out_instance_id)
        *out_instance_id = inst_id;
    return 0;
}

static inline int find_task_slot(
    const volatile struct timpani_ttsched_shm* shm,
    uint32_t n_tasks, const char* name)
{
    if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS)
        return -1;
    for (uint32_t i = 0; i < n_tasks; i++) {
        if (strncmp((const char*)shm->tasks[i].name, name, 15) == 0)
            return (int)i;
    }
    return -1;
}

static inline uint32_t ready_sequence(uint64_t state)
{
    return (uint32_t)((state >> TIMPANI_READY_SEQUENCE_SHIFT) &
                      TIMPANI_READY_SEQUENCE_MASK);
}

static inline uint32_t ready_mask(uint64_t state)
{
    return (uint32_t)(state & TIMPANI_READY_MASK);
}

static inline uint64_t make_ready_state(uint32_t sequence, uint32_t mask)
{
    return ((uint64_t)sequence << TIMPANI_READY_SEQUENCE_SHIFT) |
           (uint64_t)(mask & TIMPANI_READY_MASK);
}

static inline uint64_t make_source_release_state(uint32_t sequence)
{
    return ((uint64_t)sequence << TIMPANI_READY_SEQUENCE_SHIFT) |
           TIMPANI_SOURCE_RELEASE_VALID;
}

static inline int counter_ahead(uint32_t current, uint32_t previous)
{
    return (int32_t)(current - previous) > 0;
}

static inline void pipeline_commit_exit(
    const volatile struct timpani_ttsched_shm* shm)
{
    uint32_t after = __atomic_sub_fetch(
        (uint32_t*)&shm->pipeline_header.publication_guard, 1U,
        __ATOMIC_ACQ_REL);
    if ((after & TIMPANI_PUB_WRITER_PENDING) != 0U &&
        (after & TIMPANI_PUB_ACTIVE_MASK) == 0U) {
        syscall(SYS_futex,
                (uint32_t*)&shm->pipeline_header.publication_guard,
                FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

/* 쓰기 경로(ttsched_signal_ready) 전용 commit guard 진입 (O_RDWR 필수) */
static inline int pipeline_commit_enter(
    const volatile struct timpani_ttsched_shm* shm,
    uint32_t* out_gen, uint64_t* out_inst)
{
    uint32_t expected_gen;
    uint64_t expected_inst;
    if (read_stable_pipeline_gen(shm, &expected_gen, &expected_inst) != 0)
        return -1;

    uint32_t guard_before = __atomic_fetch_add(
        (uint32_t*)&shm->pipeline_header.publication_guard, 1U,
        __ATOMIC_ACQ_REL);
    if ((guard_before & TIMPANI_PUB_WRITER_PENDING) != 0U) {
        pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
        errno = ESTALE;
        return -1;
    }
    if ((guard_before & TIMPANI_PUB_ACTIVE_MASK) ==
            TIMPANI_PUB_ACTIVE_MASK) {
        pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
        errno = EAGAIN;
        return -1;
    }

    uint32_t confirmed_gen;
    uint64_t confirmed_inst;
    if (read_stable_pipeline_gen(shm, &confirmed_gen, &confirmed_inst) != 0) {
        int saved_errno = errno;
        pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
        errno = saved_errno;
        return -1;
    }
    if (confirmed_gen != expected_gen || confirmed_inst != expected_inst) {
        pipeline_commit_exit(shm); /* Lost Wakeup 방지 */
        errno = ESTALE;
        return -1;
    }

    *out_gen = expected_gen;
    *out_inst = expected_inst;
    return 0;
}

static inline int is_trigger_satisfied(
    uint32_t mask, uint32_t req_mask, uint16_t mode)
{
    uint32_t ready = mask & TIMPANI_READY_MASK;
    if (req_mask == 0U || (req_mask & ~TIMPANI_READY_MASK) != 0U)
        return 0;
    if (mode == TIMPANI_TRIG_ALL)
        return (ready & req_mask) == req_mask;
    return 0; /* TIMPANI_TRIG_ANY is reserved and rejected in Phase 0. */
}

/* L1/L2 Source 공통 job_token 취득 — release snapshot 읽기 (O_RDONLY 가능, 무-VFS) */
int ttsched_current_job_token(uint32_t* job_token)
{
    if (!job_token) {
        errno = EINVAL;
        return -1;
    }
    if (!g_shm || !g_shm_pipeline_capable) {
        errno = ENOTSUP;
        return -1;
    }

    uint32_t gen_before, gen_after;
    uint64_t inst_before, inst_after;
    if (read_stable_pipeline_gen(g_shm, &gen_before, &inst_before) != 0)
        return -1;

    if (!g_name_cached) {
        prctl(PR_GET_NAME, g_my_name, 0, 0, 0);
        g_name_cached = 1;
    }

    uint32_t n_tasks = __atomic_load_n(&g_shm->n_tasks, __ATOMIC_ACQUIRE);
    int idx = -1;
    int snapshot_errno = 0;
    uint32_t released_token = 0U;
    if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS) {
        snapshot_errno = EINVAL;
    } else {
        idx = find_task_slot(g_shm, n_tasks, g_my_name);
        if (idx < 0) {
            snapshot_errno = ENOENT;
        } else {
            const volatile struct timpani_pipeline_slot* src =
                &g_shm->pipeline[idx];
            uint32_t required_mask = __atomic_load_n(
                &src->required_mask, __ATOMIC_ACQUIRE);
            uint16_t num_out_edges = __atomic_load_n(
                &src->num_out_edges, __ATOMIC_ACQUIRE);
            if (required_mask != 0U || num_out_edges == 0U ||
                num_out_edges > TIMPANI_MAX_OUT_EDGES) {
                snapshot_errno = EINVAL;
            } else {
                uint64_t state = __atomic_load_n(
                    &src->ready_state, __ATOMIC_ACQUIRE);
                if ((state & TIMPANI_SOURCE_RELEASE_VALID) == 0ULL) {
                    snapshot_errno = EAGAIN;
                } else {
                    released_token = ready_sequence(state);
                    if (state != make_source_release_state(released_token))
                        snapshot_errno = EINVAL;
                }
            }
        }
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (read_stable_pipeline_gen(g_shm, &gen_after, &inst_after) != 0)
        return -1;
    if (gen_after != gen_before || inst_after != inst_before) {
        errno = ESTALE;
        return -1;
    }
    if (snapshot_errno != 0) {
        errno = snapshot_errno;
        return -1;
    }
    if (g_shm_instance_id == 0ULL)
        g_shm_instance_id = inst_before;

    g_shm_generation = gen_before;
    g_task_idx = idx;
    *job_token = released_token;
    return 0;
}

/* ttsched_wait_data() 전용 읽기 전용 seqlock 스냅샷 (publication_guard 미기록) */
static inline int pipeline_read_dispatch_snapshot(
    const volatile struct timpani_ttsched_shm* shm,
    uint32_t* out_gen, uint64_t* out_inst,
    int* out_idx, uint32_t* out_dispatch_counter)
{
    uint32_t gen_before, gen_after;
    uint64_t inst_before, inst_after;

    if (read_stable_pipeline_gen(shm, &gen_before, &inst_before) != 0)
        return -1;

    uint32_t n_tasks = __atomic_load_n(&shm->n_tasks, __ATOMIC_ACQUIRE);
    if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (read_stable_pipeline_gen(shm, &gen_after, &inst_after) != 0 ||
            gen_after != gen_before || inst_after != inst_before) {
            errno = ESTALE;
            return -1;
        }
        errno = EINVAL;
        return -1;
    }

    int idx = (gen_before == g_shm_generation &&
               inst_before == g_shm_instance_id)
                  ? g_task_idx
                  : -1;
    if (idx < 0 || (uint32_t)idx >= n_tasks) {
        idx = find_task_slot(shm, n_tasks, g_my_name);
        if (idx < 0) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (read_stable_pipeline_gen(shm, &gen_after, &inst_after) != 0 ||
                gen_after != gen_before || inst_after != inst_before) {
                errno = ESTALE;
                return -1;
            }
            errno = ENOENT;
            return -1;
        }
    }
    if (strncmp((const char*)shm->tasks[idx].name, g_my_name, 15) != 0) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (read_stable_pipeline_gen(shm, &gen_after, &inst_after) != 0 ||
            gen_after != gen_before || inst_after != inst_before) {
            errno = ESTALE;
            return -1;
        }
        errno = EINVAL;
        return -1;
    }

    uint32_t cur = __atomic_load_n(
        &shm->pipeline[idx].dispatch_counter, __ATOMIC_ACQUIRE);

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (read_stable_pipeline_gen(shm, &gen_after, &inst_after) != 0)
        return -1;
    if (gen_after != gen_before || inst_after != inst_before) {
        errno = ESTALE;
        return -1;
    }

    *out_gen = gen_before;
    *out_inst = inst_before;
    *out_idx = idx;
    *out_dispatch_counter = cur;
    return 0;
}

/* Successor 대기 API — O_RDONLY(PROT_READ) 지원, FIFO 순차 소비 */
int ttsched_wait_data(uint32_t* job_token)
{
    if (!job_token) {
        errno = EINVAL;
        return -1;
    }
    if (!g_shm || !g_shm_pipeline_capable) {
        errno = ENOTSUP;
        return -1;
    }
    if (!g_name_cached) {
        prctl(PR_GET_NAME, g_my_name, 0, 0, 0);
        g_name_cached = 1;
    }

    uint32_t wait_gen = 0U;
    int has_waited = 0;
    int woken_by_futex = 0;

    for (;;) {
        uint32_t gen = 0U;
        uint64_t inst = 0ULL;
        int idx = -1;
        uint32_t cur = 0U;

        if (pipeline_read_dispatch_snapshot(
                g_shm, &gen, &inst, &idx, &cur) != 0) {
            return -1;
        }

        if (has_waited && gen != wait_gen) {
            errno = ESTALE;
            return -1;
        }

        if (g_shm_instance_id == 0ULL)
            g_shm_instance_id = inst;

        /* 최초 attach 또는 reinit에서 이미 관측한 현재값을 cursor로 사용한다.
         * Phase 0 정상 경로는 첫 dispatch 전에 ttsched_init()을 완료하므로
         * token 0은 init snapshot의 cur=0 뒤에 보존된다. 늦은 attach의
         * cur=N은 과거 token replay를 시작하지 않는다. */
        if (gen != g_shm_generation || !g_dispatch_counter_synced) {
            g_shm_generation = gen;
            g_task_idx = idx;
            g_last_dispatch_counter = cur;
            g_dispatch_counter_synced = 1;
        } else {
            g_task_idx = idx;
        }

        if (counter_ahead(cur, g_last_dispatch_counter)) {
            uint32_t token = g_last_dispatch_counter;
            /* FIFO 순차 소비: current - 1U 점프 시 하류 ready_sequence와의
             * 영구 ESTALE 교착을 방지한다. (cur - token > 1은 Phase 0
             * pending<=1 전제 이탈 backlog 경고 대상) */
            g_last_dispatch_counter = token + 1U;
            *job_token = token;
            return 0;
        }

        /* FUTEX_WAKE로 깨어났으나 dispatch_counter가 증가하지 않음 -> 제어 wake */
        if (woken_by_futex) {
            errno = ESTALE;
            return -1;
        }

        wait_gen = gen;
        has_waited = 1;
        long rc = syscall(SYS_futex,
                          (uint32_t*)&g_shm->pipeline[idx].dispatch_counter,
                          FUTEX_WAIT, cur, NULL, NULL, 0);
        if (rc == 0) {
            woken_by_futex = 1;
        } else if (errno == EAGAIN || errno == EINTR) {
            woken_by_futex = 0;
        } else {
            return -1;
        }
    }
}

int ttsched_signal_ready(uint32_t job_token)
{
    uint32_t gen;
    uint64_t inst;
    if (!g_shm || !g_shm_pipeline_capable) {
        errno = ENOTSUP;
        return -1;
    }
    if (!g_shm_writable) {
        errno = EACCES;
        return -1;
    }
    if (pipeline_commit_enter(g_shm, &gen, &inst) != 0)
        return -1;

    int result = -1;
    if (g_shm_instance_id == 0ULL)
        g_shm_instance_id = inst;

    uint32_t n_tasks = __atomic_load_n(&g_shm->n_tasks, __ATOMIC_ACQUIRE);
    if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS) {
        errno = EINVAL;
        goto out;
    }

    if (!g_name_cached) {
        prctl(PR_GET_NAME, g_my_name, 0, 0, 0);
        g_name_cached = 1;
    }
    if (gen != g_shm_generation) {
        g_shm_generation = gen;
        g_task_idx = -1;
        /* Wait-side state is invalidated; the next wait_data() reattaches at cur. */
        g_dispatch_counter_synced = 0;
    }
    if (g_task_idx < 0 || (uint32_t)g_task_idx >= n_tasks) {
        g_task_idx = find_task_slot(g_shm, n_tasks, g_my_name);
        if (g_task_idx < 0) {
            errno = ENOENT;
            goto out;
        }
    }
    /* 슬롯 신원(이름) 재확인 (n_tasks 범위 검증 이후 안전 접근) */
    if (strncmp((const char*)g_shm->tasks[g_task_idx].name,
                g_my_name, 15) != 0) {
        errno = EINVAL;
        goto out;
    }

    struct signal_edge_plan {
        uint16_t dst_slot_idx;
        uint16_t execution_model;
        uint32_t required_mask;
        uint32_t bit;
        uint64_t old_state;
        int do_wake;
    };

    const volatile struct timpani_pipeline_slot* src =
        &g_shm->pipeline[g_task_idx];
    uint16_t n_edges =
        __atomic_load_n(&src->num_out_edges, __ATOMIC_ACQUIRE);
    if (n_edges > TIMPANI_MAX_OUT_EDGES) {
        errno = EINVAL;
        goto out;
    }
    if (n_edges == 0U) {
        /* Sink 태스크(num_out_edges == 0): 신원·SHM 검증 통과 후 안전한 no-op */
        result = 0;
        goto out;
    }

    struct signal_edge_plan plans[TIMPANI_MAX_OUT_EDGES];

    /* 첫 변경 전에 전체 fan-out을 검증하고 preflight한다. */
    for (uint16_t i = 0; i < n_edges; i++) {
        uint16_t dst_idx = src->out_edges[i].dst_slot_idx;
        uint16_t bit_idx = src->out_edges[i].bit_idx;
        if (dst_idx >= TIMPANI_MAX_TASKS || dst_idx >= n_tasks ||
            bit_idx >= 16 || dst_idx == g_task_idx) {
            errno = EINVAL;
            goto out;
        }
        for (uint16_t prior = 0; prior < i; prior++) {
            if (plans[prior].dst_slot_idx == dst_idx) {
                errno = EINVAL;
                goto out;
            }
        }

        volatile struct timpani_pipeline_slot* dst =
            &g_shm->pipeline[dst_idx];
        uint32_t bit = (1U << bit_idx);
        uint32_t req = __atomic_load_n(&dst->required_mask,
                                       __ATOMIC_ACQUIRE);
        uint16_t mode = __atomic_load_n(&dst->trigger_mode,
                                        __ATOMIC_ACQUIRE);
        uint16_t model = __atomic_load_n(&dst->execution_model,
                                         __ATOMIC_ACQUIRE);
        if (mode != TIMPANI_TRIG_ALL || req == 0U ||
            (req & ~TIMPANI_READY_MASK) != 0U ||
            (req & bit) == 0U ||
            (model != TIMPANI_EXEC_TT && model != TIMPANI_EXEC_CBS)) {
            errno = EINVAL;
            goto out;
        }

        uint64_t state = __atomic_load_n(
            (const uint64_t*)&dst->ready_state, __ATOMIC_ACQUIRE);
        if (ready_sequence(state) != job_token) {
            errno = ESTALE;
            goto out;
        }
        if ((ready_mask(state) & bit) != 0U) {
            errno = EALREADY;
            goto out;
        }

        plans[i].dst_slot_idx = dst_idx;
        plans[i].execution_model = model;
        plans[i].required_mask = req;
        plans[i].bit = bit;
        plans[i].old_state = state;
        plans[i].do_wake = 0;
    }

    /* Preflight는 여기서 종료한다. 첫 CAS 이후에는 Phase 0 crash transaction을
     * 보장하지 않는다. */
    for (uint16_t i = 0; i < n_edges; i++) {
        uint32_t commit_gen;
        uint64_t commit_inst;
        if (read_stable_pipeline_gen(g_shm, &commit_gen, &commit_inst) != 0)
            goto out;
        if (commit_gen != gen || commit_inst != inst) {
            errno = ESTALE;
            goto out;
        }

        volatile struct timpani_pipeline_slot* dst =
            &g_shm->pipeline[plans[i].dst_slot_idx];
        uint64_t old_state = plans[i].old_state;

        while (1) {
            plans[i].do_wake = 0;
            uint32_t sequence = ready_sequence(old_state);
            if (sequence != job_token)
                break; /* guard 아래 reset 불가이므로 이미 commit됨. */

            uint32_t current_mask = ready_mask(old_state);
            if ((current_mask & plans[i].bit) != 0U)
                break; /* 동시 동일 호출이 이 bit를 설정했다. */

            uint32_t merged = current_mask | plans[i].bit;
            uint64_t next_state = make_ready_state(sequence, merged);
            if (is_trigger_satisfied(
                    merged, plans[i].required_mask, TIMPANI_TRIG_ALL)) {
                next_state = make_ready_state(sequence + 1U, 0U);
                plans[i].do_wake =
                    plans[i].execution_model == TIMPANI_EXEC_CBS;
            }
            if (__atomic_compare_exchange_n(
                    (uint64_t*)&dst->ready_state, &old_state, next_state,
                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                break;
            }
        }
    }

    /* 모든 edge CAS가 완료된 뒤에야 wake를 관측 가능하게 한다. */
    for (uint16_t i = 0; i < n_edges; i++) {
        if (!plans[i].do_wake)
            continue;
        uint32_t wake_gen;
        uint64_t wake_inst;
        if (read_stable_pipeline_gen(g_shm, &wake_gen, &wake_inst) != 0)
            goto out;
        if (wake_gen != gen || wake_inst != inst) {
            errno = ESTALE;
            goto out;
        }
        volatile struct timpani_pipeline_slot* dst =
            &g_shm->pipeline[plans[i].dst_slot_idx];
        __atomic_add_fetch((uint32_t*)&dst->dispatch_counter, 1U,
                           __ATOMIC_RELEASE);
        syscall(SYS_futex, (uint32_t*)&dst->dispatch_counter,
                FUTEX_WAKE, 1, NULL, NULL, 0);
    }
    /* TT target: Timer Master가 허용 slot에서 dispatch_counter를 연다. */
    result = 0;

out:
    {
        int saved_errno = result < 0 ? errno : 0;
        pipeline_commit_exit(g_shm);
        if (result < 0)
            errno = saved_errno;
    }
    return result;
}
```

- **`ttsched_wait_data()` 소비자 규약 요약**:
  1. `publication_guard`에 쓰지 않는 **읽기 전용 seqlock**(`pipeline_read_dispatch_snapshot()`)으로 스냅샷 전후 `read_stable_pipeline_gen()`(짝수 `generation > 0`, `magic`, ABI, `instance_id`, `WRITER_PENDING == 0`) 일치와 자기 Task 신원(`n_tasks` 선행 검증 포함)을 확인한다. 따라서 `O_RDONLY`(`PROT_READ`) 매핑에서도 `SIGSEGV` 없이 동작하며 소비자 크래시(`SIGKILL`)가 `publication_guard`를 영구 점유하지 않는다.
  2. `ttsched_init()`은 안정 SHM generation과 자기 Task 신원을 확인한 직후 해당 slot의 현재 `dispatch_counter`를 최초 attach cursor(`g_last_dispatch_counter`)로 snapshot한다. 첫 `wait_data()` fallback도 같은 `cur` 기준을 사용한다. 따라서 정상 Phase 0 경로는 모든 consumer가 최초 dispatch 전에 초기화해 `cur=0`에서 token `0`을 보존하고, 늦은 `cur=N` attach는 token `0..N-1`을 replay하지 않는다. `counter_ahead(cur, g_last_dispatch_counter)`가 참일 때 `current - 1U`로 점프하지 않고 **FIFO 순차 소비(`token = g_last_dispatch_counter; g_last_dispatch_counter = token + 1U; *job_token = token`)** 를 수행해 하류 `ready_sequence`와의 영구 `ESTALE` 교착을 방지한다. `cur - token > 1U`이면 Phase 0 정상 수용 조건(`pending <= 1`) 이탈로 관측·경고한다. restart/recovery payload 복원은 R-04 및 Phase 2(`REQ-DD-014`) 범위다.
  3. 미진행 상태에서는 `PROT_READ`에서도 커널이 기본 지원하는 `FUTEX_WAIT(&dispatch_counter, cur)`로 대기한다. `EAGAIN`/`EINTR` 시에는 스냅샷을 재읽기해 재평가하며, `FUTEX_WAKE`로 깨어났으나 세대가 바뀌었거나 `dispatch_counter` 증가가 없는 경우(게시/종료 control wake)에는 `ESTALE`을 반환한다.
  4. L2 CBS Task는 `ttsched_signal_ready()`의 `pipeline_commit_enter()` guard 안에서 packed `ready_state` CAS와 `dispatch_counter` 증가·futex wake를 수행한다. L1 TT Task는 `Timer Master`가 내부 경량 guard(`tm_pipeline_commit_enter()`)로 `ready_sequence > dispatch_counter`를 확인하고 허용 slot에서 `dispatch_counter`를 1 증가시킨다.

`signal_ready`의 성공은 전체 outgoing-edge 목록이 preflight를 통과하고 전달 token이 모든 destination과 일치했으며, 각 선행 비트가 이 호출에서 commit됐거나 동일 동시 호출에 의해 먼저 완료됐음을 뜻한다. CAS 재시도 중 같은 bit가 이미 설정되었거나 destination sequence가 앞선 경우 해당 edge는 수렴 완료로 처리한다. `pipeline_commit_enter()`의 모든 실패 롤백 분기는 `pipeline_commit_exit()`를 호출해 drain 대기 중인 게시자의 Lost Wakeup을 방지한다. publication guard가 정적 metadata·generation을 유지하고 edge별 CAS가 선행 경쟁을 보호한다. 모든 정적/preflight 실패는 첫 CAS 이전에 mutation-free이며, 모든 edge CAS가 끝나기 전에는 CBS `dispatch_counter`나 futex wake가 바뀌지 않는다.

첫 destination CAS가 시작된 뒤 Source process가 종료되면 일부 destination bit만 남을 수 있다. Phase 0은 이를 원자적 fan-out transaction으로 취급하지 않으며 자동 rollback, idempotent retry, quarantine 성공을 보장하지 않는다. 해당 chain run은 실패·시험 전제 이탈로 기록하고, commit marker·generation quarantine·crash recovery는 Phase 2 `REQ-DD-014`에서 설계한다. `pipeline_commit_enter()` 자체는 대기하지 않으며, 게시 대기는 게시자 쪽에서만 제한된 시간 수행한다.

---

## 4. L-DD-P0-03 — `Timpani-N` 사전 확인, 안정 게시 및 타이머 규율

### 4.1 비RT 노드 로컬 적합성 확인 (`main.cpp`)

테이블 적용 전에 비RT 경로에서 다음을 확인한다. 실패 시 활성 상태를 바꾸지 않고 거절한다.
- 공통 DAG metadata의 이름/해시, 선행 참조, 간선·용량 상한, Source별 전체 outgoing-edge 유일성/범위
- 모든 outgoing edge의 destination, predecessor bit assignment, trigger mode, TT/CBS model을 확인한다. 각 destination에서 incoming-bit OR-union은 `required_mask`와 같고 각 bit의 incoming edge는 정확히 하나이며 중복 incoming bit가 없어야 edge plan을 게시할 수 있다
- 각 Task의 공통 metadata 실행 계층이 TT entry 또는 CBS entry 중 정확히 하나와 일치
- TT entry의 CPU/slot/timing, CBS entry의 CPU/budget/period/deadline 표현 범위
- Source/Successor 역할과 실행 계층은 서로 독립이며 클래스가 강제로 바뀌지 않았는지 확인
- *(N은 DAG 또는 TT/CBS 스케줄 가능성을 재계산하지 않는다.)*

### 4.2 비RT 공통 SHM 그래프 및 계층 등록

1. SHM 게시자는 TT/CBS entry의 `(workload_id_hash, task_id_hash)` 쌍을 정규 key로 사용해 공통 `pipeline[]` 인덱스를 만들고, 해당 해시 쌍을 `pipeline[i].{workload_id_hash, task_id_hash}`에 기록한다. 이 해시 쌍은 **`Timpani-N` 내부 결합 검증 및 진단 도구 소비용**이며, Phase 0 `libttsched`는 `prctl(PR_GET_NAME)`과 `tasks[i].name`(노드 내 유일 `1..15`바이트 ASCII)으로 슬롯을 조회한다.
2. 각 pipeline slot의 `workload_id_hash`, `task_id_hash`, `execution_model`, `trigger_mode`, `required_mask`, `out_edges`를 초기화하고 `ready_state=0`, `dispatch_counter=0`을 설정한다. Source의 `ready_state=0`은 release-valid 표식이 없는 상태이며, 최초 실제 release authority가 `make_source_release_state(k)`를 기록할 때 token `k`가 유효해진다. Successor의 `ready_state=0`은 active `ready_sequence == 0`을 뜻한다. `pipeline_header`에는 신규 `instance_id`, `origin_epoch_ns`(schedule/diagnostic epoch), `hyperperiod_us`를 준비하되 이 값들은 `job_token` 계산에 사용하지 않는다.
3. 게시 전에 destination-indexed incoming-bit map과 슬롯 메타데이터(`n_tasks`, `dst == src` 자기 참조, `trigger_mode`, `execution_model`, `required_mask` 상위 16비트 오염 여부)를 함께 생성·검증한다.
   - 공통 DAG metadata의 predecessor key를 순회해 Successor의 직속 선행마다 서로 다른 bit position `0..15`를 배정한다. 이 배정으로 `required_mask`를 만들고 각 선행에 대응하는 `out_edge`를 하나 생성한다.
   - 고정 배열 `incoming_masks[TIMPANI_MAX_TASKS]`와 `incoming_edges[TIMPANI_MAX_TASKS]`를 0으로 초기화한다. 전체 슬롯 순회에서 `trigger_mode == TIMPANI_TRIG_ALL`, `execution_model ∈ {TIMPANI_EXEC_TT, TIMPANI_EXEC_CBS}`, `(required_mask & ~TIMPANI_READY_MASK) == 0U`를 확인하고, 전체 outgoing-edge 순회에서 `dst_slot_idx < n_tasks`, `dst_slot_idx != src`(자기 참조 금지), `bit_idx < 16`, destination `required_mask` 소속, Source/destination 간 중복 간선 부재를 확인한다. `incoming_masks[dst]`에 이미 같은 bit가 있으면 중복 incoming-bit assignment로 거절하고, 없으면 bit를 설정하고 `incoming_edges[dst]`를 증가시킨다.
   - 모든 활성 Source 순회 후 각 활성 Pipeline slot에 대해 `incoming_masks[i] == required_mask[i]`와 `incoming_edges[i] == popcount(required_mask[i])`를 요구한다. 따라서 `ALL`의 모든 required bit에는 정확히 하나의 incoming edge가 존재하며 edge 없는 required bit는 없다. bit alias, 여분 required bit, required-mask 밖 edge bit, 상위 16비트 오염은 게시 전에 거절한다. `required_mask == 0`인 Task는 `incoming_edges == 0`이어야 한다.

   ```c
   uint32_t incoming_masks[TIMPANI_MAX_TASKS] = {};
   uint16_t incoming_edges[TIMPANI_MAX_TASKS] = {};

   if (n_tasks == 0U || n_tasks > TIMPANI_MAX_TASKS)
       return EINVAL;
   for (uint16_t i = 0; i < n_tasks; i++) {
       if (pipeline[i].trigger_mode != TIMPANI_TRIG_ALL)
           return EINVAL;
       if (pipeline[i].execution_model != TIMPANI_EXEC_TT &&
           pipeline[i].execution_model != TIMPANI_EXEC_CBS)
           return EINVAL;
       if ((pipeline[i].required_mask & ~TIMPANI_READY_MASK) != 0U)
           return EINVAL;
   }
   for (uint16_t src = 0; src < n_tasks; src++) {
       uint16_t n_edges = pipeline[src].num_out_edges;
       if (n_edges > TIMPANI_MAX_OUT_EDGES)
           return EINVAL;
       for (uint16_t edge = 0; edge < n_edges; edge++) {
           uint16_t dst = pipeline[src].out_edges[edge].dst_slot_idx;
           uint16_t bit_idx = pipeline[src].out_edges[edge].bit_idx;

           if (dst >= n_tasks || dst == src || bit_idx >= 16)
               return EINVAL;
           for (uint16_t prior = 0; prior < edge; prior++) {
               if (pipeline[src].out_edges[prior].dst_slot_idx == dst)
                   return EINVAL;
           }
           uint32_t bit = 1U << bit_idx;
           if ((pipeline[dst].required_mask & bit) == 0U ||
               (incoming_masks[dst] & bit) != 0U)
               return EINVAL;
           incoming_masks[dst] |= bit;
           incoming_edges[dst]++;
       }
   }

   for (uint16_t dst = 0; dst < n_tasks; dst++) {
       uint32_t required = pipeline[dst].required_mask;
       if ((required & ~TIMPANI_READY_MASK) != 0U ||
           incoming_masks[dst] != required ||
           incoming_edges[dst] != __builtin_popcount(required))
           return EINVAL;
   }
   ```

   이 전체 테이블 검사는 N 게시 사전조건이다. RT `signal_ready()`의 Source-local plan만으로는 global bijection을 증명할 수 없으므로 게시 generation 수명 동안 검증된 불변 metadata를 사용한다.
4. TT Task는 기존 `TtSlot`/Timer Master에, CBS Task는 기존 `CbsConfig`/CbsState에 등록한다.
5. **Drain 상수 및 `publication_guard` 직렬화**:
   실제 게시 직전 `pipeline_header.publication_guard`에 `TIMPANI_PUB_WRITER_PENDING` 비트를 원자적으로 설정해 새 쓰기 commit 진입을 차단하고, 이미 시작된 활성 commit이 `0`이 될 때까지 제한된 시간 동안 `FUTEX_WAIT`로 대기한다. 마지막 reader는 `pipeline_commit_exit()`(실패 롤백 분기 포함)에서 `WRITER_PENDING`과 `active == 0`을 감지해 guard futex wake를 수행한다.

   | 상수명 | 값 | 적용 상황 | 타임아웃 초과 시 동작 |
   |:--|:--:|:--|:--|
   | `TIMPANI_PUB_DRAIN_TIMEOUT_US` | **`5,000 µs` (`5 ms`)**<br/>*(또는 `min(5 ms, hyperperiod_us / 4)`)* | 정상 게시 및 런타임 테이블 갱신 | `WRITER_PENDING` 즉시 해제 후 적용 거절/이연 (**in-place 덮어쓰기 금지**) |
   | `TIMPANI_PUB_MAX_DEFERS` | **`2`회** *(Phase 1+)* | 연속 drain 타임아웃 이연 상한 | 상한 초과 시 갱신 최종 실패(`TableApplied{success=false}`) 보고 |
   | `TIMPANI_PUB_SHUTDOWN_DRAIN_MS` | **`50 ms`** | 데몬 종료, SHM 객체 교체, 기동 시 잔존 SHM 정리 | 아래 **Safe Abandonment** 절차로 강제 무효화 |

6. **필드별 Release-Store 게시 순서 (`publication_guard` 덮어쓰기 금지)**:
   `struct timpani_pipeline_header`를 통째로 복사 대입하면 `WRITER_PENDING` 비트가 유효화 완료 전에 `0`으로 덮여 reader가 미완성 테이블에 진입할 수 있으므로, 반드시 아래 **필드별 `__atomic_store_n(..., __ATOMIC_RELEASE)`** 순서를 준수한다.

   ```c
   /* active_count == 0 확인 완료 및 TIMPANI_PUB_WRITER_PENDING 유지 상태 */
   __atomic_store_n(&shm->magic, 0U, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.magic, 0U, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->generation, odd_gen, __ATOMIC_RELEASE);

   /* tasks[], n_tasks, pipeline[] 갱신 (ready_state=0, dispatch_counter=0) */
   write_tasks_and_pipeline_slots(shm, candidate);

   /* pipeline_header 개별 필드 기록 (publication_guard 미접근) */
   __atomic_store_n(&shm->pipeline_header.abi_version,
                    TIMPANI_PIPELINE_ABI_VERSION, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.reserved, 0U, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.abi_size,
                    (uint32_t)sizeof(*shm), __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.instance_id,
                    shm_instance_id, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.origin_epoch_ns,
                    origin_epoch_ns, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.hyperperiod_us,
                    hyperperiod_us, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->pipeline_header.magic,
                    TIMPANI_PIPELINE_MAGIC, __ATOMIC_RELEASE);

   /* 짝수 generation -> 상위 magic -> 마지막에 WRITER_PENDING 해제 */
   __atomic_store_n(&shm->generation, even_gen, __ATOMIC_RELEASE);
   __atomic_store_n(&shm->magic, TIMPANI_TTSCHED_MAGIC, __ATOMIC_RELEASE);
   __atomic_and_fetch((uint32_t*)&shm->pipeline_header.publication_guard,
                      ~TIMPANI_PUB_WRITER_PENDING, __ATOMIC_RELEASE);
   ```

7. **종료/교체 시 강제 무효화 절차 (Safe Abandonment)**:
   데몬 종료·SHM 객체 교체·기동 시 잔존 SHM 정리 경로에서 `TIMPANI_PUB_SHUTDOWN_DRAIN_MS` (`50 ms`)가 경과해도 `active_count > 0`(예: writer 프로세스가 `pipeline_commit_enter`와 `exit` 사이에서 `SIGKILL` 종료)이면 잔여 `active_count`를 경고 로그로 남긴 뒤 다음 순서로 강제 무효화한다:
   1. `TIMPANI_PUB_WRITER_PENDING`을 **유지**한 채 `shm->magic = 0`과 `shm->pipeline_header.magic = 0`(`__ATOMIC_RELEASE`)을 기록한다.
   2. 구 SHM 객체에 `ftruncate()`나 `pipeline[]` in-place 덮어쓰기를 **절대 수행하지 않는다**(생존 reader의 `SIGBUS` 및 상태 손상 차단).
   3. 대기 중인 소비자를 깨우기 위한 종료 control wake(`dispatch_counter` 미증가 `FUTEX_WAKE`) 후 `shm_unlink(TIMPANI_TTSCHED_NAME)`으로 이름 공간만 회수한다. 교체·재기동 시에는 `O_CREAT | O_EXCL`로 새 inode와 새 `instance_id`를 가진 전체 크기 SHM 객체를 생성한다.

   Safe Abandonment는 active writer를 원자적으로 취소하거나 실행 중인 스레드를 중단시키는 연산이 아니다. 구 mapping에 이미 진입한 writer가 50 ms 초과 후 resume될 수 있으므로, writer는 각 destination CAS와 각 wake 직전에 `magic`/generation/`instance_id`를 재확인하고 무효화가 관측되면 `pipeline_commit_exit()` 후 `ESTALE`로 중단한다. 마지막 재확인과 이후 CAS/wake 사이의 짧은 race까지 제거하는 cancellation guarantee는 Phase 0에 없다.

   invalidation+unlink 이후의 late CAS/wake는 **새 SHM object로 전달될 수 없으며**, 구 object를 보유한 consumer도 wake 후 stable-state 재검증에서 `magic=0`/`instance_id` 불일치를 보고 `ESTALE`를 반환한다. 따라서 Phase 0 보장은 "구 mapping의 연산이 무조건 취소됨"이 아니라 "구 object가 재사용되지 않고 late operation이 active generation 실행으로 소비되지 않음"이다. 무효화 전에 `ttsched_wait_data()`가 token을 반환받아 이미 실행 중인 Job은 Safe Abandonment가 취소하지 않는다. 정상 운영 중 publisher 종료/교체 없이 writer가 멈춘 active-count 누수는 자동 Safe Abandonment trigger가 아니며 전제 이탈/Phase 2 recovery 대상이다.

게시 자체는 `ready_state`나 `dispatch_counter`를 증가시키지 않는다. Source release stamp는 게시가 아니라 실제 L1/L2 release 경로에서만 수행한다. 이 순서는 Phase 0의 최초 게시·종료/무효화에서도 `pipeline_commit_enter()`와 읽기 전용 seqlock이 미완성 상태를 관측하는 동시 구간을 제거한다. Phase 1의 Pipeline 포함 정상 런타임 갱신은 동일 guard와 `TIMPANI_PUB_DRAIN_TIMEOUT_US` / `TIMPANI_PUB_MAX_DEFERS`를 경계별 커밋에 재사용하며, Phase 2는 publish 실패·rollback·재시작 시 잔여 상태 초기화 정책을 추가한다.

### 4.3 RT TT/CBS 실행 어댑터

- **L1 TT Source 또는 일반 TT:** `required_mask == 0`이면 기존 slot 발화와 동일하게 `update_current_slot()`·`next_tt_start`를 갱신한다. Pipeline Source(`num_out_edges > 0`)는 Timer Master가 내부 schedule cycle ordinal `k`를 `pipeline[i].ready_state = make_source_release_state(k)`로 release-store한 뒤 `tasks[i].counter` 증가·futex wake를 수행한다. Pipeline `dispatch_counter`는 사용하지 않는다. 일반 TT Task는 Source stamp 없이 기존 wake를 유지한다. Timer Master는 `k`를 schedule cursor에서 얻고 wall-clock 나눗셈이나 앱-local counter를 사용하지 않는다.
  ```c
  if (tm_pipeline_commit_enter(shm, &gen) == 0) {
      __atomic_store_n(&pipeline[i].ready_state,
                       make_source_release_state(k), __ATOMIC_RELEASE);
      __atomic_add_fetch(&tasks[i].counter, 1U, __ATOMIC_RELEASE);
      futex_wake(&tasks[i].counter, 1);
      tm_pipeline_commit_exit(shm);
  }
  ```
  stamp가 실패하거나 세대가 바뀌면 `counter` 증가·wake 없이 guard를 해제한다.
- **L1 TT Successor (`Timer Master` 경량 guard 및 선행 `pending` 검사):**
  - `Timer Master`는 SHM 객체의 유일한 생명주기 소유자이므로 RT 슬롯 루프에서 `shm_open`/`fstat`/`close`를 호출하지 않고, 내부 경량 guard **`tm_pipeline_commit_enter(shm, &gen)`**(순수 원자 `magic`/`generation`/`WRITER_PENDING` 확인 + `publication_guard` 증가, 실패 롤백 시 `pipeline_commit_exit()` 호출)을 사용한다.
  - L1 TT Successor의 슬롯 시각이 도래하면 먼저 후속 L1 TT 슬롯 보호를 위해 **`next_tt_start`를 갱신**한 뒤 `tm_pipeline_commit_enter()`를 획득해 `pending = counter_ahead(ready_sequence(ready_state), dispatch_counter)`를 확인한다.
  - **`pending == true`인 경우에만** ① `update_current_slot(cpu, slot_idx)`로 `current_slot_map[cpu]` 활성화, ② `pipeline[idx].dispatch_counter`를 정확히 `1` 증가(`__ATOMIC_RELEASE`), ③ `FUTEX_WAKE(&pipeline[idx].dispatch_counter, 1)`, ④ `scx_bpf_kick_cpu(cpu)`를 수행한 뒤 `pipeline_commit_exit(shm)`로 guard를 해제한다.
  - **`pending == false`이거나 guard 획득에 실패한 경우**에는 `tasks[i].counter`와 `dispatch_counter`를 모두 증가시키지 않으며, **`current_slot_map[cpu]`를 활성화하지 않고(`SLOT_NONE` 유지) `scx_bpf_kick_cpu()`도 생략**한다. 이를 통해 정상적인 부분 입력 대기(`UC-DD-02`) 구간에서 [DDR-005](../fundamental-design/DDR-005-sched-ext-bpf-scheduler.md)/[DDR-007](../fundamental-design/DDR-007-tt-cbs-integrated-scheduling.md)의 "활성 TT 슬롯 내 미준비 태스크 missing-task 장애" 오탐을 방지하고 불필요한 CPU kick을 제거하며, `next_tt_start`는 여전히 후속 L1 슬롯을 보호한다.
- **L2 CBS Source 또는 일반 CBS:** 기존 release 경로를 유지한다. Pipeline Source(`num_out_edges > 0`)는 009 release adapter가 공통 schedule activation ordinal `k`를 `pipeline[i].ready_state`에 stamp한 뒤 Task를 wake한다. Phase 0에서는 하나의 fixed Hyperperiod 단위 common ordinal만 허용하며, release 내부의 상세 입력 식별자 확장은 이번 009 내부 계약에서 별도로 정의하지 않는다.
- **L2 CBS Successor:** `signal_ready`의 commit guard 안에서 packed `ready_state` CAS로 해당 `ALL`을 확정한다. 모든 outgoing-edge CAS가 완료된 뒤 `dispatch_counter`를 1 증가시키고 해당 futex wake를 수행한다. BPF는 wake된 thread가 Runnable이 된 뒤 기존 CBS CPU·budget·Gap 규칙만 적용하며, OI-DD-03이 별도 CBS DSQ나 BPF release API를 추가하지 않는다.

Phase 0 L1 slot에 ready가 늦게 도착하면 해당 slot에서는 `current_slot_map` 활성화 및 wake를 생략하고 pending ready를 유지한다. 다음 TT slot에서 ready sequence를 1회 소비한다. 정상 Phase 0 시험은 modulo 비교 결과 pending ready가 최대 1개인 것을 전제로 하며, 2개 이상이면 backlog/slot-miss 시험 전제 이탈로 기록한다. pending 만료, deadline miss 처리, `Activation Window`, `SKIP`는 Phase 1/2에서 결정한다.

| 경로 | 소유 주체 | 허용 | 금지 |
|:---|:---|:---|:---|
| **L1 Source** | Timer Master | source `ready_state`에 release ordinal stamp 후 `tasks[i].counter` slot wake | wall-clock token 계산, Pipeline pending 확인 |
| **L2 Source** | Source release adapter | source `ready_state`에 release ordinal stamp 후 기존 release/wake | 앱-local 또는 wall-clock token 계산 |
| **L1 Successor adapter** | Timer Master | 경량 guard(`tm_pipeline_commit_enter`)로 `pending` 확인 후 `current_slot_map` 활성화·`dispatch_counter++`·`kick_cpu` | `pending == false` 시 `current_slot_map` 활성화·`kick_cpu`·wake, RT VFS 호출, `Window` 적용 |
| **L2 Successor adapter** | `signal_ready` | 전체 outgoing-edge CAS 집합에서 `ALL` 형성 후 `dispatch_counter++` | CAS 전 wake, TT slot 대기, 별도 release daemon |
| **CBS CPU execution** | `scx_timpani` | Runnable 후 기존 budget/Gap gate | Data-Ready 그래프 계산 |
| **libttsched** | Application API | 무-VFS 원자 commit·읽기 전용 seqlock·dispatch futex | RT `shm_open`/`fstat`, `O_RDONLY` 쓰기, control wake 실행 소비 |
| **BPF** | Kernel scheduler | 기존 TT/CBS DSQ, budget/slot 규칙 | 별도 Data-Driven DSQ |

### 4.4 `scx_timpani` 계층별 집행

- TT Successor는 `dispatch_counter` futex wake 전에는 Runnable이 되지 않는다. Timer Master가 `pending ready`를 확인해 슬롯을 활성화하고 wake한 뒤 BPF는 기존 `SCHED_TYPE_TT` 경로로 집행한다.
- CBS Successor는 `ALL` commit wake로 Runnable이 되고 기존 `SCHED_TYPE_CBS` 경로에 enqueue된다. `ALL`은 release 조건만 추가하며 BPF의 budget/Gap/TT active 판정을 바꾸지 않는다.
- TT/CBS 공통으로 `DMISS`, budget, trace 판단은 기존 Task 계층 규칙을 사용한다. Data-Ready 자체를 scheduler fault 또는 BPF dispatch queue로 취급하지 않는다.

---

## 5. L-DD-P0-04 — `Timpani-O` 후보 검증 및 결정적 위상 배치 (`global_scheduler.{h,cpp}`)

1. **확정 모델 보호·Task class 해석**: 후보를 먼저 구성해 검증한다. 각 Task는 `TaskInfo.temporal_class`가 있으면 그 명시값을 사용하고 없으면 `SchedInfo.temporal_class`를 상속한다. resolved class가 `predecessors`나 Source/Successor 역할로 바뀌지 않는지 검증한다. 거절 시 확정 모델을 유지한다.
2. **Task 참조 정규화 및 결정적 DAG 검증 (`ValidateAndSortDag`)**:
   - 내부 식별자는 `TaskNameKey = std::pair<std::string, std::string>` `(workload_id, task_id)`로 통일한다. `std::map`과 사전순 `std::set`만 사용한다(`std::unordered_map` 금지).
   - `TaskReference.workload_id`가 빈 문자열이면 현재 `SchedInfo.workload_id`로 채운다. Phase 0에서는 정규화 결과가 현재 workload와 다르면 `InfeasibleReason::InvalidDag`로 거절한다.
   - Task name(`task_id`)은 기존 SHM lookup 호환을 위해 노드 단위로 유일해야 하며 길이는 `1..15`바이트 ASCII다(`REQ-DD-001`, `ASM-DD-01`). 정규화 Task key 중복, 노드 내 `task_id` 중복/길이 위반, 자기 참조, 미존재 참조, **동일 Task의 `predecessors` 배열 내 중복 선행 선언(예: `[CAM, CAM]` — 암묵적 중복 제거 금지)**, `(fnv1a(workload_id), fnv1a(task_id))` hash 쌍 충돌을 모두 `InfeasibleReason::InvalidDag`로 거절한다.
   - 직속 선행 최대 `16`개와 정점별 후행 간선 최대 `8`개를 확인한다. 사전순 ready queue 기반 Kahn's Algorithm 종료 후 `out_topo_rank.size() != task_index.size()`이면 순환 의존성으로 거절한다.
3. **클래스 독립 테이블 생성 (`build_table`)**:
   - `PipelineTaskConfig`는 실행 계층 필드를 갖지 않고 `PipelineTaskKey(workload_id_hash, task_id_hash)`와 선행 key 목록만 갖는다. 각 Successor의 정규화 선행 key를 결정적으로 정렬하고 오름차순으로 서로 다른 bit position을 배정한다. 선행 수가 `n`이면 생성 `required_mask`는 정확히 `(1U << n) - 1U`이고 `n == 0`이면 `0`이다. TT/CBS entry는 resolved class에 따라 별도로 만든다.
   - L1 Task는 입력 timing으로 `TtSlot`을, L2 Task는 입력 timing으로 `CbsConfig`를 만든다. Source/Successor는 이 분기를 결정하지 않는다. 배정한 predecessor/destination 쌍마다 outgoing edge를 하나 생성하고, 후보를 반환하기 전 §4.2와 같은 incoming-mask exact equality 검사를 실행한다.
   - O는 DAG 위상 순서를 TT slot 충돌 검사 또는 CBS admission 검사의 tie-breaker로만 사용한다.
4. **Phase 0 배치와 이관**:
   - 같은 workload 안에서 명시/상속 class가 혼합된 Task를 지원하고 기존 TT placement 및 CBS admission/worst-fit을 각각 사용한다.
   - Phase 1은 cross-workload `TaskReference`(서로 다른 workload 간 `task_id` 충돌 해소 규칙 포함, `OI-DD-06`)와 혼합 DAG의 TT Window/CBS Gap·예산 지연, `E2E` 검증, Pipeline 갱신으로 확장한다. `E2E` 필드는 Phase 0 OI-DD-01 protobuf에 포함하지 않는다.

---

## 6. Protobuf 인터페이스 최소 확장안 (`OI-DD-01`, 인터페이스 승인)

> **주의**: 실제 `.proto` 파일 반영은 `OI-DD-01` 승인 범위와 구현 병합 게이트를 준수해 수행한다(`CON-DD-05`).

```protobuf
// timpani-o/proto/schedinfo.proto (package schedinfo.v1)
message TaskReference {
  // Phase 0: empty means the enclosing SchedInfo.workload_id.
  string workload_id = 1;
  string task_id = 2;
}

message TaskInfo {
  // ... existing fields 1..10 ...
  // If absent, inherit SchedInfo.temporal_class.
  optional TemporalClass temporal_class = 11;
  repeated TaskReference predecessors = 12;
}

// timpani-o/proto/node_control.proto (package timpani.node.v1)
enum PipelineTriggerMode {
  PIPELINE_TRIGGER_ALL = 0;
}

message PipelineTaskKey {
  uint64 workload_id_hash = 1;
  uint64 task_id_hash = 2;
}

message PipelineTaskConfig {
  PipelineTaskKey task_key = 1;
  repeated PipelineTaskKey predecessor_keys = 2;
  PipelineTriggerMode trigger_mode = 3;
}

message HierarchicalScheduleTable {
  // ... existing fields 1..5 ...
  repeated PipelineTaskConfig pipeline_tasks = 6;
}
```

권고 field 번호 `11`, `12`, `6`은 현재 schema의 빈 번호를 사용한 것이며 실제 반영 전에 인터페이스 승인으로 확정한다. `SchedInfo.temporal_class`는 기존 workload 기본값으로 유지한다. O는 `TaskInfo.temporal_class`의 presence를 확인해야 하므로 단순 enum 대신 `optional`을 사용한다. Phase 0에서 trigger enum의 유효값은 `PIPELINE_TRIGGER_ALL`뿐이다.

TT/CBS entry 자체에는 dependency를 중복하지 않는다. O→N DAG metadata는 `PipelineTaskConfig`에만 두고, N은 key 쌍을 `TtSlot` 또는 `CbsConfig` 중 정확히 하나와 연결한다. node-level SHM 이름 조회는 `REQ-DD-001` 및 `ASM-DD-01`의 `1..15`바이트 ASCII `task_id` 노드 유일성 제약을 Phase 0에도 유지한다.

---

## 7. L-DD-P0-05 — 시험 접점

단위 시험은 실행 계층 추론 없이 DAG를 처리하고, L1/L2 연결을 계층별 gate로 실행하는지 검증한다.

| 시험 | 확인 |
|:---|:---|
| class 해석 | Task 명시 class는 workload 기본값보다 우선하고, 미지정 Task는 기본값을 상속한다. `predecessors` 유무는 class에 영향을 주지 않는다 |
| 참조 정규화/거절 | 빈 `workload_id`는 현재 workload로 해석되고, Phase 0 cross-workload 참조·중복 key·노드 내 `task_id` 중복/`1..15`B ASCII 위반·동일 Task `predecessors` 중복(`[CAM, CAM]`)·hash-pair 충돌·미존재 참조는 활성화 전에 거절된다 |
| O DAG/배치 | 정규화 key, 위상 순서, TT/CBS 배치가 입력 순서와 무관하며 실행 class는 명시/상속값과 일치한다 |
| N 등록 | `(workload_id_hash, task_id_hash)` 쌍이 정확히 하나의 TT/CBS entry와 연결되고 클래스가 바뀌지 않는다. `n_tasks` 범위·`dst == src` 자기 참조·`trigger_mode`·`execution_model`·`required_mask` 상위 16비트 오염을 거절하며, 모든 활성 slot에서 incoming-bit OR-union이 `required_mask`와 같고 `incoming_edge_count == popcount(required_mask)`다 |
| incoming-bit bijection | 선행 2개 Successor의 bit `0`, `1` 및 `required_mask=0b11` 정상 후보는 승인한다. 별도로 두 incoming edge가 모두 bit `0`, 세 번째 edge 없이 mask가 `0b111`, mask 밖 edge bit, 상위 16비트 오염(`0x10003`)인 경우를 거절하고 committed candidate/active SHM 상태가 불변인지 확인한다 |
| SHM ABI 및 초기화 | 비RT `ttsched_init()`에서 `fstat()` < 784B 거절(`EINVAL`), 784B legacy/짧은 매핑에서는 TT API만 동작하고 Pipeline API는 extension 접근 없이 `ENOTSUP`; magic/ABI-size 불일치 `EINVAL`, magic이 유효한 미지원 version은 `ENOTSUP`; `mmap()` 직후 fd 즉시 close 및 스레드 종료 시 `pthread_key` destructor로 `munmap` 누수 `0`건 확인 |
| 무-VFS 교체/종료 감지 | RT 경로(`signal_ready`, `wait_data`, `current_job_token`, `Timer Master`)에서 `shm_open`/`fstat`/`close` 호출 `0`회 확인. Safe Abandonment(`WRITER_PENDING` 유지 + `shm->magic=0` + `pipeline_header.magic=0` + `shm_unlink` + `O_CREAT|O_EXCL` 신규 `instance_id`) 또는 `instance_id` 변경 시 기존 매핑의 RT 호출은 새 `pipeline[]`에 접근하거나 실행하지 않고 `ESTALE`을 반환하며, 비RT `ttsched_init()` 재초기화 후 새 객체에 정상 연결됨을 확인한다. writer를 CAS/wake 직전에 50 ms 이상 pause하고 old object가 abandon된 뒤 resume시키면 신 object 변경 `0`, old consumer 실행 `0`, old API `ESTALE`이어야 한다. 마지막 재검증 직후 pause/resume race에서 old object late CAS/wake 자체는 허용하되 active generation에 영향을 주지 않는다 |
| 권한 및 읽기 전용 seqlock | Pipeline writer의 `O_RDWR` 실패는 `EACCES`, `O_RDONLY`(`PROT_READ`) Successor의 `ttsched_wait_data()` 및 `ttsched_current_job_token()`은 `publication_guard` 쓰기·`SIGSEGV` 없이 성공, `O_RDONLY`의 `ttsched_signal_ready()`는 `EACCES`. `O_RDWR` Sink(`num_out_edges == 0`)의 `signal_ready()`는 신원 검증 후 no-op `0` 반환. 배포 manifest에서 `timpani-pub` member가 신뢰 writer 목록과 일치하고 비신뢰 workload가 RO group에만 속하는지 검증한다. RW writer의 다른 slot/header 임의 변경 격리는 Phase 0 보장으로 시험하지 않는다 |
| 공통 `job_token` 취득 | Timer/release authority가 Source slot에 `make_source_release_state(k)`를 stamp한 경우에만 `ttsched_current_job_token()`이 `k`를 반환한다. release-invalid 상태는 `EAGAIN`, Source가 아닌 slot이나 손상 state는 `EINVAL`, 세대 변경 중에는 `ESTALE`이다. 앱 wake를 Hyperperiod 경계 뒤로 미루거나 `CLOCK_REALTIME`을 forward/backward로 변경해도 반환 token과 downstream `ready_sequence`는 변하지 않는다. 32비트 activation ordinal의 `0xFFFFFFFF → 0` 경계는 wrap 비교 규칙과 동일하게 Source/Successor 모두 같은 cycle에 매핑되어야 한다 |
| fan-out 사전검증 | 다중 edge Source에서 `n_tasks` 경계 오염 및 뒤쪽 edge 손상(`dst_idx`, `bit_idx`, `required_mask` 상위비트/소속, `trigger_mode`, `execution_model`, 중복 destination, `num_out_edges` overflow)시켜 `signal_ready`를 호출한다. `-1`/`EINVAL`, 첫 CAS 전 전체 fan-out의 mask·sequence·dispatch counter·futex wake 변경 `0`건이어야 한다. 뒤쪽 destination의 preflight token을 `ESTALE` 또는 `EALREADY`로 만든 경우에도 앞쪽 유효 destination은 불변이다. 유효 fan-out에서는 모든 CAS가 끝난 뒤 첫 CBS wake가 발생한다. 별도 failpoint로 destination CAS 사이·CAS 직후·첫 wake 전후 Source를 종료하면 Phase 0가 rollback/retry 성공을 보장하지 않으며 partial state를 관측·기록하고 chain run을 실패 처리한다 |
| payload visibility/retention | Producer가 token-addressed payload에 generation marker와 pattern을 기록한 뒤 `signal_ready()`를 호출하면 `wait_data(token)` 이후 Consumer가 동일 token/generation/pattern을 읽는다. `signal_ready()` 실패 시 해당 payload는 새 dispatch로 승인되지 않으며 활성 token storage는 덮어쓰이지 않는다. Phase 0 pending `<= 1`에서 최소 2-slot PoC를 사용해 Consumer 처리 중 Producer가 승인된 slot을 overwrite하지 않음을 반복 검증한다. payload 위치·ack·최종 reuse 시각은 애플리케이션 계약이며 TIMPANI API가 소유하지 않는다 |
| 동시성 및 drain/Lost Wakeup | 64비트 `ready_state` CAS가 target에서 lock-free이고, 마지막 선행 CAS는 mask reset+sequence 증가를 한 번에 수행하며 concurrent predecessor 경합에 wake 1회다. 동일 동시 `signal_ready`가 preflight된 bit를 설정하거나 sequence를 증가시키면 중복 wake 없이 수렴 완료(`0`)로 본다. `pipeline_commit_enter()` 실패 롤백(`WRITER_PENDING`/세대 불일치) 시에도 `pipeline_commit_exit()`가 drain 대기 게시자를 깨워 Lost Wakeup이 `0`건이며, `5 ms` 정상 이연 및 `50 ms` 종료 Safe Abandonment가 동작함을 검증한다 |
| 주기 신원 및 FIFO 소비 | 활성 token `k`에서 A가 bit를 설정한 뒤 중복 `(A,k)`는 `EALREADY`, `A(k+1)`/`B(k+1)`은 `ESTALE`이며 mask/sequence/dispatch는 불변이다. preflight 통과 후 CAS 경합 중 동일 동시 호출로 bit가 설정되거나 sequence가 전진한 경우는 수렴 완료(`0`)이며, sequence 증가 이후 늦게 도착한 이전 token 호출은 preflight에서 `ESTALE`로 거절한다. 첫 dispatch 전에 attach한 consumer(`init cursor=0`)는 token `0`을 FIFO로 받고, `dispatch_counter=N` 이후 attach한 consumer(`init cursor=N`)는 token `0..N-1`을 replay하지 않고 다음 dispatch만 기다린다. `cur - cursor > 1`이면 backlog 전제 이탈로 관측한다 |
| wake 계약 | `dispatch_counter` 증가만 실행 wake로 취급하고, publication/shutdown control wake는 `ESTALE`; `0x7fffffff` 경계에서 modulo 비교가 중복/누락 없이 동작 |
| L1 gate | `ALL` 전 또는 `pending ready == false`일 때 `dispatch_counter` 증가 `0`·`current_slot_map` 미활성화(`SLOT_NONE`)·`kick_cpu` 생략(`next_tt_start`는 갱신); ready가 slot 전에 있으면 해당 slot에서 `current_slot_map` 활성화 및 `dispatch_counter` `+1`·wake; 같은 ready의 중복 wake `0`; slot 후 ready는 다음 slot까지 pending |
| L2 gate | packed `ALL` CAS 뒤 즉시 `dispatch_counter+1`과 futex wake `1`; 실제 CPU 실행은 CBS budget/Gap에 따라 지연 가능 |
| CBS BPF 경계 | TT active 또는 budget=0일 때 CBS Task가 즉시 dispatch되지 않지만 release 자체는 유지되고 기존 throttled/replenishment 경로로 집행 |
| 조합 | Source/Successor 역할을 섞어 TT→TT, TT→CBS, CBS→TT, CBS→CBS 각 최소 1회 통과 |

SRS-009 §5.1의 boundary matrix는 위 시험 행을 다음과 같이 역추적한다.

| BM | LLD 시험 접점 | 최소 관측 필드 |
|:---|:---|:---|
| **BM-DD-P0-01** | `공통 job_token 취득`, `wake 계약` | source release state, token, downstream ready sequence, `clock_gettime` call count |
| **BM-DD-P0-02** | `주기 신원 및 FIFO 소비`, `SHM ABI 및 초기화` | `dispatch_counter`, init cursor, first returned token, replay count |
| **BM-DD-P0-03** | `fan-out 사전검증`, `동시성 및 drain/Lost Wakeup` | failpoint, destination별 mask/sequence, wake count, partial-state/disposition |
| **BM-DD-P0-04** | `payload visibility/retention` | payload token/generation/pattern, producer reuse event, consumer completion |
| **BM-DD-P0-05** | `권한 및 읽기 전용 seqlock` | UID/GID/ACL, allowlist membership, API `errno` |
| **BM-DD-P0-06** | `무-VFS 교체/종료 감지`, `wake 계약` | pause/resume point, old/new instance, late CAS/wake, old API result |
| **BM-DD-P0-07** | `fan-out 사전검증`, `동시성 및 drain/Lost Wakeup` | fan-out=8, concurrent writers, latency distribution, wake order/count, timeout/`EAGAIN` |

타깃 미실행 시 하드웨어 검증은 미수행으로 기록한다.
