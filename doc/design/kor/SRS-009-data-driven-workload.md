<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# SRS-009: Data-Driven Workload — Phase 0 소프트웨어 요구사항 명세

**작성일:** 2026-10-27  
**최종 수정:** 2026-11-20 (Phase 0 내부 기술 리뷰 `I-01`~`I-09` 보완 반영)  
**상태:** Draft — Phase 0 조건부 설계 승인 (`OI-DD-01` 인터페이스 승인, `OI-DD-02`/`OI-DD-03` 조건부 승인; 구현 완료 또는 기능안전 운용 승인 아님)  
**상위 기준:** [SRS-000](../fundamental-design/SRS-000-timpani-system.md) §5.1 `REQ-DD-001`, [REQ-009](REQ-009-data-driven-workload.md) §1.5, §6, §7.1, §8.2, §9

**연계 문서:** [HLD-009](HLD-009-data-driven-workload.md), [DDR-009](DDR-009-data-driven-workload.md), [LLD-009](LLD-009-data-driven-workload.md), [REQ-011](../DDR-011-runtime-table-update/REQ-011-runtime-table-update.md), [DDR-011](../DDR-011-runtime-table-update/DDR-011-runtime-table-update.md)

**용어 기준:** [REQ-009](REQ-009-data-driven-workload.md) §2 및 [DDR-012](../fundamental-design/DDR-012-fault-management.md) §2

---

## 1. 목적과 적용 규칙

본 문서는 [REQ-009](REQ-009-data-driven-workload.md)에 정의된 Data-Driven Workload의 **Phase 0(기본 체인 동작 확인)** 달성에 필요한 소프트웨어 요구사항만 명세한다. Phase 1(`Activation Window` 통제, 오프라인 E2E 검증, Pipeline 포함 테이블의 정상 런타임 갱신) 및 Phase 2(입력 누락 `SKIP`, `DATA_TIMEOUT` 보고, 비정상 상태 안전 초기화) 요구사항은 본 문서로 자동 승계하지 않는다.

요구사항 ID는 전체 단계 요구사항(`REQ-DD-###`)과 구분되도록 `REQ-DD-P0-F-###`(기능) 및 `REQ-DD-P0-N-###`(비기능) 접두어를 사용한다. 표의 모든 활성 항목은 Phase 0 범위에서 **SHALL**(필수)이다. 복합 단계 상위 요구사항(`REQ-DD-001`, `002`, `009`, `015`, `013`)에 대한 매핑은 **[REQ-009](REQ-009-data-driven-workload.md) §1.5.3의 Phase 0 적용 부분에만 한정**되며, 해당 상위 ID 전체의 완료를 뜻하지 않는다.

---

## 2. Phase 0 시험 경계

| 구분 | Phase 0 조건 |
|:---|:---|
| **실행 대상** | 단일 노드에서 원래 `TemporalClass`가 L1 TT 또는 L2 CBS인 Task를 동일 DAG에 연결. DAG 역할로 실행 계층을 변경하지 않음 |
| **체인 구성** | 동일 주기 기본 체인과 L1/L2 혼합 Task 조합을 사용. 각 Task는 기존 계층의 TT timing 또는 CBS timing을 갖는다 |
| **시간·상태 기준** | 고정 `E/H/table`. `ALL`은 공통 ready를 만든다. L1은 허용 TT slot에서, L2는 CBS release/gate에서 실행한다. ready 상태는 원자적으로 1회 소비한다 |
| **초기 attach 경계** | 정상 Phase 0 시험에서는 최초 Source dispatch 전에 모든 Successor가 `ttsched_init()`을 완료한다. 초기화 시 현재 `dispatch_counter`가 attach cursor가 되며 `cur>0` late attach는 attach 이전 token을 replay하지 않는다 |
| **Payload 소유·수명** | payload는 애플리케이션 소유다. Producer는 token-addressed payload publish 완료 후에만 `signal_ready(token)`을 호출하고, Consumer 처리 완료 전 overwrite하지 않는다. 009는 payload 위치·구조·ack API를 정의·검증하지 않는다 |
| **SHM writer trust boundary** | `O_RDWR`는 전체 `/timpani_ttsched` object mapping이다. Phase 0의 `timpani-pub`은 상호 신뢰된 cooperative writer만 포함하며, 비신뢰 workload는 read-only group만 사용한다. slot/header 단위 악의적 writer 격리는 보장하지 않는다 |
| **런타임 갱신과의 경계** | 시험 중 **Pipeline 포함 테이블의 런타임 갱신(`UPD-REPLACE / ADD / REMOVE / CLEAR`)은 수행하지 않음**(Phase 1 이관). 단, `/timpani_ttsched` 안정 게시 확인·제어 wake 비실행([DDR-011](../DDR-011-runtime-table-update/DDR-011-runtime-table-update.md) `D-P0-05`)과 Pipeline 미포함 일반 TT 테이블의 [REQ-011](../DDR-011-runtime-table-update/REQ-011-runtime-table-update.md) Phase 0 정상 갱신 회귀 방지는 필수 |
| **입력 및 역할 분담** | `Timpani-O` 입력(또는 시험 입력 경로)으로 DAG를 제출하고 `Timpani-O`만 검증·생성하여 기존 `O → N` 경로로 전송. `Timpani-N`이나 SHM 직접 주입만으로 End-to-End 성공 판정 금지 |
| **인터페이스·승인 제약** | 기존 공개 C++ 메서드 및 `timpani_task_slot` 초기 레이아웃 보존. `.proto` 확장(`OI-DD-01`), SHM 후방 확장 및 `libttsched` 신규 공개 API(`OI-DD-02`)는 코드 반영 전 Project Leader 승인 필수 |

Harmonic 다중 주기, 계층별 정밀 시간 창, 정량 `E2E Feasibility`, `SKIP`, `DATA_TIMEOUT`은 Phase 0 범위에서 제외한다. Phase 0의 CPU/slot 배치는 기존 TT 또는 CBS scheduler 정책을 사용한다.

---

## 3. 기능 요구사항 (Phase 0 Functional Requirements)

### 3.1 입력·생성·사전 거절 (`Timpani-O` & `Timpani-N`)

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-001** | `Timpani-O`만이 `TaskReference(workload_id, task_id)` 형식의 선행 의존성을 해석·검증해야 한다. 빈 `workload_id`는 현재 workload로 정규화하고 다른 workload 참조는 Phase 0에서 거절한다. `task_id`는 SHM 이름 조회 호환을 위해 **노드 내 유일한 `1..15`바이트 ASCII**여야 한다. 순환, 자기 참조, 미존재 참조, 동일 Task의 `predecessors` 배열 내 중복 선행 선언(`[A, A]`), Task 쌍/노드 내 `task_id` 중복, `(workload_id_hash, task_id_hash)` 충돌, 선행 최대 `16`개·후행 간선 최대 `8`개 초과, incoming predecessor-bit 중복 배정 또는 incoming bit union과 `required_mask` 불일치 입력은 사유와 함께 거절하고 확정 모델과 활성 테이블을 변경하지 않아야 한다. | REQ-DD-001 | AC-DD-P0-01, AC-DD-P0-04 |
| **REQ-DD-P0-F-002** | O는 각 Task의 `TaskInfo.temporal_class` 명시값을 사용하고, 미지정 Task는 `SchedInfo.temporal_class`를 상속해야 한다. resolved class를 유지한 채 `TtSlot` 또는 `CbsConfig`를 만들고, DAG 역할과 별도 공통 메타를 `(workload_id_hash, task_id_hash)` 쌍으로 전달한다. | REQ-DD-002, 009 | AC-DD-P0-01, AC-DD-P0-05, AC-DD-P0-06 |
| **REQ-DD-P0-F-003** | N은 공통 DAG Task key가 정확히 하나의 TT 또는 CBS entry와 대응되고, 전체 incoming-edge map이 해당 Task의 `required_mask`에 대한 bijection인지 확인한다. 또한 `n_tasks` 범위, `dst != src` 자기 참조 금지, `trigger_mode = ALL`, `execution_model ∈ {TT, CBS}`, `required_mask` 상위 16비트 0을 확인하며, 각 bit는 정확히 하나의 incoming edge로 배정되고 incoming OR-union은 mask와 같아야 하며 `0` mask에는 incoming edge가 없어야 한다. `(workload_id_hash, task_id_hash)` 쌍은 N의 결합 검증·진단용으로 `pipeline[]`에 기록되며 Phase 0 `libttsched`는 노드 내 유일 `1..15`B ASCII `tasks[i].name`으로 슬롯을 조회한다. 부적합·불일치 후보는 TT/CBS 승인식을 재계산하지 않고 거절한다. | REQ-DD-001, 002, CON-DD-03 | AC-DD-P0-04 |

### 3.2 런타임 `Data-Ready` 동기화 및 스케줄링 집행 (`Timpani-N` & `libttsched`)

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-004** | L1/L2 Source의 release authority는 실제 활성화 시 공통 activation ordinal을 Source slot의 `ready_state`에 release-valid 표식과 함께 기록한 뒤 Task를 깨워야 한다. 앱은 활성화 직후 읽기 전용 seqlock 기반 `ttsched_current_job_token(&token)`으로 그 snapshot을 1회 취득해 고정한다. `CLOCK_REALTIME`이나 Task-local counter로 token을 재산출해서는 안 된다. Successor는 하나의 활성 chain-job token에 대한 `ALL` ready와 계층별 dispatch gate를 모두 만족해야 하고, 게시·종료·슬롯 재사용 wake는 ready나 dispatch로 소비하지 않는다. | REQ-DD-002, 004, 005 | AC-DD-P0-01, AC-DD-P0-02 |
| **REQ-DD-P0-F-005** | Producer는 token-addressed payload publish와 필요한 release ordering을 완료한 뒤에만 `signal_ready(token)`을 호출한다. `signal_ready(job_token)`는 게시 commit guard 안에서 먼저 `n_tasks` 범위와 자기 슬롯 신원을 검증한 뒤 전체 outgoing-edge 목록을 snapshot·검증하고 각 destination을 첫 `ready_state` 변경 전에 preflight해야 한다. Sink(`num_out_edges == 0`)는 신원·SHM 검증 통과 후 변경 없이 `0`(no-op)을 반환한다. edge 수 초과, 잘못된/self/중복 destination, 범위 밖 bit, `required_mask` 상위 16비트 오염 또는 미소속, trigger/model 오류, destination token/중복 bit 실패 중 하나라도 있으면 첫 CAS 전에 fan-out의 mask·sequence·dispatch counter·wake를 모두 변경하지 않는다. 첫 CAS 시작 후 Source process crash에 대한 rollback/재시도 원자성은 Phase 0 보장이 아니며 발생 시 chain run을 실패 처리한다. 직접 중복은 `EALREADY`, 다른 token은 `ESTALE`이다. preflight 후 각 edge는 lock-free 64비트 CAS를 사용하며, 동일 동시 commit이 bit를 설정하거나 sequence를 앞서가면 수렴 완료(`0`)로 본다. 모든 edge CAS 완료 뒤에만 CBS `dispatch_counter` 갱신/wake를 수행한다. | REQ-DD-004, 005, 007 | AC-DD-P0-01, AC-DD-P0-02 |
| **REQ-DD-P0-F-006** | `ttsched_init()`은 비RT 경로에서 `fstat()`로 SHM 크기를 1회 검증해 `g_shm_pipeline_capable`을 설정하고 `mmap()` 직후 fd를 즉시 닫으며 스레드 종료 시 `munmap`한다. RT 경로(`signal_ready`, `wait_data`, `current_job_token`)는 `shm_open`/`fstat`/`close` 없이 순수 원자 로드로 `g_shm_pipeline_capable`, `publication_guard`(`WRITER_PENDING`), `shm->magic`, 짝수 `generation`, `pipeline_header.{magic, abi_version, abi_size, instance_id}`를 확인한다. `Timpani-N`만 `ftruncate()`/`shm_unlink()` object lifecycle을 소유하며 live object를 고정 크기(`3,128`B, no-shrink)로 유지하고, 교체·종료는 `WRITER_PENDING` 유지 + drain + `magic=0`/`pipeline_header.magic=0` 무효화 + `ftruncate` 없는 `shm_unlink()` + `O_CREAT|O_EXCL` 신규 `instance_id` 생성으로 수행한다. `wait_data()`와 `current_job_token()`은 `publication_guard`에 쓰지 않는 읽기 전용 seqlock으로 `O_RDONLY`(`PROT_READ`) 매핑을 지원하며, `ttsched_init()`과 새 세대 fallback은 현재 `dispatch_counter`를 최초 attach cursor로 snapshot하고, `wait_data()`는 그 cursor 이후만 FIFO 순차 소비(`g_last_dispatch_counter++`)해 job token을 반환하며 attach 이전 token을 replay하지 않는다. 정상 Phase 0에서는 모든 Successor가 최초 dispatch 전에 초기화되고, 그 경우 cursor `0`이 token `0`을 보존한다. 또한 `dispatch_counter`만 futex wait/wake word로 사용한다. | REQ-DD-004, 015 | AC-DD-P0-03, AC-DD-P0-05 |
| **REQ-DD-P0-F-007** | L1 Successor는 Timer Master가 자신의 TT slot 시각에 먼저 `next_tt_start`를 갱신하고 내부 경량 guard로 `pending ready`를 확인한 경우에만 `current_slot_map`을 활성화하고 `dispatch_counter`를 1 증가·futex wake·`scx_bpf_kick_cpu`한다. slot에 ready가 없거나 ready가 slot 후에 생기면 해당 slot에서 `dispatch_counter` 증가·`current_slot_map` 활성화(`SLOT_NONE` 유지)·`kick_cpu`·wake가 모두 `0`건이다. L2 Successor는 전체 outgoing-edge CAS가 완료된 `ALL` fan-out commit에서만 `dispatch_counter`를 1 증가시키고 wake한 뒤 BPF CBS CPU/budget/Gap gate만 실제 실행을 결정한다. | REQ-DD-008, 009 | AC-DD-P0-01, AC-DD-P0-05 |

### 3.3 시험 관측 및 전제 이탈 처리

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-F-008** | 시험 로그는 테이블 식별자, job token, payload generation/token, 태스크별 시작·완료 `CLOCK_MONOTONIC` 타임스탬프, `ttsched_signal_ready()` 소요 시간, 사전 거절 사유를 연결할 수 있어야 한다. 중복 token/token 불일치 거절은 `EALREADY`/`ESTALE`을 기록한다. 첫 CAS 후 Source process crash가 관측되면 부분 commit 가능 지점을 기록하고 chain run을 실패 처리해야 하며, 자동 rollback/retry 성공으로 판정해서는 안 된다. 연결 단절·시계 이상 등 Phase 0 시험 전제가 깨진 경우에도 성공으로 판정하지 않고 시험을 중단해 원인을 기록하며, 자동 복구나 타임아웃 `SKIP`이 동작한다고 가정해서는 안 된다. | REQ-DD-013 | AC-DD-P0-01..06 |

---

## 4. 비기능 요구사항 (Phase 0 Non-Functional Requirements)

| ID | 요구사항 명세 | 상위 REQ | REQ 수용 기준 |
|:---|:---|:---|:---|
| **REQ-DD-P0-N-001** | 변경한 RT critical path(`Timer Master` 루프, `libttsched` `signal_ready`/`wait_data`/`current_job_token` 경로, `scx_timpani` 콜백)에 동적 메모리 할당, 블로킹 IPC/gRPC, 데이터 파일/네트워크 I/O, **`shm_open`/`fstat`/`close` VFS/fd 시스템 콜**, 무한 busy-spin 또는 블로킹 lock을 추가해서는 안 된다. SHM 크기 `fstat()` 검증은 비RT `ttsched_init()`에서만 수행한다. `publication_guard` writer는 비대기 fetch-add/fetch-sub만 사용하고 실패 롤백을 포함한 모든 해제 경로에서 `pipeline_commit_exit()`로 Lost Wakeup을 방지하며 경합 시 `EAGAIN` 또는 `ESTALE`을 반환한다. 게시자는 active commit을 무한 대기하지 않고 정상 게시/갱신 시 `TIMPANI_PUB_DRAIN_TIMEOUT_US`(`5 ms`, 또는 `min(5 ms, H/4)`) 초과 시 이연(Phase 1+ 최대 `2`회), 종료/교체 시 `TIMPANI_PUB_SHUTDOWN_DRAIN_MS`(`50 ms`) 초과 시 Safe Abandonment로 강제 무효화한다. `ready_state` 64비트 원자 연산은 대상 아키텍처에서 lock-free여야 하며, 워크로드 컨테이너는 `CAP_BPF` 특권 없이 동작해야 한다. | REQ-DD-007, CON-DD-04 | AC-DD-P0-06 |
| **REQ-DD-P0-N-002** | Isolated CPU + `PREEMPT_RT` 환경에서 워밍업 후 연속 `10,000`회 이상 표본을 측정할 때, 단일 간선(`num_out_edges = 1`) `ttsched_signal_ready()` 소요 시간은 **`p99 <= 5 µs`**, 최대 fan-out(`num_out_edges = 8`)은 **`p99 <= 10 µs`** 를 만족해야 한다(mean/`p99`/`p99.99`/max 및 경합 조건을 함께 기록). 또한 L1/L2 그래프 혼합이 각 계층의 기존 타이밍 기준을 악화시키지 않아야 한다. | REQ-DD-007, 008, 013 | AC-DD-P0-01, AC-DD-P0-05 |
| **REQ-DD-P0-N-003** | DAG 순서·공통 의존성 메타·계층별 TT/CBS 배치가 입력 순서와 무관해야 한다. 실행 계층은 `predecessors` 유무로 결정되지 않는다. | REQ-DD-001, 002, 009, CON-DD-04 | AC-DD-P0-05, AC-DD-P0-06 |
| **REQ-DD-P0-N-004** | 인터페이스 변경은 승인 후 반영하며 TT/CBS/SHM/간선 상한을 넘는 입력은 사전 거절한다. | REQ-DD-001, 015, CON-DD-05, CON-DD-06 | AC-DD-P0-04, AC-DD-P0-05 |

---

## 5. Phase 0 검증 시나리오

고정 `Epoch`/`Hyperperiod`(`100ms`)와 사전 지정 Isolated CPU에서 `Timpani-O` 생성 결과를 사용한다. host 단위 시험과 Linux 6.12+ `sched_ext` 목표 환경의 End-to-End 실행 증거를 구분하여 기록한다.

| 시험 ID | 입력·조작 | 관측 및 성공 판정 | REQ 수용 기준 |
|:---|:---|:---|:---|
| **TC-DD-P0-001** | 고정 DAG에서 Task class를 명시한 경우와 workload class에서 상속한 경우를 섞어 L1→L1, L1→L2, L2→L1, L2→L2 최소 조합을 반복한다. Successor의 두 초기화 순서—최초 dispatch 전 attach와 `dispatch_counter=N` 이후 attach—를 모두 시험한다. PoC는 Producer별 최소 2-slot token-addressed payload와 consumer token marker를 사용한다. L1/L2 Source는 release authority가 stamp한 activation ordinal을 `ttsched_current_job_token()`으로 취득하며, 앱 wake를 Hyperperiod 경계 뒤로 지연시키거나 `clock_gettime(CLOCK_REALTIME)` 결과를 mock/interpose하여 forward/backward step해도 token을 바꾸지 않으며 호출 자체가 없는지, 32비트 activation ordinal이 `0xFFFFFFFF → 0`으로 wrap될 때도 Source/Successor가 같은 cycle을 쓰는지 확인한다. L1 Successor에는 `ALL`을 slot 전에, slot 직후, ready 없이 배치하고 CBS budget/TT active 상태도 조작한다. 또한 Isolated CPU + `PREEMPT_RT`에서 연속 `10,000`회 이상 `signal_ready()` 오버헤드(`num_out_edges = 1` 및 `8`)를 측정한다 | L1은 slot 전 또는 ready 없음 시 `dispatch_counter` 증가 `0`·`current_slot_map` 미활성화(`SLOT_NONE`)·`kick_cpu` 생략; slot 전에 준비된 ready는 해당 slot에서 `+1`; slot 직후 생성된 ready는 현재 slot에서 wake `0` 후 다음 slot에서 `+1`; pending은 정상 시험에서 1개다. L2는 `ALL` 직후 release `1`, 소비자는 `wait_data()` FIFO 순차 소비로 dispatch된 token을 받지만 실제 실행은 CBS budget/Gap에 따라 지연 가능. Source token은 wake 지연·wall-clock step에 불변이며, `current_job_token()` before-release는 `EAGAIN`이다. 최초 dispatch 전 attach는 token `0`을 보존하고, `N` 이후 late attach는 token `0..N-1`을 반환하지 않은 채 다음 dispatch를 기다린다. payload token/generation 불일치, 승인 전 overwrite, 순서·token 혼합 위반 `0`, class 회귀 `0`, 단일 간선 `signal_ready` `p99 <= 5 µs`(fan-out=8 `p99 <= 10 µs`, mean/`p99`/`p99.99`/max 기록) | AC-DD-P0-01 |
| **TC-DD-P0-002** | L1/L2 Successor 각각에 일부 선행만 완료한다. A의 token `k`가 남아 있고 B가 미신호인 동안 중복 `(A,k)`, 미래 `A(k+1)`/`B(k+1)`, L1 slot 또는 제어 wake를 주입한다. 이어서 두 outgoing edge를 가진 Source의 뒤쪽 edge만 각각 손상(`num_out_edges`, index, `required_mask` 상위비트/소속, mode/model, 중복 destination)시키거나 그 destination을 `ESTALE`/`EALREADY` 상태로 만든 뒤 유효한 `B(k)`를 보낸다 | 중복 `(A,k)`는 `EALREADY`, 비활성 token은 `ESTALE`이다. 뒤쪽 edge validation/preflight 실패 시 첫 CAS 전 전체 fan-out의 mask/sequence/dispatch/wake가 불변이고 부분 알림 `0`건이다. 추가로 각 CAS 사이·CAS 직후·first wake 전후 failpoint에서 Source를 종료시키면 Phase 0가 자동 rollback/retry를 약속하지 않으며, 부분 상태를 관측하고 chain run을 실패·전제 이탈로 판정한다. 이후 유효 `B(k)`만 token `k`의 `ALL`을 정확히 1회 완성하고, 유효 multi-edge CBS fan-out은 첫 wake 전에 모든 CAS를 끝내며, 제어 wake는 `ESTALE`이다 | AC-DD-P0-02 |
| **TC-DD-P0-003** | 순환(`A → B → A`), 자기 참조, 미존재 참조, 다른 workload를 가리키는 Phase 0 참조, 동일 Task `predecessors` 중복(`[A, A]`), 노드 내 `task_id` 중복 또는 `1..15`B ASCII 위반, Task 쌍 중복/해시 충돌, 용량·간선 상한 초과, 두 incoming predecessor의 동일 bit 배정, incoming edge 없는 required bit, `required_mask` 밖 또는 상위 16비트 오염 incoming edge 입력을 `Timpani-O` 및 `Timpani-N` 사전 확인에 제출 | 활성화 전 거절 및 사유 확인, `Timpani-O` 확정 모델과 `Timpani-N` 활성 테이블·SHM 상태 불변. 유효 후보는 exact union/equality를 만족하고 통과 | AC-DD-P0-04 |
| **TC-DD-P0-004** | 구 784바이트 object와 784바이트 prefix만 mapping한 full object, extension magic/ABI-size 불일치, 미지원 ABI version, `magic=0`, 홀수 `generation`, `WRITER_PENDING`, `O_RDONLY` `wait_data`/`current_job_token`/`signal_ready`, Safe Abandonment(`50 ms` drain 초과 시 `WRITER_PENDING` 유지 + `magic=0`/`pipeline_header.magic=0` + `shm_unlink` + `O_CREAT|O_EXCL` 신규 `instance_id` 교체), `pipeline_commit_enter()` 실패 롤백 경합, 안정 확인 직후 게시가 packed CAS 전후에 시작되는 interleaving에서 API를 호출한다. writer를 `pipeline_commit_enter()` 후 CAS 직전 또는 wake 직전에서 50 ms 이상 pause하고 publisher가 Safe Abandonment+신 `instance_id` 교체를 완료한 뒤 writer를 resume한다. dispatch 증가 없는 게시/종료 control wake도 인가한다 | 비RT `ttsched_init()`의 `fstat()` < 784B는 `EINVAL`, 구/짧은 mapping에서는 TT API만 동작하고 `ENOTSUP`; magic/ABI-size 오류는 `EINVAL`, magic이 유효한 미지원 ABI version은 `ENOTSUP`이다. RT 경로의 `shm_open`/`fstat`/`close` 호출 `0`회, `O_RDONLY` Successor의 `wait_data`/`current_job_token`은 `SIGSEGV` 없이 성공하고 `signal_ready`는 `EACCES`다. 배포 manifest에서는 `timpani-pub` member가 신뢰 writer allowlist와 일치하고 비신뢰 workload는 read-only group에만 속한다. 무효화/교체(`magic=0`/`WRITER_PENDING`/`instance_id` 변경)·불안정 게시·제어 wake는 `pipeline[]` 접근, 상태 변경, `SIGBUS` 없이 `ESTALE`이며 재연결에는 비RT `ttsched_init()` 재초기화가 필요하다. rollback Lost Wakeup `0`건, 정상 경로 packed CAS·dispatch·wake는 게시자와 완전히 직렬화된다. abandon 후 resume late CAS/wake는 old object에 한정되고 신 object 변경은 `0`건, 아직 dispatch되지 않은 old consumer 실행은 `0`건, 구/신 세대 혼합·control-wake 실행은 `0`건이며 old API는 `ESTALE`다. 무효화 전에 token을 반환받은 Job 취소는 Phase 0 보장이 아니다 | AC-DD-P0-03 |
| **TC-DD-P0-005** | L1/L2 혼합 DAG와 기존 TT/CBS 회귀·011 일반 TT 갱신 시험 실행. TT Successor에 slot 전에 ready를 만들고, CBS Successor에 budget=0과 active TT을 각각 인가한다 | DAG 추가 후 기존 Source는 기존 counter/Event 경로를 유지한다. TT는 slot gate, CBS는 release 후 budget/Gap gate를 지키며 기존 class·scheduling 회귀 `0`건 | AC-DD-P0-05 |
| **TC-DD-P0-006** | 제출 순서를 섞고 동일 DAG 반복 생성 | 위상 순서·공통 의존성 메타·계층별 배치 100% 일치 | AC-DD-P0-06 |

### 5.1 Phase 0 경계 검증 매트릭스

아래 `BM-DD-P0-*`는 독립 러너 ID가 아니라 기존 `TC-DD-P0-001/002/004`에 포함되어야 하는 필수 boundary/failure 조합이다. host 단위 시험과 Linux 6.12+ `sched_ext` 목표 시험에서 각각 실행 여부를 기록하며, 목표 환경 미실행 항목은 성공이 아니라 미검증으로 판정한다.

| ID | 기존 TC | 입력·관측 조건 | 합격 / 전제 이탈 판정 |
|:---|:---|:---|:---|
| **BM-DD-P0-01** | TC-001 | Source release ordinal, 앱 wake 지연, mocked `CLOCK_REALTIME` step, `0xFFFFFFFF→0`; `source release state`, `job_token`, downstream `ready_sequence` 기록 | token/domain 불일치 `0`건, wall-clock 조회 `0`회, before-release `EAGAIN` |
| **BM-DD-P0-02** | TC-001 | consumer attach 전/후의 `dispatch_counter`, `g_last_dispatch_counter`; 최초 dispatch 전 `cur=0`, `cur=N` late attach | pre-attach case는 token `0` 소비, late attach는 `0..N-1` replay `0`; Phase 0 정상 E2E는 최초 case만 성공 판정 |
| **BM-DD-P0-03** | TC-002 | multi-edge preflight failpoint, CAS 사이/직후, 첫 wake 전후 Source 종료; per-destination mask/sequence/dispatch 기록 | 첫 CAS 전 오류는 상태 변경 `0`; 첫 CAS 후 crash는 자동 복구 성공을 주장하지 않고 chain run 실패·부분 상태 관측 |
| **BM-DD-P0-04** | TC-001/002 | token-addressed 최소 2-slot payload, generation marker/pattern, publish-before-signal, Consumer 처리 중 overwrite | payload token/generation mismatch `0`, 승인 전 overwrite `0`; marker/ack 위반은 시험 전제 이탈 |
| **BM-DD-P0-05** | TC-004 | SHM ACL/group manifest, `O_RDWR` writer allowlist, RO-only nonwriter | trusted `timpani-pub` member와 배포 manifest 일치, RO `signal_ready` `EACCES`; RW member의 slot/header 악의적 isolation은 비보장으로 판정 |
| **BM-DD-P0-06** | TC-004 | CAS/wake 직전 writer 50 ms 이상 pause, old object abandon, new `instance_id`, old consumer wake/late resume | new object 변경 `0`, 아직 미dispatch old consumer 실행 `0`, old API `ESTALE`; 이미 token을 받은 Job 취소는 비보장 |
| **BM-DD-P0-07** | TC-001/002/004 | `num_out_edges=8`, concurrent writer 경합, FUTEX wake/order, mean/p99/p99.99/max, timeout/`EAGAIN` count 기록 | signal latency 기준 유지, wake 중복/누락 `0`, 부분 CAS·control wake 실행 `0`, 측정 환경과 경합 수준 기록 |

공통 관측 필드는 `table_id`, SHM `generation`/`instance_id`, `task_id`, `job_token`, `dispatch_counter` 전후값, `payload_generation`, failpoint 이름, `errno`, 실행 시작/완료 시각, wake count다. 성공 판정 외의 결과는 **실패**와 **시험 전제 이탈(비보장 영역)**로 구분해 기록한다.

---

## 6. 미결·승인 항목의 Phase 0 처리

| REQ 항목 | Phase 0 결정 / 계약 | 상세 근거 |
|:---|:---|:---|
| **OI-DD-01** | `TaskInfo.temporal_class`는 optional explicit-over-inherit 규칙을 따른다. 같은 workload의 `TaskReference`로 DAG를 전달하고(노드 내 유일 `1..15`B ASCII `task_id`, 중복 선행 선언 거절), O→N에서는 `(workload_id_hash, task_id_hash)` 쌍으로 공통 metadata를 식별한다. Phase 0은 `ALL`만 지원하고 `E2E`는 OI-DD-05로 이관한다 **(인터페이스 승인)** | DDR-009 `D-DD-P0-03` |
| **OI-DD-02** | 784바이트 prefix 뒤 40바이트 extension header(`magic/version/abi_size/publication_guard/instance_id/origin_epoch_ns/hyperperiod_us`)와 `pipeline[]`를 추가한다. Source `ready_state`는 release-valid 표식+activation ordinal을, Successor `ready_state`는 mask+ready sequence를 가진다. 앱 소유 payload의 publish-before-signal·token visibility·retention precondition과 `ttsched_init()` attach cursor snapshot·`ttsched_current_job_token()`·`ttsched_wait_data()`(읽기 전용 seqlock, attach 이후 FIFO 순차 소비)·`ttsched_signal_ready()`(Sink no-op 포함) API 및 ABI/DAC·POSIX ACL/`errno` 계약 및 `timpani-pub` trusted-writer trust boundary, 비RT `ttsched_init()` `fstat()` 검증과 RT 무-VFS 순수 원자 재검증, 고정 크기(no-shrink)·Safe Abandonment(`5 ms`/`50 ms` drain) 수명과 old-object late operation isolation, incoming-edge/`required_mask` bit bijection, preflight-failure mutation-free fan-out 사전검증(첫 CAS 후 process-crash rollback은 Phase 2), Source의 64비트 lock-free release-store와 Successor의 64비트 lock-free CAS, release snapshot 기반 job token/ready sequence 정확 일치, `dispatch_counter` 전용 futex, `O_RDWR`/`O_RDONLY` 시험 및 control-wake 시험을 조건으로 설계를 승인한다. 미충족 시험이 있으면 구현 병합을 보류한다 **(조건부 SHM/API 승인)** | DDR-009 `D-DD-P0-01`, `D-DD-P0-04` |
| **OI-DD-03** | Phase 0 Source release authority는 기존 TT/CBS release 경로에서 실제 activation ordinal을 Source slot에 stamp한 뒤 Task를 깨우고, 앱은 `ttsched_current_job_token()`으로 그 snapshot을 취득한다. wall-clock 기반 재산출은 금지한다. L1 Successor는 Timer Master가 자신의 TT slot에서 `next_tt_start` 갱신 후 내부 경량 guard로 `pending ready`를 확인한 경우에만 `current_slot_map` 활성화·`dispatch_counter` 1 증가·wake·`kick_cpu`하며(`pending == false` 시 `SLOT_NONE` 유지 및 `kick_cpu` 생략), L2 Successor는 전체 outgoing-edge CAS가 완료된 `ALL` fan-out commit에서 1 증가시키고 wake한 뒤 BPF가 기존 CBS budget/Gap을 집행한다. slot-miss pending, pending 최대 1개, 첫 CAS 전에 발견된 뒤쪽 잘못된 edge의 mutation-free 거절, `Activation Window`/`SKIP` 이관을 조건으로 설계 승인하되, token boundary-delay/clock-adjustment·no-ready·mixed-class·late-ready·fan-out prevalidation 시험 미통과 시 구현 병합을 보류한다 **(조건부 Phase 0 실행 연결 승인)** | DDR-009 `D-DD-P0-03`, `D-DD-P0-05` |

---

## 7. 추적성 및 후속 단계 이관

§3·§4의 모든 활성 요구사항은 상위 [REQ-009](REQ-009-data-driven-workload.md) ID와 `AC-DD-P0-01` ~ `06`을 인용하며, 역방향으로 `AC-DD-P0-01` ~ `06`은 §5의 `TC-DD-P0-001` ~ `006`에 대응한다. `BM-DD-P0-01`~`07`은 해당 TC의 필수 경계 조합이므로 별도 수용 기준을 추가하지 않고 기존 AC의 fail/pass 판정을 더 엄격히 구체화한다. [HLD-009](HLD-009-data-driven-workload.md)는 컴포넌트 책임과 흐름, [DDR-009](DDR-009-data-driven-workload.md)는 핵심 설계 결정, [LLD-009](LLD-009-data-driven-workload.md)는 내부 자료·동기화·검증 접점을 명세하며 본 SRS의 활성 ID를 인용한다.

| 후속 단계 | 이관 범위 (본 문서 비포함) |
|:---|:---|
| **Phase 1** | Window, Harmonic 주기, L1/L2 혼합 DAG의 `E2E Feasibility`, 계층별 CPU/slot/예산 간섭 정밀화, Pipeline 갱신 |
| **Phase 2** | `SKIP`, 최초 원인 `DATA_TIMEOUT`, Sink Fallback, writer-pending/활성 commit을 포함한 클래스 공통 복구 상태 초기화 |

이관 범위의 상태 플래그·타이머 훅·복구 상태 기계를 Phase 0에 선행 구현하지 않는다.
