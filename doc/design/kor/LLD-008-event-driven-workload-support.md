<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# LLD-008: Event-Driven Workload Support — 상세 설계 (Low-Level Design)

**날짜:** 2026-11-11
**상태:** Draft (v5.2 — P1 Event–Job 계약 정합(Issue #53): 한 Event–한 Job, 명시적 Job 경계, 도달 기준 마감과 DeadlineMonitor, 실행 구간 관측. §1–§12 전체를 정합했다. v5.1에서 D-22~D-24와 리뷰 지적을 반영했다) — DDR-008(Proposal)의 D-01~D-24를 전제로 함
**작성:** Human + AI
**상위 문서:** [REQ-008](REQ-008-event-driven-workload-support.md), [SRS-008](SRS-008-event-driven-workload-support.md), [DDR-008](DDR-008-event-driven-workload-support.md), [HLD-008](HLD-008-event-driven-workload-support.md), [P1 계약 보강 합의](P1-008-event-job-contract-amendment.md)
**기준 코드:** `timpani26` HEAD `4201573` (`timpani-o/`, `timpani-n/`, `sample-apps/`, `examples/someip/`)

> 본 문서는 HLD-008의 컴포넌트를 **파일·구조체·함수 수준**으로 전개한다. 코드 블록은 구현 지침용 스케치이며, 식별자·필드 번호는 본 문서를 단일 기준(single source)으로 삼는다. 모든 수치(후보값)와 공개 API·SHM 레이아웃·protobuf 필드 변경은 **제안**이며 Project Leader 승인 대상이다. 구현·검증 완료를 주장하지 않는다.
> 코딩 규칙: 기존 저장소 스타일(C++17, `TLOG_*`, SPDX 헤더, 4-space)과 `.github/copilot-instructions.md`(RT 경로에 gRPC/할당 금지, proto 변경 승인) 준수.

---

## 변경 이력

| 버전 | 날짜 | 변경 |
|:--|:--|:--|
| v4 | 2026-11-10 | 공통 Event/Job 모델, 독립 검토·검증 반영, FCW 수정 스케치 확장, Phase 1 범위 |
| v5 | 2026-11-11 | P1 Event–Job 계약 보강 합의(Issue #53, `P1-008-event-job-contract-amendment.md`, P1 design brief v3)에 맞춰 §1–§12 개정. §6–§12에서 libttsched v2 계약과 구현 스케치, 인터리빙 논거와 P1 한계, 샘플(evttask, FCW), 타이밍 후보와 측정 방법, 실패 모드, AC-008-01~16 매핑, WBS, 열린 이슈(L-7 철회, L-9~L-23 추가)를 갱신했다. 한 Event–한 Job과 명시적 Job 경계, 도달 기준 마감(`t_arr + D`)과 DeadlineMonitor 신설, Event 레코드 링·Job 로그를 가진 SHM v2 슬롯, libttsched 게시·Event 대기·종료 계약, BPF의 실행 구간 관측으로 축소(MIT 검사·L2 DMISS 판정 제거), 단일 수용식(`L_del_max`, `δ_ovr`, `T_i ≤ MIT_i`, `D_i ≤ MIT_i`), FaultInfo 플래그 표 통일, proto 변경 승인 필요 표시. 어댑터 K/M 구분, batch count, 활성화 기준 DMISS는 폐지 |
| v5.1 | 2026-11-16 | 설계 체인 리뷰 반영. DDR-008 D-22(TT 없는 CPU의 CBS 실행)와 D-24(허용 CPU 집합 확인)에 맞춰 §1 매핑에 FR-O-022, FR-B-025, FR-N-032~033을 추가했다. §3.1·§3.3의 `DEFAULT_HP_US`는 테이블 형식 값으로 정정했고(표현 변경은 OI-25), §3.3에 TT 없는 CPU의 배치를, §3.5에 `TtFreeCpuAdmitsL2WithZeroBtt`을 추가했다. §5.1 바인딩 스케치에 허용 CPU 검사(FR-N-033)를 넣고, §4·§5의 BPF dispatch와 보충·감시가 TT 이벤트에 의존하지 않음을 명시했다. §7 evttask의 C 측정 옵션, §10 AC-008-17~19, §11 WBS, §12 L-24~L-25를 추가했다. 설계 제안이며 API, protobuf, SHM 필드는 바꾸지 않았다 |
| v5.2 | 2026-11-18 | 2026-11-17 변경: admission 용어를 수용 판정·수용식으로, 도착을 도달로 통일했다. budget-only L2 클래스 서술을 CBS 예산 관리와 Event/Job 관리의 책임 구분으로 바꿨다. 클래스나 구성 플래그는 두지 않는다(D-21, Project Leader(작성자)의 결정). wait 안에서 Job 시작을 확정하고 별도 공개 취득 API는 두지 않는다. 실행 문맥(D-24)과 타이머 대안 비교(F-10, OI-22)를 추가했다. 2026-11-18 리뷰 정정: `take()`의 복사 대상을 호출자 `ev` 버퍼로 고치고 앱 분류 비의존 문장을 복원했다. 정식 문서는 REQ-008 v5.2, SRS-008 v4.2, DDR-008 v3.3, HLD-008 v5.3, LLD-008 v5.2다. 이전 이력 행의 용어도 새 용어로 바꿨다. |

---

## 요약 (Korean Summary)

- **모델**: 수락한 Event 한 건이 Job 한 건이다. Job은 워커가 명시적으로 시작(`ttsched_evt_wait`)하고 명시적으로 끝낸다(`ttsched_evt_done`). 절대 마감은 도달 시각 + D이며 수락 시 고정된다. Event 생명주기는 QUEUED → RUNNING → COMPLETED/DISCARDED이고, TIMPANI가 닫는 CANCELLED/ABANDONED를 별도 결과로 둔다.
- **proto (승인 필요)**: `schedinfo`는 `TaskInfo.min_inter_arrival_us=11`, `FaultType` DMISS/BUDGET_EXCEED/MIT_VIOLATION(WATCHDOG 예약), `FaultInfo` `timestamp_ns=6`, `count=7`, `flags=8`. `node_control`은 `CbsConfig.min_inter_arrival_us=9`, `cpu=10`, `FaultInfo` `count=10`, `flags=11`, `job_seq=12`, `job_gen=13`. 승인 전에는 보고에 카운트만 싣는다.
- **timpani-o**: L2 task는 `0 < T_i ≤ MIT_i`, `D_i ≤ MIT_i`를 검증한다. CPU별 수용 판정은 모든 L2 task에 같은 식 `C_i + B_tt(k, D_i) + L_del_max + Σ_{j≠i}(C_j + δ_ovr) ≤ D_i`를 적용한다(`L_del_max` 후보 100 µs, `δ_ovr` 후보 1 ms). 충족 조건의 가정 8개를 명시한다. 두 번째 L2 task와 같은 CPU에 놓이면 기본 FCW 파라미터는 거부된다.
- **timpani-n**: TaskRegistry는 바인딩 시점에 슬롯을 활성화하고 `bind_gen`을 BPF TaskMeta와 종료 기록에 넣는다. 오래된 종료 기록은 무시한다. TimerMaster SHM v2는 슬롯당 Event 레코드 링(깊이 8, 레코드 128 B)과 Job 로그(32개, 48 B)를 가진다(슬롯 2752 B, 64 슬롯 후보, 객체 약 172 KiB). 활성화(프로토콜 1), 비활성화와 닫기 규칙(프로토콜 5), 시작 정리(프로토콜 7)는 seq_cst 원자 연산을 쓴다. 신규 DeadlineMonitor는 SCHED_FIFO 스레드로 1 ms 주기(후보) 스캔하며 QUEUED·RUNNING 기록의 마감 경과를 CAS로 표시한다. FaultMonitor는 계수 경로(무손실)와 상세 경로(best effort)로 나뉜다. EventSourceMonitor는 누적 계수를 읽는다. JobTraceCollector는 Job 로그와 실행 구간 레코드를 읽는다. Replenisher는 변경하지 않는다.
- **BPF**: 실행 구간 관측과 집행만 한다(CBS 예산, CPU 고정, 활성화 순서 큐, CARRYOVER·THROTTLED 표시). 활성화 기준 MIT 검사, `FAULT_MIT_VIOLATION` 발행, L2 DMISS 판정과 조기 DMISS를 제거했다. TT DMISS 경로(DDR-012)는 그대로 둔다.
- **libttsched v2 (승인 필요)**: `ttsched_evt_open`, `ttsched_evt_post`(도달 시각과 payload를 인라인으로 수락), `ttsched_evt_wait`(Job 하나를 시작), `ttsched_evt_done`(필수 종료). 게시 문맥과 워커는 source마다 하나다. 게시 (a)–(f)와 가져가기·종료·닫기의 경합 규칙은 §6.3에 적었다. 공개 API와 SHM 레이아웃은 승인 대상이다(§12 L-9, L-10).
- **샘플과 libttsched 세부(§6–§7)**: `evttask`는 타이머, datagram, pipe, 자기 생성 source에서 같은 API를 쓴다. FCW는 수신 콜백이 `RadarTarget`을 인라인으로 게시하고 워커가 Job을 종료한다. 통지 API, 배치 count, 링과 무효 표시 규약은 없다.
- **타이밍과 실패 모드(§8–§9)**: 감지 상한, 보고 상한, 도달 지연 상한, 비용은 모두 후보값이며 측정 전이다. 측정 방법을 함께 적었다. 실패 모드 표는 게시 거절, 큐 가득 참, 게시자와 워커의 사망, 데몬 재시작, 감시 정지, 기록 넘침을 포함한다.
- **테스트**: timpani-o GTest 추가, timpani-n GTest 타깃 신설. `job_fsm.h`, SHM 프로토콜 경쟁, libttsched 경쟁 테스트를 호스트에서 실행하도록 설계한다(§5.8, §5.9, §6.5). AC-008-01~19를 테스트 또는 절차에 매핑했다(§10). 실행하거나 검증했다는 주장은 없다.
- **수치**: 감지 지연, 보고 지연, `L_del_max`, `δ_ovr`, 큐 깊이, payload 크기는 모두 후보값이며 합의와 Linux 타깃 측정이 필요하다.

---

## 1. 목적 및 대응 요구사항

| 영역 | SRS-008 ID | 본 문서 절 |
|:--|:--|:--|
| 프로토콜 | IF-PO-001/004/005/006, IF-ON-001~004/006 | §2 |
| TIMPANI-O | FR-O-001/003/005/010~016/020/022 | §3 |
| BPF | FR-B-001~005/010~017/022~023, FR-B-024/025 | §4 |
| TIMPANI-N | FR-N-002~007/010~015/020~022/024, FR-N-027~033 | §5 |
| 공통 모델·3층 | FR-G-001/002, FR-G-003 | §1.1, §1.2 |
| 라이브러리 | FR-L-001~012, IF-L-001~004 | §5.6(SHM 레이아웃), §6 |
| 샘플 앱 | FR-G-001, FR-L-006 | §7 |
| 타이밍 | TMG-001~016 | §8 |
| 검증 인터페이스 | IF-V-001~004, FR-N-026, OBS-004 | §5.4, §10 |
| 비기능 | NFR-001(용량), NFR-002(BPF 반복 상한), NFR-003(TT 무회귀), NFR-005(proto 호환) | §4.1, §4.5, §8, §2.3 |
| 테스트 | NFR-006/007, AC-008-* | §5.8, §5.9, §10 |

### 1.1 공통 모델 ↔ 구현 요소 대응

이전 초안의 "어댑터 K/M"(두 가지 동등한 Event 획득 방법) 구분은 폐지한다. Event는 명시적 연결(libttsched Event 큐)로만 만들어지며, 커널 관측(BPF)은 실행 구간을 관측할 뿐 Event를 만들지 않는다.

| 모델 요소 (REQ-008 §2, MOD-009/010/011) | 구현 요소 | 계층 | 절 |
|:--|:--|:--|:--|
| Event source S (`task_id`, L2 task 1:1) | `CbsConfig{task_id, min_inter_arrival_us, deadline_us}` → SHM 슬롯 `name`, `mit_us`, `deadline_us` | 명시적 연결 | §2.2, §3.1, §5.6 |
| Event 식별자 `(gen, seq)` | 레코드 `gen`(등록 세대), `seq`(게시 시점의 `tail` 값) | 명시적 연결 | §5.6, §6.2 |
| Event 도달 시각 `t_arr`, 절대 마감 `t_dl = t_arr + D` | 레코드 `t_arr_ns`, `t_dl_ns`(수락 시 한 번 기록, 이후 불변) | 명시적 연결 | §5.6 |
| Arrival (MIT 대상) | `ttsched_evt_post()` 단계 (c)의 `arrivals`, `mit_viol` 계수 | 명시적 연결 | §5.6, §6.2 |
| 수락 거절 (E_INACTIVE, E_FULL) | 게시 전 거절, `overflow` 계수 | 명시적 연결 | §5.6, §6.2 |
| 워커 Job 시작 (R1–R3) | `ttsched_evt_wait()`의 QUEUED → RUNNING 전이, `t_begin`, `started` | 명시적 연결 | §5.6, §6.2 |
| 워커 Job 종료 (COMPLETED / DISCARDED) | `ttsched_evt_done()`의 종료 전이, `t_end`, Job 로그 | 명시적 연결 | §5.6, §6.2 |
| 닫기 (CANCELLED / ABANDONED, TIMPANI가 닫음) | TimerMaster 비활성화·활성화·시작 정리의 닫기 규칙 | TIMPANI-N | §5.1, §5.6 |
| 마감 실패 감지 (MISSED, Job당 1회) | `DeadlineMonitor` 주기 스캔, CAS, `missed++` | TIMPANI-N 감시 | §5.9 |
| 실행 구간 (커널 활성화 → 잠듦) | BPF `runnable` / `quiescent` → `JobRecord`(실행 구간 레코드) | 커널 관측(BPF) | §4.3, §4.4, §5.4(a) |
| 보충 윈도우 (D-04) | `CbsState.replenish_at_ns`, `cbs_on_activation()`, `cbs_try_promote()` | 커널 집행(BPF) | §4.3, §4.4 |
| CPU 고정·활성화 순서·스로틀 | `cbs_map`, CPU별 DSQ(`queue_key_ns` 순) | 커널 집행(BPF) | §4.2, §4.4 |
| 등록 세대·바인딩 | 슬롯 `gen`(u64), `bind_gen`(TaskMeta, 실행 구간·종료 레코드) | TIMPANI-N | §4.1, §5.2, §5.6 |
| Event source 통계 | 슬롯 누적 계수 → `EventSourceMonitor` 판독 | TIMPANI-N | §5.4(b) |
| Job 기록 (마감·식별 포함) | SHM Job 로그(워커 생산) + TIMPANI-N의 닫기 기록 | 명시적 연결 + TIMPANI-N | §5.4(a), §5.6 |

### 1.2 개념과 메커니즘의 경계 (model-first)

공통 용어는 [REQ-008 §2](REQ-008-event-driven-workload-support.md#2-개념-모델-공통-eventjob-모델-common-eventjob-model)를 따른다. **CBS 예산 관리**와 **Event/Job 관리**는 같은 L2 워커의 상호 보완적 책임이며 앱의 클래스·모드가 아니다. DDR-001의 L2 분류와 기존 선택적 연결 호환성은 바꾸지 않는다.

- **개념 (REQ-008 §2)**: Event source, Event, Job, 도달 시각, 절대 마감, 생명주기 상태, 마감 실패, 실행 구간. 이들의 정의는 OS·스케줄러·라이브러리와 독립이다.
- **메커니즘 (본 문서)**: futex, CAS, SHM 레이아웃, BPF 콜백, API 이름은 개념을 구현하는 수단이다. 개념 정의를 메커니즘에 맞춰 바꾸지 않는다.
- **규칙 (REQ-008 MOD-009/010, DET-007, PRT-008/009)**:
  - R1: 워커가 대기 중에 수락된 Event는 즉시 Job 대상이 된다.
  - R2: Job이 실행 중에 수락된 Event는 QUEUED로 남으며 진행 중인 Job에 합쳐지지 않는다.
  - R3: Job이 끝났고 QUEUED Event가 있으면 워커는 잠들지 않고 가장 오래된 Event로 다음 Job을 시작한다. 연속된 Job은 서로 다른 Job이다.
  - R4: Job 한 건은 Event 한 건을 소비한다. source마다 FIFO다.
  - I1 (불변식): 수락된 모든 Event는 정확히 한 개의 종료 상태에 도달하며, Job과 수락된 Event는 1:1로 대응한다.
- **MIT**: 어댑터 경계의 도달에 대한 제약이다. MIT 계수에 들어가는 게시는 수락, 가득 참 거절, 롤백(REVOKED) 중 하나로 끝난다. 비활성 source에 대한 게시는 도달로 세지 않는다. MIT를 어기는 도달도 수락하고 큐에 넣으며, 위반을 계수하고 보고한다. **활성화 기준 MIT 검사는 없다**(A-04 철회).
- **세 계층**: (1) 모델, (2) 명시적 연결과 커널 관측 — 명시적 연결(libttsched Event 큐)은 Event, Job, 마감을 제공하는 유일한 경로이고, 커널 관측(BPF)은 모든 L2 task의 실행 구간과 예산을 관측한다, (3) 집행 — CBS·CPU·TT는 BPF, 마감 감시와 보고는 TIMPANI-N, 수용 판정은 TIMPANI-O.
- **책임과 관측 범위(MOD-011, D-21)**: 명시적 Event 연결을 쓰지 않아도 CBS 예산 관리, CPU 고정, BUDGET_EXCEED, 실행 구간 기록은 유지된다. 연결 없이는 Event 식별·도달 시각·Job 기록·MIT 계수·Event 기준 마감 감시의 정보를 얻지 못하며 BPF 실행 구간에서 이를 추정하지 않는다. Event 기록이 없을 때 선언 D는 수용 판정 파라미터이지 감시되는 Job 마감이 아니다. P1은 추가 분류·구성 플래그를 두지 않으며, 어떤 컴포넌트도 앱 분류에 의존하지 않는다. source에 Event 기록이 없으면 DeadlineMonitor가 판정할 객체가 없는 것이며 일시적인 게시 침묵으로 앱을 재분류하지 않는다.
- **A-06 (블로킹)**: 더 이상 정합성 전제가 아니다. Job 경계가 명시되므로 Job 중 블로킹은 허용되고 관측된다(R에 포함되며, 블로킹 뒤의 깨움은 열린 예산 윈도우 안의 커널 활성화다). 다만 수용 판정 결과(§3.3)는 블로킹하지 않는 Job에만 보장되며, 블로킹하는 source의 마감은 감시되지만 보장되지 않는다.

### 1.3 P1 한계 요약 (상세는 각 절)

- 마감 실패 감지는 감시 주기와 깨움 지터에 의존하며 지연 0을 보장하지 않는다(§5.9, TIM-009). 보고 지연은 별도다(TIM-010).
- 데몬이 중단된 동안에는 실시간 감지와 보고가 없다. 다음 시작 정리에서 `DETECTED_LATE`로 늦게 확인된다. 하트비트는 없다(§5.6 프로토콜 7).
- 한 source에 대한 게시 컨텍스트는 하나다. 여러 생산자 게시는 P2다(F-13).
- payload는 인라인 최대 64 B(후보)다. 큰 payload와 해제 통지는 P2다.
- SHM 객체에 쓰는 모든 프로세스는 신뢰 대상이다(§6, 격리는 F-14).
- 재등록 경계에서 게시 1건과 `missed++` 1건이 이전·새 등록 계수에 걸칠 수 있다(§5.6 한계).
- 워커 종료 대기를 취소하는 API는 없다(LLD L-8, 9209bc5 유지).
- 완료 시각 `t_end`는 워커가 선언된 처리와 결과 게시를 마친 시각이다. 네트워크 전달이나 장치 반영의 완료를 뜻하지 않는다(P1-008 §2). IRQ/ISR, 미들웨어 내부 스레드, 네트워크 전체 경로의 예산은 TIMPANI가 자동으로 잡지 않는다(REQ-008 §7, Issue #53).
- 게시자가 게시 중간에 죽으면 두 경우가 있다. 항목을 공개한 뒤 `tail`을 전진시키기 전에 죽으면 다음 게시가 R-b 복구로 그 항목을 수락으로 계수한다. `tail`을 전진시킨 뒤 (f) 앞에서 죽으면 `accepted` 계수 1건이 빠지며 복구되지 않는다. 두 경우 모두 워커는 다음 게시나 TimerMaster 변화 전까지 깨어나지 않으며, 그 Event는 마감 감시 대상으로 남는다(§6.4, A8).
- 데몬 시작 정리(`kind = NONE`)와 활성화 틈(sched_ext 전환 뒤 슬롯 활성화 전)에는 게시가 E_INACTIVE를 받는다(§6.4). TT 재발행(`magic` 토글)은 게시를 거절하지 않는다. 게시 (a)는 슬롯 `kind`만 보며(§6.2), 이로써 L-15는 닫힌다(§12).

---

## 2. 프로토콜 변경 (`timpani-o/proto`) — 제안, Project Leader 승인 필요

모든 변경은 필드 추가다. 승인 전에는 구현하지 않는다. 미승인 시 보고에는 카운트(`count`)만 실리고 `flags`, `job_seq`, `job_gen`은 없다(SRS/REQ 기준 축소 동작).

### 2.1 `schedinfo.proto` (Pullpiri ↔ O)

```proto
message TaskInfo {
  // ... 1..10 unchanged ...
  // L2 (TEMPORAL_SPORADIC) only: minimum inter-arrival time in us. A constraint on arrivals at the
  // adapter boundary (REQ-008 §2). 0 → period is used as MIT (transition rule, warning logged).
  uint32 min_inter_arrival_us = 11;
  // [P2] TemporalClass temporal_class = 12;  // per-task override, 0 = inherit
}

enum FaultType {
  UNKNOWN = 0;
  DMISS = 1;           // L2: an Event's Job was still open after its absolute deadline t_arr + D (once per Job)
  BUDGET_EXCEED = 2;   // informational: CBS budget exhausted (once per throttle episode)
  WATCHDOG = 3;        // reserved (P2)
  MIT_VIOLATION = 4;   // arrivals closer than min_inter_arrival_us (counted at the adapter boundary, one report per T_s)
}

message FaultInfo {
  // ... 1..5 unchanged ...
  uint64 timestamp_ns = 6;   // CLOCK_REALTIME at detection (node). New in this message (node_control has timestamp_ns = 5, unset today)
  uint32 count = 7;          // occurrences covered by this report: misses of the registration, violations in the window, or 1 per episode. Not dmiss_count / cumulative_dmiss (verification-only, not reused)
  uint32 flags = 8;          // FaultFlags bitmask (table below)
}
```

FaultFlags 표는 `schedinfo.FaultInfo.flags`와 `node_control.FaultInfo.flags`에서 같은 값을 쓴다. 이 표가 플래그 값의 단일 출처다. BPF 실행 구간 플래그(§4.1)와는 다른 표다.

| 비트 | 이름 | 뜻 | 설정 주체 |
|:--|:--|:--|:--|
| 0x1 | MIT_RELATED | 마감 실패 Job의 `t_arr` 기준으로 `t_arr − T_s` 이후에 MIT 위반 도달이 있었다(`last_viol_ns ≥ t_arr − T_s`) | FaultMonitor (`last_viol_ns` 판독으로 계산) |
| 0x2 | THROTTLED | 감지 시점에 서버가 스로틀 상태였다. 감지 경로(DeadlineMonitor)에서만 설정된다 | DeadlineMonitor (CbsState 판독) |
| 0x4 | NOT_STARTED | 감지 시점에 Event가 아직 QUEUED였다(구 EARLY 비트를 대체) | DeadlineMonitor |
| 0x8 | LATE_COMPLETION | 마감 실패가 워커의 종료 전이(`t_end > t_dl`)에서 설정되었다(구 MARKER 비트를 대체) | 워커 (Job 로그 `miss_src = TTSCHED_MISS_END`) |
| 0x10 | DETECTED_LATE | 마감 실패가 닫기 절차(시작 정리, 비활성화·활성화 닫기 규칙, 워커 대기 중 stale 세대 취소 `close_stale`)에서 설정되었다 | TIMPANI-N 닫기 절차 |

- 구 `EARLY`(0x4)와 `MARKER`(0x8)는 폐지되었다. 같은 비트를 위 의미로 재사용하며 구 구현은 없다(설계 단계).
- 구 `CARRYOVER`(0x40)는 `FaultInfo.flags`에서 뺀다. 이 값은 BPF 실행 구간 레코드에만 남는다(§4.1). P1에서 DMISS 보고 플래그가 아니다.
- `THROTTLED`는 감지 경로에서만 값이 있다. 워커나 닫기 절차가 설정한 마감 실패에는 뜻이 없으므로 0이다.

schedinfo에는 Job 식별자(`job_seq`, `job_gen`)를 싣지 않는다. 이 값은 node_control 쪽에서만 전달된다(§2.2). TIMPANI-O가 Pullpiri로 전달할 때 식별자를 전달할지는 열린 질문이다(§12, 후속 정합).

### 2.2 `node_control.proto` (O ↔ N)

```proto
enum FaultType {
  UNKNOWN = 0; DMISS = 1; BUDGET_EXCEED = 2; WATCHDOG = 3;
  MIT_VIOLATION = 4;
  WAKEUP_LATENCY_EXCEED = 5;   // reserved (P2)
}

message FaultInfo {
  // ... 1..9 unchanged ...
  uint32 count = 10;     // see schedinfo.FaultInfo.count (per-report count; dmiss_count is cumulative and not reused)
  uint32 flags = 11;     // same bitmask as schedinfo.FaultInfo.flags (§2.1)
  uint32 job_seq = 12;   // DMISS only: per-source seq (§5.6) of the missed Event; 0 = not available
  uint64 job_gen = 13;   // DMISS only: registration gen (§5.6). (task, job_gen, job_seq) is unique within one SHM object; across daemon restarts it is probabilistic (§5.6); 0 = not available
}

message CbsConfig {
  // ... 1..8 unchanged (budget_us = C_s, period_us = T_s, deadline_us = D) ...
  uint32 min_inter_arrival_us = 9;  // MIT; 0 → use period_us (transition)
  optional uint32 cpu = 10;         // assigned isolated CPU. `optional` (proto3 field presence) so that CPU 0 differs from "not set"
}
```

- `job_seq`/`job_gen`은 DMISS에서만 채운다. BUDGET_EXCEED와 MIT_VIOLATION은 0으로 둔다.
- `job_gen`은 `uint64`다. 상위 32비트는 SHM 객체의 `instance_id`, 하위 32비트는 바인딩 계수에서 온다(§5.6). 데몬 재시작 뒤 값이 겹치지 않는 것은 확률적이다(§5.6, A14). 구간이 2^32 ns의 배수인 재시작이나 벽시계 변경에서는 상위 비트가 같아질 수 있다.

**발행 규칙**(메시지 변경 없음): `PartitionConfig.layers = [ {layer_index=1, model=TIME_TRIGGERED, tt_slots=[...], cbs_entries=[]}, {layer_index=2, model=EVENT_DRIVEN, tt_slots=[], cbs_entries=[...]} ]`. 비어 있는 레이어는 생략할 수 있다. `table_id = "table_" + generation`.

### 2.3 하위 호환 규칙
- 모든 변경은 필드 추가다. 구 Pullpiri(필드 1~10)·구 timpani-n(필드 1~8)이 보낸 메시지는 그대로 역직렬화된다.
- O는 항상 `cpu`를 채우며 CPU 0도 명시 값이다. timpani-n은 `has_cpu()`가 false이거나 Isolated CPU가 아닌 엔트리를 적용 오류로 처리한다(파티션 cpuset으로 대체하지 않는다. 대체하면 수용 판정 때의 CPU별 검사가 무효가 된다). O와 N은 함께 배포한다. proto3 `optional`은 protoc 3.15 이상이 필요하다.
- `scripts/mock_pullpiri.py`, `scripts/mock_pullpiri.yaml`, `timpani-o/scripts/grpcurl/cbs.json`에 `min_inter_arrival_us`를 추가한다. 제안 필드(`count`, `flags`, `job_seq`, `job_gen`)가 승인되지 않으면 이 필드들은 추가하지 않는다.

---

## 3. TIMPANI-O 상세

### 3.1 `global_scheduler.h` — 자료구조

```cpp
struct ClassifiedTask {
    std::string workload_id;
    std::string task_id;
    Mechanism   mechanism;
    uint32_t    period_us;      // L1: period. L2: server period T_s, with 0 < T_s <= mit_us (T_i in REQ terms)
    uint32_t    mit_us;         // L2 only (NEW) — min_inter_arrival_us, or period when not given (D-11). Replaces the dual use of period_us
    uint32_t    wcet_us;        // C_i (L2): declared CPU budget per Job, with headroom (§3.3 assumption 6)
    uint32_t    deadline_us;    // D_i (L2): relative deadline, counted from arrival t_arr
    uint32_t    max_dmiss;
    int         assigned_cpu;
};

struct CbsAllocation {
    // ... existing ...
    uint32_t mit_us;            // NEW → CbsConfig.min_inter_arrival_us
};
```

```cpp
enum class InfeasibleReason {
    NonHarmonicPeriod, TtSlotConflict, NoCbsBandwidth, SafetyCbsExceeded,
    NoCbsCpu, UtilizationExceeded,
    NodeUnknown,         // NEW: node_id not registered (FR-O-004 minimum set)
    ParamInvalid,        // NEW: 0<T≤MIT, C≤D≤MIT violated, task_id longer than 15 bytes, or task_id not unique on the node
    GapTooSmall,         // NEW: min_gap < CBS_MIN_EXEC_US (was warning)
    DeadlineInfeasible,  // NEW: the admission inequality (§3.3) failed for some L2 task on the CPU
};
```

```cpp
struct TtBlocking {          // NEW (per CPU)
    std::vector<std::pair<uint32_t,uint32_t>> merged_us;   // merged TT occupancy [start,end) sorted, one hyperperiod
    uint32_t hp_us = 0;
    uint32_t max_contig_us = 0;                            // longest merged block (wrap-aware); diagnostic only — B_tt(k,L) can be larger when the window covers several blocks
    uint32_t total_us = 0;                                 // sum of merged block lengths in one hyperperiod
    uint32_t occupancy_within_us(uint32_t L) const;        // B_tt(k, L): max TT time in any window of length L (wrap-aware)
    // [P2] sbf_k(L) gap-supply function
};

// Tunables (header constants, overridable by -D at build)
static constexpr double   U_BOUND        = 0.80;
static constexpr double   U_OVERHEAD     = 0.02;
static constexpr uint32_t CBS_MIN_EXEC_US = 100;
static constexpr uint32_t L_WAKE_MAX_US  = 50;     // TIM-001 (runnable → first run on an idle, TT-free CPU), candidate, to be measured (A24)
static constexpr uint32_t L_POST_MAX_US  = 50;     // NEW: TIM-011 part 1 (post → runnable), candidate, to be measured
static constexpr uint32_t L_DEL_MAX_US   = L_POST_MAX_US + L_WAKE_MAX_US;   // NEW: TIM-011 delivery bound, candidate 100 µs
static constexpr uint32_t DELTA_OVR_US   = 1000;   // NEW: TIM-004 / TMG-004 overrun bound δ_ovr = one tick at HZ=1000, candidate
static constexpr uint32_t DEFAULT_HP_US  = 10000;  // TT 없는 노드의 테이블 형식 값(FR-O-015). CBS 실행은 이 값을 쓰지 않는다(D-22)
static constexpr size_t   MAX_L2_PER_CPU = 16;     // equals BPF PROMOTE_BUDGET (§4.4), so a due throttled server is never skipped
static constexpr size_t   MAX_TASK_ID_LEN = 15;    // Linux comm limit (TASK_COMM_LEN − 1)
```

`L_DEL_MAX_US`와 `DELTA_OVR_US`는 측정 전의 후보값이다. 두 값을 "증명된 최악값"으로 쓰지 않는다(§3.3, 개요 검토 의견).

내부 사유와 SRS-008 FR-O-004 사유 코드의 대응(P1은 로그, P2는 `Response.reason`):

| SRS 사유 코드 | `InfeasibleReason` |
|:--|:--|
| `PARAM_INVALID` | `ParamInvalid` (T > MIT, D > MIT 포함) |
| `UTIL_EXCEEDED` | `UtilizationExceeded`, `NoCbsBandwidth`, `SafetyCbsExceeded` |
| `DEADLINE_INFEASIBLE` | `DeadlineInfeasible`, `GapTooSmall` |
| `NO_CPU` | `NoCbsCpu`(CPU당 L2 개수 상한 초과 포함), `TtSlotConflict` |
| `NODE_UNKNOWN` | `NodeUnknown` |
| `NON_HARMONIC` | `NonHarmonicPeriod` |

### 3.2 `schedinfo_service.cpp` — 변환·검증·복원

```cpp
// ConvertToClassifiedTasks(): per task
if (mech == Mechanism::CBS) {
    t.mit_us = ti.min_inter_arrival_us();
    if (t.mit_us == 0 && ti.period() > 0) {            // transition rule (FR-O-001)
        t.mit_us = static_cast<uint32_t>(ti.period());
        TLOG_WARN("task ", ti.name(), ": min_inter_arrival_us=0, using period as MIT (deprecated)");
    }
    t.period_us   = ti.period() > 0 ? ti.period() : t.mit_us;      // T_s: the server period (NEW explicit rule)
    t.deadline_us = ti.deadline() > 0 ? ti.deadline() : t.mit_us;   // D, relative to arrival
    if (auto err = validate_l2_task(t)) return reject(*err);        // FR-O-003
}

std::optional<InfeasibleError> validate_l2_task(const ClassifiedTask& t) {
    if (t.task_id.empty() || t.task_id.size() > MAX_TASK_ID_LEN)
                                           return err(ParamInvalid, "task_id must be 1..15 bytes (Linux comm limit)");   // REQ-008 A-03
    if (t.mit_us == 0)                     return err(ParamInvalid, "MIT must be > 0");
    if (t.period_us == 0 || t.period_us > t.mit_us)
                                           return err(ParamInvalid, "T must satisfy 0 < T <= MIT");   // NEW: T_i <= MIT_i
    if (t.wcet_us == 0)                    return err(ParamInvalid, "WCET must be > 0");
    if (t.wcet_us > t.deadline_us)         return err(ParamInvalid, "WCET > deadline");
    if (t.deadline_us > t.mit_us)          return err(ParamInvalid, "D must satisfy D <= MIT (arbitrary deadline unsupported in P1)");   // NEW: D_i <= MIT_i
    return std::nullopt;
}

// RegenerateAllSchedules(): per node, before scheduling — task_id must be unique on the node (it is the SHM slot / comm key)
//   std::set<std::string> seen; for each task on the node: if (!seen.insert(t.task_id).second) → ParamInvalid "duplicate task_id on node"

// AddSchedInfo(): restore-on-reject (FR-O-005), unchanged
auto previous = workload_tasks_.find(id) != end ? std::optional{workload_tasks_[id]} : std::nullopt;
workload_tasks_[id] = tasks;
if (!RegenerateAllSchedules(err)) {
    if (previous) workload_tasks_[id] = *previous; else workload_tasks_.erase(id);
    RegenerateAllSchedules(ignored);         // tables return to last-good state
    TLOG_ERROR("reject ", id, " reason=", to_string(err.reason), " cpu=", err.cpu, " task=", err.task_id, ": ", err.details);   // FR-O-004 P1: structured log; [P2] Response.reason
    return status -1;
}
```

### 3.3 `global_scheduler.cpp` — 파이프라인과 수용 판정

```text
generate_schedule(node, tasks):
  1. split TT / CBS (unchanged). CBS: T_s = period (or MIT when no period), MIT, D, C
  2. global_hp = TT 있음 ? LCM(all TT periods on node) : DEFAULT_HP_US     // FR-O-015; 표현 변경은 OI-25
  3. assign_cpus(): TT round-robin (unchanged); check U_tt(k) ≤ U_BOUND (NEW)
     CBS worst-fit by U_s = C/T_s over residual (T_s replaces MIT in the utilization term:
     the server's bandwidth is C/T_s, and T_s ≤ MIT)
  4. per CPU k:
       place_tt_slots(k, global_hp)          // repeats = global_hp / period
       gaps_k   = compute_gaps(slots_k, global_hp)
       btt_k    = compute_tt_blocking(slots_k, global_hp)              // NEW
       admit_l2_fifo(k, cbs_tasks_k, u_tt_k, gaps_k, btt_k)            // replaces allocate_cbs_budgets
  5. build_table(): layer 1 TT, layer 2 EVENT_DRIVEN; table_id = "table_<gen>"
```

TT 슬롯이 없는 CPU(TT 태스크가 없는 노드 포함)도 4단계를 거쳐 자신의 `PerCpuSchedule`을 받는다(FR-O-022). 이 CPU는 `slots_k`가 비어 있으므로 `btt_k`는 0이고, `occupancy_within_us`는 빈 `merged_us`에서 0을 돌려준다. Gap은 전체 구간 하나다. `DEFAULT_HP_US`는 테이블 형식 값과 gap 계산의 horizon으로만 쓰며, CBS 실행 파라미터가 아니다(D-22). 이 CPU의 수용 판정은 $B_{tt} = 0$으로 한다(CBS-007).

```cpp
TtBlocking GlobalScheduler::compute_tt_blocking(std::vector<TtSlotPlacement> slots, uint32_t hp_us) {
    TtBlocking b; b.hp_us = hp_us;
    if (slots.empty()) return b;
    std::sort(slots.begin(), slots.end(), by_offset);
    for (auto& s : slots) {                                   // merge adjacent/overlapping [offset, offset+duration)
        uint32_t st = s.offset_us, en = s.offset_us + s.duration_us;
        if (!b.merged_us.empty() && st <= b.merged_us.back().second) b.merged_us.back().second = std::max(b.merged_us.back().second, en);
        else b.merged_us.push_back({st, en});
    }
    for (auto& m : b.merged_us) { b.max_contig_us = std::max(b.max_contig_us, m.second - m.first); b.total_us += m.second - m.first; }
    if (b.merged_us.size() > 1 && b.merged_us.back().second == hp_us && b.merged_us.front().first == 0)   // wrap
        b.max_contig_us = std::max(b.max_contig_us,
            (b.merged_us.back().second - b.merged_us.back().first) + (b.merged_us.front().second - b.merged_us.front().first));
    return b;
}

// B_tt(k, L): TT occupancy is a periodic step function, so the maximum over windows of fixed length L is attained
// when the window starts at a block start or ends at a block end. Evaluate both candidate sets over two hyperperiods.
uint32_t TtBlocking::occupancy_within_us(uint32_t L) const {
    if (merged_us.empty() || L == 0) return 0;
    if (L >= hp_us) return total_us * (L / hp_us) + occupancy_within_us(L % hp_us);                 // whole hyperperiods + remainder window
    auto occ = [&](uint64_t ws) {                            // occupancy of [ws, ws+L) over unrolled blocks
        uint64_t we = ws + L, sum = 0;
        for (int rep = 0; rep < 2; ++rep) for (auto& m : merged_us) {
            uint64_t s = m.first + (uint64_t)rep * hp_us, e = m.second + (uint64_t)rep * hp_us;
            if (e > ws && s < we) sum += std::min(e, we) - std::max(s, ws);
        }
        return sum;
    };
    uint64_t best = 0;
    for (auto& m : merged_us) {
        best = std::max(best, occ(m.first));                                        // window starts at block start
        best = std::max(best, occ(m.second >= L ? m.second - L : m.second + hp_us - L));   // window ends at block end
    }
    return static_cast<uint32_t>(best);
}

bool GlobalScheduler::admit_l2_fifo(int cpu, const std::vector<ClassifiedTask>& cbs_tasks, double u_tt,
                                    const std::vector<GapInterval>& gaps, const TtBlocking& btt,
                                    PerCpuSchedule& sched, InfeasibleError& error) {
    if (cbs_tasks.empty()) return true;                                     // TT-only CPU: nothing to admit
    if (cbs_tasks.size() > MAX_L2_PER_CPU) return fail(NoCbsCpu);          // FR-O-013 / FR-B-015
    // (1) utilization with the server period: sum of C_s / T_s (existing logic)
    double u_avail = U_BOUND - u_tt - U_OVERHEAD;
    if (u_avail <= 0.0) return fail(NoCbsBandwidth);
    // (2) min gap → reject (was warning)                                     FR-O-014
    if (!gaps.empty() && min_len(gaps) < CBS_MIN_EXEC_US) return fail(GapTooSmall);
    ...                                                                     // utilization admission in decreasing U_s order (existing)
    // (3) ONE admission inequality for EVERY L2 task on this CPU (existing tasks are re-checked,
    //     because a new task adds its C and δ_ovr to their interference term)   FR-O-013 (P1)
    for (const auto& t : cbs_tasks) {
        uint64_t sum_other = 0;                                             // Σ_{j≠i} (C_j + δ_ovr)
        for (const auto& j : cbs_tasks)
            if (j.task_id != t.task_id) sum_other += (uint64_t)j.wcet_us + DELTA_OVR_US;
        uint64_t bound = (uint64_t)t.wcet_us                                 // C_i
                       + btt.occupancy_within_us(t.deadline_us)              // B_tt(k, D_i)
                       + L_DEL_MAX_US                                        // L_del_max
                       + sum_other;
        if (bound > t.deadline_us) {
            error = InfeasibleError(InfeasibleReason::DeadlineInfeasible, cpu, t.task_id,
                "C+B_tt(D)+L_del_max+sum(C_j+delta_ovr)=" + std::to_string(bound) + "us > D=" + std::to_string(t.deadline_us) + "us");
            return false;
        }
    }
    // allocations: budget=wcet (C), period=T_s, mit=mit, deadline=D, cpu
    return true;
}
```

`assign_cpus()`의 CBS worst-fit 단계에서도 후보 CPU마다 조건 (2)/(3)과 개수 상한(`MAX_L2_PER_CPU`)을 사전 검사하여 통과하는 CPU만 후보로 둔다(FR-O-014). 모든 CPU가 실패하면 `NoCbsCpu`. 조건 (3)의 $\sum_{j \ne i} C_j$ 항은 BPF의 활성화 순서 큐와 보충 윈도우 규칙에서 온다(§4.3, DDR-008 D-04 (P-c)). δ_ovr 항은 다른 서버가 예산을 넘겨 실행하는 양의 상한이다(TIM-004 / TMG-004).

**단일 수용식(D-09, P1)**. 모든 L2 task `i`가 CPU `k`에서 만족해야 한다. 이 식은 명시적 Event 연결 여부와 무관하다.

$C_i + B_{tt}(k, D_i) + L_{del,max} + \sum_{j \ne i} (C_j + \delta_{ovr}) \le D_i$ 이고 $D_i \le MIT_i$ 이고 $T_i \le MIT_i$

- $MIT_i$는 `min_inter_arrival_us`이며, 없으면 주기를 쓴다(D-11 규칙). 그러므로 MIT를 선언하지 않은 task에서는 두 부가 조건이 자동으로 참이다.
- $L_{del,max}$ = 도달 시각 → 워커가 유휴 TT 없는 CPU에서 처음 디스패치될 때까지 = $L_{post}$(post → runnable, 새 타이밍 항목, 후보 50 µs) + $L_{wake}$(runnable → 첫 실행, 유휴이고 TT 없는 CPU의 TIM-001, 50 µs). TT 점유 중 깨움(TIM-002)은 이 합에 넣지 않는다. TT 점유는 $B_{tt}$ 항이 덮는다(A24). 후보 $L_{del,max}$ = 100 µs. 측정할 목표이며 증명된 최악값이 아니다. TIM-001, TIM-002, TIM-011은 커널 쪽 정의를 그대로 둔다.
- $\delta_{ovr}$ = 한 서버가 스로틀되기 전에 예산을 넘겨 실행할 수 있는 상한(TIM-004 / TMG-004). P1에서는 HZ=1000의 틱 하나인 1 ms를 후보로 쓴다. nohz_full CPU와 틱 재시작 지연은 측정 대상이다. 이 항은 이전 식이 틱 초과를 무시했다는 개요 검토 의견에 답한다.
- **결과**: 같은 CPU에 L2 task가 여러 개 있으면 수용 가능한 파라미터가 줄어든다. 위험이며 P2(HRTICK)로 완화한다.
- $D_i \le MIT_i$이면 적합한 도달에서 한 source의 살아 있는 Event는 최대 하나다. 따라서 자기 큐잉 항이 필요 없다. MIT 위반이 있으면 큐(QDEPTH 이하)가 쌓이고 뒤의 Job이 마감을 놓친다. 이는 방지하지 않고 보고한다.

이 식은 다음 가정 아래의 **충분조건**이다. 문서가 이 가정을 명시해야 한다.

1. TT task는 선언된 실행 시간 안에서 동작한다. TT 초과는 TT DMISS이며 L2 보장 밖이다.
2. $\delta_{ovr}$과 $L_{del,max}$는 측정된 값으로 성립한다(후보값은 측정 전까지 가정일 뿐이다).
3. source의 도달은 MIT를 지킨다.
4. Job은 블로킹하지 않는다(§1.2).
5. 어댑터는 도달 시점에 게시한다. 공급된 `t_arr`이 더 이르면 그만큼 $D$를 소비한다.
6. 각 Job의 CPU 수요는 선언 예산 $C_i$보다 작다(예산은 여유를 두고 선언한다).
7. 각 Job은 다음 도달 전에 워커가 잠들 만큼 일찍 끝난다(§4.5, P-a 경계 사례).
8. `tol`(`CBS_ACT_TOL_US`)은 측정된 활성화 지터를 덮는다.

- 명시적 Event 연결 여부와 무관하게 모든 L2는 같은 수용식을 통과한다. Event 기록이 없을 때 D는 커널 활성화 기준 수용 판정 파라미터이며 감시되는 Job 마감이 아니다. 따라서 $L_{post}$ 항은 추가 여유일 뿐이다.
- **FCW 검사**: $2000 + 2000 + 100 + 0 = 4100 \le 5000$ µs (같은 CPU에 다른 L2 task가 없을 때). 같은 CPU에 어떤 두 번째 L2 task가 있어도 기본 FCW 파라미터는 실패한다. $2000 + 2000 + 100 + (C_j + 1000) > 5000$이 $C_j \ge 0$에 대해 항상 성립하기 때문이다. 공존에는 다른 파라미터가 필요하다(§8, AC-008-16).
- **proto 변경 없음**: 큐 깊이와 payload 크기는 P1에서 빌드 상수다(§5.6). `deadline_us`와 MIT는 이미 테이블에 있고 슬롯에 게시된다.

### 3.4 `orchestrator_service.cpp` — 폴트 유형 보존

```cpp
static schedinfo::v1::FaultType map_fault_type(node::v1::FaultType t) {
    switch (t) {
    case node::v1::DMISS:          return schedinfo::v1::DMISS;
    case node::v1::BUDGET_EXCEED:  return schedinfo::v1::BUDGET_EXCEED;
    case node::v1::WATCHDOG:       return schedinfo::v1::WATCHDOG;
    case node::v1::MIT_VIOLATION:  return schedinfo::v1::MIT_VIOLATION;
    default:                       return schedinfo::v1::UNKNOWN;
    }
}
// FaultServiceClient::NotifyFault(...) gains timestamp_ns, count, flags → FaultInfo 6..8
// (job_seq / job_gen are not forwarded in P1 — §2.1, §12)
```

### 3.5 timpani-o 테스트 (GTest, `timpani-o/tests`)

| 테스트 | 검증 |
|:--|:--|
| `SchedInfoServiceTest.MitFieldPreferredOverPeriod` | FR-O-001 |
| `SchedInfoServiceTest.LegacyPeriodAsMitWarns` | FR-O-001 전환 규칙 |
| `SchedInfoServiceTest.RejectWcetGtDeadline` / `RejectDeadlineGtMit` / `RejectZeroMit` / `RejectTaskIdLongerThan15` / `RejectDuplicateTaskIdOnNode` | FR-O-003 |
| `SchedInfoServiceTest.RejectPeriodGtMit` (T=12 ms, MIT=10 ms → ParamInvalid) / `PeriodAbsentUsesMitAsServerPeriod` | FR-O-003, T_i ≤ MIT_i |
| `SchedInfoServiceTest.RejectRestoresPreviousWorkload` | FR-O-005 |
| `GlobalSchedulerTest.TtBlockingMergesAdjacentSlots` / `TtBlockingWrapAround` / `TtBlockingWindowSpansTwoSlots` (D=15 ms, TT 2 ms @ 10 ms → B_tt=4 ms) | compute_tt_blocking, occupancy_within_us |
| `GlobalSchedulerTest.TtBlockingWindowLongerThanHyperperiod` (D=25 ms, hp=10 ms, TT 2 ms → 2·2 ms + B_tt(5 ms)) | `total_us` 분기 |
| `GlobalSchedulerTest.FifoAdmission_FcwConfigPasses` (C=2000, B_tt=2000, L_del_max=100, D=5000 → 4100 ≤ 5000) | FR-O-013, 단일 식 |
| `GlobalSchedulerTest.FifoAdmission_SecondL2Rejected` (C=2000×2, B_tt=2000, D=5000 → 2000+2000+100+3000 = 7100 > 5000) | FR-O-013 |
| `GlobalSchedulerTest.FifoAdmission_DeltaOvrCounted` (C=1000×2, B_tt=0, D=3000 → 1000+0+100+2000 = 3100 > 3000 거부; δ_ovr 없이는 2100으로 통과하는 경계) | δ_ovr 항 (TIM-004) |
| `GlobalSchedulerTest.FifoAdmission_ExistingTaskRechecked` (기존 태스크의 조건이 새 태스크 때문에 깨지면 거부) / `MaxL2PerCpuEnforced` | FR-O-013 |
| `GlobalSchedulerTest.MinGapRejects` / `TtOnlyCpuWithSmallGapNotRejected` (L2가 없는 CPU의 50 µs gap) | FR-O-014 |
| `GlobalSchedulerTest.WorstFitUsesServerPeriod` (C/T_s로 CPU를 고른다. C/MIT로 고르지 않는다) | FR-O-014 |
| `GlobalSchedulerTest.CbsEntryCpuZeroIsExplicit` (`has_cpu()` && `cpu()==0`) | IF-ON-003 |
| `GlobalSchedulerTest.EventDrivenLayerEmitted` / `TtLayerHasNoCbsEntries` | FR-O-016 |
| `GlobalSchedulerTest.CbsOnlyNodeHyperperiodNonZero` / `SlotsRepeatPerGlobalHyperperiod` | FR-O-015 |
| `GlobalSchedulerTest.TtFreeCpuAdmitsL2WithZeroBtt` (TT 슬롯이 없는 CPU에 L2를 배치·수용하고 B_tt = 0으로 검사한다, Gap은 전체 구간. 계획된 테스트) | FR-O-022, CBS-007 |
| `GlobalSchedulerTest.TtUtilizationBoundEnforced` | FR-O-011 |
| `OrchestratorServiceTest.FaultTypePreserved_{BudgetExceed,MitViolation}` | FR-O-020 |
| `OrchestratorServiceTest.FaultCountForwarded` / (승인 후) `OrchestratorServiceTest.FaultFlagsForwarded` | FR-O-020, §2.1 |
| `PublicationCoordinatorTest.TableIdMonotonic` | IF-ON-006 |

---

## 4. BPF 스케줄러 상세 (`timpani-n/src/bpf`)

> **식별자와 의미(공통 규칙)**: BPF 상태 머신은 이제 **실행 구간**을 관측한다. 커널 활성화(잠들었던 태스크가 실행 가능해짐) → 첫 실행 → 자발적 잠듦으로 이루어진 구간이다. 이 구간은 Job이 아니다. Job 경계는 워커가 명시하며(§5.6, Job 로그), BPF는 Job을 세지 않는다. 식별자 `job_state`, `JOB_WAITING`, `JOB_ACTIVE`, `JOB_FLAG_*`, `JobRecord`, `job_trace_rb`, `job_fsm.h`, `job_fsm_on_*`는 9209bc5에서 이어받아 **이름만 유지**한다. 의미는 실행 구간이다. 필드 `job_seq`는 `run_seq`로, `t_done_ns`는 `t_sleep_ns`로 바꾼다(의미가 바뀐 필드이므로).

### 4.1 `maps.h` — 구조체 (전체 교체본)

```c
#define SCHED_TYPE_TT 0
#define SCHED_TYPE_CBS 1
#define SCHED_TYPE_BEST_EFFORT 2

/* L2 execution-interval state (DDR-008 D-02). JOB_WAITING = outside an execution interval (asleep);
 * JOB_ACTIVE = inside one (kernel activation → voluntary sleep). Not a Job state. */
#define JOB_WAITING 0
#define JOB_ACTIVE  1

/* JOB_FLAG_* (identifier kept): flags of the current execution interval (TaskMeta.job_flags) and of JobRecord.flags.
 * These are NOT FaultInfo.flags (§2.1). Retired bits, never reused in P1:
 *   bit 0  JOB_FLAG_MIT_VIOLATION — activation-based MIT check removed (D-05); MIT is counted at the adapter boundary
 *   bit 2  JOB_FLAG_EARLY_DMISS   — BPF no longer judges L2 deadlines (D-20)
 *   bit 3  JOB_FLAG_MARKER        — no marker mode; arrival source is explicit linkage only
 *   bit 4  JOB_FLAG_DMISS         — BPF no longer judges L2 deadlines (D-20); the TT DMISS path keeps its own FAULT_DMISS */
#define CBS_ACT_TOL_US  50u   /* tolerance of replenishment window rule 1 only (D-04). Candidate 50 µs = L_post, to be measured.
                                 It is no longer shared with an MIT check nor tied to L_wake_max */
#define PROMOTE_BUDGET  16    /* promotion scan bound = MAX_L2_PER_CPU (timpani-o admission) */
#define JOB_FLAG_THROTTLED     (1u << 1)   /* the server was throttled during this interval (enqueue / stopping) */
#define JOB_FLAG_CARRYOVER     (1u << 6)   /* interval started without a fresh budget (window not ended at activation, D-04 rule 1) */
#define JOB_FLAG_TASK_EXIT     (1u << 5)   /* JobRecord only: exit_task record */

struct TaskMeta {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 assigned_cpu;
    __u32 slot_duration_us;
    __u8  scheduling_type;
    __u8  layer;
    __u8  tt_runnable;
    __u8  job_state;          /* L2 only: JOB_WAITING / JOB_ACTIVE (execution interval) */
    __u64 activation_ns;      /* TT: last wake (unchanged); L2: t_act of the current execution interval */
    __u64 cgroup_id;
    /* ---- 008 additions (L2) ---- */
    __u32 bind_gen;           /* NEW: binding count the slot was activated with (§5.6, bind_gen = low 32 bits of gen). Copied into every record */
    __u32 job_flags;          /* JOB_FLAG_* of the current interval, reset at interval start */
    __u32 run_seq;            /* execution-interval counter within one binding (was job_seq); reset at bind */
    __u32 _pad1;
    __u64 t_start_ns;         /* first running after activation; 0 until set */
    __u64 queue_key_ns;       /* L2 DSQ rank: t_act of the current interval; promotion time after a throttle (D-08) */
    __u32 budget_used_us;     /* per interval, reset on activation */
    __u32 _pad2;
};
```

`mit_us`, `mit_viol_count`, `last_act_ns` 필드는 제거한다. 활성화 기준 MIT 검사에만 쓰였다. `TaskMeta` 레이아웃 변경은 같은 헤더를 쓰는 timpani-n 사용자 공간과 함께 빌드되므로 추가 호환 조치는 없다.

```c
struct CbsState {   /* unchanged (D-04); the fields are written by BPF only, except the initial values set at bind (§5.1) */
    __u64 task_id_hash;
    __u32 budget_us;
    __u32 period_us;          /* T_s */
    __u32 remaining_us;
    __u32 deadline_us;        /* kept for the TaskRegistry parameter comparison; not used by BPF (no L2 deadline judgement) */
    __u64 exec_start_ns;
    __u64 replenish_at_ns;    /* END of the current replenishment window (D-04); 0 = no window yet. Written by BPF only */
    /* ---- 008 additions ---- */
    __u32 exec_frac_ns;       /* sub-µs carry between stops (0..999) */
    __u32 throttle_episodes;  /* incremented on remaining 1→0 transition */
    __u32 throttled;          /* 1 while the task waits in dsq_throttled (set/cleared in enqueue, cleared at promotion) */
    __u32 _pad;
};

#define FAULT_DMISS 0                   /* TT path only (DDR-012). L2 deadlines are not judged in BPF (D-20) */
#define FAULT_BUDGET_EXCEED 1
#define FAULT_MIT_VIOLATION 2           /* retired: never emitted by BPF (D-05). The value is not reused */
#define FAULT_WAKEUP_LATENCY_EXCEED 3   /* reserved P2 */
#define FAULT_WATCHDOG 4                /* reserved P2 */

struct FaultEvent {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 cpu;
    __u8  fault_type;
    __u8  _pad[3];
    __u64 expected_deadline_ns;   /* TT DMISS: abs deadline; BUDGET_EXCEED: replenish_at (window end) */
    __u64 actual_completion_ns;   /* TT DMISS: t_done; BUDGET_EXCEED: stop time */
    __u32 run_seq;                /* interval the event belongs to (was job_seq) */
    __u32 flags;                  /* JOB_FLAG_* of that interval */
    __u64 activation_ns;          /* t_act of that interval */
};

struct JobRecord {               /* job_trace_rb payload (DDR-008 D-16). Name kept; content is an execution-interval record,
                                  * one per voluntary sleep, or one per task exit (JOB_FLAG_TASK_EXIT). Not a Job record */
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 bind_gen;               /* NEW: binding that produced the interval. TaskRegistry ignores exit records of an older binding */
    __u32 run_seq;                /* interval counter within the binding (was job_seq) */
    __u32 cpu;
    __u32 budget_used_us;
    __u64 t_act_ns;               /* kernel activation (runnable) */
    __u64 t_start_ns;             /* first run after activation; 0 if the interval never ran */
    __u64 t_sleep_ns;             /* voluntary sleep, or the exit time for JOB_FLAG_TASK_EXIT (was t_done) */
    __u32 flags;                  /* JOB_FLAG_* */
    __u32 _pad;
};

struct ReplenishKickArgs {       /* in/out context of timpani_replenish_kick (SYSCALL prog) */
    __u64 cbs_key;               /* in  */
    __u32 cpu;                   /* in  */
    __u32 remaining_us;          /* out: snapshot */
    __u64 replenish_at_ns;       /* out: snapshot (window end); 0 if the server is unknown */
    __u32 throttled;             /* out: 1 while the task waits in dsq_throttled */
    __u32 _pad;
};
```

> `TaskMeta`/`CbsState` 레이아웃 변경은 `timpani-n` 사용자 공간(같은 헤더)과 함께 빌드되므로 추가 호환 조치는 없다. TT 경로 필드 의미는 불변(NFR-003).
> 용량(NFR-001): `task_meta_map` 4096, `cbs_map` 1024, SHM 64 슬롯(TT와 L2 합계: L2 32개를 쓰면 TT는 32개까지) → 노드당 L2 ≥ 32 충족. CPU당 L2는 8 이상, 상한 `MAX_L2_PER_CPU`(16). CPU별 DSQ는 길이 제한이 없다.
> `cbs_map`의 `remaining_us`/`replenish_at_ns`/`exec_*`는 BPF만 쓴다. 사용자 공간은 엔트리를 새로 만들 때와 파라미터가 바뀔 때만 쓴다(§5.1 (C)).


### 4.2 DSQ 및 맵 추가 (`timpani.bpf.c`)


```c
#define DSQ_CBS_BASE       0x100ULL   /* per-CPU L2 ready queue: DSQ_CBS_BASE + cpu */
#define DSQ_THROTTLED_BASE 0x200ULL   /* per-CPU throttled holding queue */
#define DSQ_BE             103
#define MAX_ISOLATED_CPUS  64

static __always_inline __u64 dsq_cbs(__u32 cpu)       { return DSQ_CBS_BASE + cpu; }
static __always_inline __u64 dsq_throttled(__u32 cpu) { return DSQ_THROTTLED_BASE + cpu; }
static __always_inline bool  cpu_is_isolated(__u32 cpu) { return cpu < 64 && (isolated_cpu_mask & (1ULL << cpu)); }

struct { __uint(type, BPF_MAP_TYPE_RINGBUF); __uint(max_entries, 256 * 1024); } job_trace_rb SEC(".maps");

/* L2 DSQs are used as priority queues: insert only with scx_bpf_dsq_insert_vtime(); mixing FIFO and PRIQ insertion
 * on one DSQ is rejected by the kernel. Rank of dsq_cbs = TaskMeta.queue_key_ns (t_act; promotion time after a throttle).
 * Rank of dsq_throttled = CbsState.replenish_at_ns, so the promotion scan can stop at the first entry that is not due. */

volatile __u32 trace_enabled = 0;       /* .bss — toggled by JobTraceCollector::set_enabled() */
volatile __u64 fault_drop_count = 0;    /* .bss — ringbuf reserve failures (FR-B-023) */

SEC("struct_ops.s/init")
s32 BPF_PROG(init) {
    for (__u32 cpu = 0; cpu < MAX_ISOLATED_CPUS; cpu++) {
        if (!cpu_is_isolated(cpu)) continue;
        scx_bpf_create_dsq(dsq_cbs(cpu), -1);
        scx_bpf_create_dsq(dsq_throttled(cpu), -1);
    }
    scx_bpf_create_dsq(DSQ_BE, -1);
    return 0;
}
```


### 4.3 공용 헬퍼 — `job_fsm.h` (BPF·호스트 공용, 순수 함수)

상태 머신 규칙을 BPF 커널 코드와 GTest가 **같은 헤더**로 공유한다. 헤더는 맵 접근·kfunc 없이 `TaskMeta`/`CbsState` 포인터와 `now`만 받는다. 헤더에는 MIT 검사, 마감 판정, 이벤트 방출이 없다. 실행 구간의 상태 전이와 예산 규칙만 있다.

```c
/* timpani-n/src/bpf/job_fsm.h — no BPF helpers; compiled into both scx_timpani and timpani-n tests */

/* returns 1 if this wakeup starts an execution interval (WAITING → ACTIVE), 0 otherwise.
 * No MIT check here (D-05): arrivals are counted at the adapter boundary, not by activation spacing. */
static __always_inline int job_fsm_on_runnable(struct TaskMeta *m, __u64 now)
{
    if (m->job_state != JOB_WAITING) return 0;            /* ACTIVE: preemption or requeue inside the interval, not a new interval */
    m->job_state = JOB_ACTIVE;
    m->activation_ns = now; m->queue_key_ns = now; m->t_start_ns = 0;
    m->budget_used_us = 0; m->job_flags = 0; m->run_seq++;
    return 1;
}

/* Replenishment window, DDR-008 D-04 rule 1. Call at activation only. Returns 1 if the budget was refilled. */
static __always_inline int cbs_on_activation(struct CbsState *c, __u64 t_act)
{
    if (t_act + (__u64)CBS_ACT_TOL_US * 1000ULL >= c->replenish_at_ns) {    /* previous window ended (or first activation: 0) */
        c->remaining_us = c->budget_us; c->exec_frac_ns = 0;
        c->replenish_at_ns = t_act + (__u64)c->period_us * 1000ULL;
        return 1;
    }
    return 0;      /* window not ended: a short interval (blocking resume or early arrival, see §4.5).
                      Keep remaining and the window; the caller marks the interval CARRYOVER */
}

/* Promotion of a throttled server, D-04 rule 3. Returns 1 if the server may run (budget refilled, new window from now). */
static __always_inline int cbs_try_promote(struct CbsState *c, __u64 now)
{
    if (c->remaining_us != 0) { c->throttled = 0; return 1; }   /* defensive: not throttled */
    if (now < c->replenish_at_ns) return 0;                  /* window not ended yet */
    c->remaining_us = c->budget_us; c->exec_frac_ns = 0; c->throttled = 0;
    c->replenish_at_ns = now + (__u64)c->period_us * 1000ULL;
    return 1;
}
/* No other code refills remaining_us (rule 4): not enqueue, not stopping, and not user space while the task is in SCHED_EXT.
 * Enforcement looks only at the window state; no MIT flag or Job count is consulted (D-05). */

static __always_inline void job_fsm_on_running(struct TaskMeta *m, __u64 now)
{
    if (m->job_state == JOB_ACTIVE && m->t_start_ns == 0) m->t_start_ns = now;   /* FR-B-005 */
}

/* returns µs charged; updates CbsState; sets *throttled_now on 1→0 transition */
static __always_inline __u32 cbs_charge(struct CbsState *c, __u64 now, __u8 *throttled_now)
{
    *throttled_now = 0;
    if (c->exec_start_ns == 0) return 0;
    __u64 total = (now - c->exec_start_ns) + c->exec_frac_ns;
    __u32 us = (__u32)(total / 1000ULL);
    c->exec_frac_ns = (__u32)(total % 1000ULL);               /* FR-B-012: ns carry */
    c->exec_start_ns = 0;
    if (c->remaining_us == 0) return us;
    if (us >= c->remaining_us) { c->remaining_us = 0; c->throttle_episodes++; *throttled_now = 1; }
    else c->remaining_us -= us;
    return us;
}

/* Execution-interval end: a voluntary sleep (SCX_DEQ_SLEEP). Returns 1 when an interval ended and a record should be emitted.
 * No deadline judgement here (D-02, D-20): L2 deadlines are judged by TIMPANI-N DeadlineMonitor (§5.9). */
static __always_inline int job_fsm_on_quiescent(struct TaskMeta *m)
{
    if (m->job_state != JOB_ACTIVE) return 0;
    m->job_state = JOB_WAITING;
    return 1;
}
```

- `FsmEvent` 구조체와 `emit_fault` 출력 인자는 제거했다. 헤더는 이벤트를 만들지 않는다.
- 헤더 내용이 바뀌었으므로 호스트 테스트 대상도 바뀐다(§5.8, `JobFsmTest`).

### 4.4 ops 구현

```c
SEC("struct_ops/select_cpu")
s32 BPF_PROG(select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
    struct TaskMeta *m = meta_of(p);
    if (m && (m->scheduling_type == SCHED_TYPE_TT || m->scheduling_type == SCHED_TYPE_CBS)) {
        if (m->scheduling_type == SCHED_TYPE_TT) m->activation_ns = bpf_ktime_get_ns();   /* TT unchanged */
        if (cpu_is_isolated(m->assigned_cpu)) return (s32)m->assigned_cpu;               /* D-06 */
    }
    return prev_cpu;
}

SEC("struct_ops/runnable")
void BPF_PROG(runnable, struct task_struct *p, u64 enq_flags)
{
    struct TaskMeta *m = meta_of(p);
    if (!m || m->scheduling_type != SCHED_TYPE_CBS) return;
    __u64 now = bpf_ktime_get_ns();
    if (!job_fsm_on_runnable(m, now)) return;                                /* not an interval start */
    __u64 key = cbs_key_from_meta(m);
    struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
    if (c && !cbs_on_activation(c, now)) m->job_flags |= JOB_FLAG_CARRYOVER; /* D-04 rule 1; no fresh budget → mark the interval (PRT-007 (b)) */
}

SEC("struct_ops/enqueue")
void BPF_PROG(enqueue, struct task_struct *p, u64 enq_flags)
{
    struct TaskMeta *m = meta_of(p);
    if (!m) { scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags); return; }
    if (m->scheduling_type == SCHED_TYPE_TT) { /* unchanged: LOCAL_ON | acpu + PREEMPT kick */ return; }
    if (m->scheduling_type == SCHED_TYPE_CBS) {
        __u32 cpu = m->assigned_cpu;
        if (!cpu_is_isolated(cpu)) { scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags); return; }
        __u64 key = cbs_key_from_meta(m);
        struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
        if (!c) { scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags); return; }
        __u64 flags = cbs_enqueue_flags(enq_flags);
        /* No refill here (D-04 rule 4). The old cbs_lazy_replenish() call is removed. */
        if (c->remaining_us == 0) {                                          /* throttled: wait for the window end */
            m->job_flags |= JOB_FLAG_THROTTLED; c->throttled = 1;
            scx_bpf_dsq_insert_vtime(p, dsq_throttled(cpu), SCX_SLICE_DFL, c->replenish_at_ns, flags);
        } else {                                                             /* ready: activation order (D-08, FR-B-016) */
            c->throttled = 0;
            scx_bpf_dsq_insert_vtime(p, dsq_cbs(cpu), SCX_SLICE_DFL, m->queue_key_ns, flags);
        }
        return;
    }
    scx_bpf_dispatch(p, DSQ_BE, SCX_SLICE_DFL, enq_flags);
}

static __always_inline void promote_throttled_if_replenished(__u32 cpu, __u64 now)
{
    /* Modelled on the current promotion loop (timpani-n/src/bpf/timpani.bpf.c, promote_throttled_if_replenished):
     * the iterator, the PROMOTE_BUDGET bound and the lookup order are kept; the per-CPU queues and cbs_try_promote() are new. */
    struct bpf_iter_scx_dsq it;
    struct task_struct *p;
    int scanned = 0;

    bpf_iter_scx_dsq_new(&it, dsq_throttled(cpu), 0);
    while ((p = bpf_iter_scx_dsq_next(&it))) {
        struct TaskMeta *m;
        struct CbsState *c;
        __u64 key;

        if (scanned++ >= PROMOTE_BUDGET)                /* bounded scan (NFR-002) */
            break;
        m = meta_of(p);
        if (!m || m->scheduling_type != SCHED_TYPE_CBS)
            continue;
        key = cbs_key_from_meta(m);
        c = bpf_map_lookup_elem(&cbs_map, &key);
        if (!c)
            continue;
        /* ordered by replenish_at: the first server not yet due ends the scan */
        if (!cbs_try_promote(c, now))
            break;
        m->queue_key_ns = now;                          /* D-08: a promoted job goes to the tail */
        scx_bpf_dsq_move_set_vtime(&it, now);
        scx_bpf_dsq_move_vtime(&it, p, dsq_cbs(cpu), 0);   /* no kick: we are on cpu */
    }
    bpf_iter_scx_dsq_destroy(&it);
    /* PROMOTE_BUDGET == MAX_L2_PER_CPU, so every due server on this CPU is promoted in one pass (FR-B-015). */
}

SEC("struct_ops/dispatch")
void BPF_PROG(dispatch, s32 cpu, struct task_struct *prev)
{
    __u64 now = bpf_ktime_get_ns();
    if (tt_active_on_cpu((__u32)cpu, now)) return;          /* TT-first (DDR-007). TT 슬롯이 없는 CPU에서는 항상 거짓이다(FR-B-025) */
    if (!cpu_is_isolated((__u32)cpu)) { scx_bpf_consume(DSQ_BE); return; }   /* fix: DSQ_BE now consumable */
    promote_throttled_if_replenished((__u32)cpu, now);
    if (scx_bpf_consume(dsq_cbs((__u32)cpu))) return;
}

SEC("struct_ops/running")
void BPF_PROG(running, struct task_struct *p)
{
    struct TaskMeta *m = meta_of(p); if (!m) return;
    if (m->scheduling_type != SCHED_TYPE_CBS) return;                        /* TT path unchanged */
    __u64 now = bpf_ktime_get_ns();
    job_fsm_on_running(m, now);
    __u64 key = cbs_key_from_meta(m);
    struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
    if (!c) return;
    c->exec_start_ns = now;                                                  /* start of this on-CPU stretch; cbs_charge() consumes and clears it in stopping */
    /* slice cap (unchanged): p->scx.slice = max(1 µs, min(remaining_us, µs until the next TT slot on this CPU)).
       TT 슬롯이 없는 CPU에서는 다음 TT 슬롯이 없으므로 상한은 remaining_us뿐이다(FR-B-025) */
}

SEC("struct_ops/stopping")
void BPF_PROG(stopping, struct task_struct *p, bool runnable)
{
    struct TaskMeta *m = meta_of(p); if (!m) return;
    __u64 now = bpf_ktime_get_ns();
    if (m->scheduling_type == SCHED_TYPE_CBS) {
        __u64 key = cbs_key_from_meta(m);
        struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
        if (c) {
            __u8 throttled_now;
            m->budget_used_us += cbs_charge(c, now, &throttled_now);
            if (throttled_now) {                                             /* FR-B-013: once per episode */
                /* replenish_at_ns (window end) is left unchanged; dispatch promotes once now >= replenish_at_ns (D-04 rule 3).
                 * If the window has already ended, the next dispatch on this CPU promotes at once. */
                m->job_flags |= JOB_FLAG_THROTTLED;
                emit_fault(m, FAULT_BUDGET_EXCEED, c->replenish_at_ns, now);
            }
        }
        return;
    }
    /* TT path: unchanged (mark_tt_not_runnable + current_slot deadline check, DDR-012).
     * The early-DMISS judgement that used to run here for L2 tasks is removed (D-20). The runnable argument is no longer used. */
}

SEC("struct_ops/quiescent")
void BPF_PROG(quiescent, struct task_struct *p, u64 deq_flags)
{
    struct TaskMeta *m = meta_of(p);
    if (!m || m->scheduling_type != SCHED_TYPE_CBS) return;
    if (!(deq_flags & SCX_DEQ_SLEEP)) return;                                /* only a voluntary sleep ends an interval (FR-B-003) */
    if (job_fsm_on_quiescent(m) && trace_enabled)
        emit_job_record(m, bpf_ktime_get_ns(), 0);                           /* no MIT check, no L2 DMISS (D-02, D-20) */
}

SEC("struct_ops/exit_task")
void BPF_PROG(exit_task, struct task_struct *p, struct scx_exit_task_args *args)
{
    struct TaskMeta *m = meta_of(p); if (!m) return;
    emit_job_record(m, bpf_ktime_get_ns(), JOB_FLAG_TASK_EXIT);             /* always, not gated: carries bind_gen and task_id_hash for TaskRegistry (D-16 exception) */
}

/* userspace-triggered kick (DDR-008 D-07 option 1). Reads the server state, never writes it. */
SEC("syscall")
int timpani_replenish_kick(struct ReplenishKickArgs *args)
{
    struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &args->cbs_key);
    args->remaining_us = 0; args->replenish_at_ns = 0; args->throttled = 0;
    if (!c) return 0;
    __u64 now = bpf_ktime_get_ns();
    __u64 rep = c->replenish_at_ns; __u32 rem = c->remaining_us; __u32 thr = c->throttled;
    if (thr && now >= rep && !tt_active_on_cpu(args->cpu, now))   /* a task waits in dsq_throttled, its window has ended, and no TT slot is running */
        scx_bpf_kick_cpu((s32)args->cpu, 0);                 /* reschedule request → ops.dispatch → promote. 0, not SCX_KICK_IDLE:
                                                                a CPU that is just going idle must not miss the kick.
                                                                During a TT slot no kick is sent: dispatch would return at the TT gate,
                                                                and the dispatch at the end of the slot promotes anyway */
    args->remaining_us = rem; args->replenish_at_ns = rep; args->throttled = thr;   /* copied back to user space */
    return 0;
}

SEC(".struct_ops.link")
struct sched_ext_ops timpani_ops = {
    .flags = SCX_OPS_SWITCH_PARTIAL,
    .init = (void*)init, .init_task = (void*)init_task, .exit_task = (void*)exit_task,
    .select_cpu = (void*)select_cpu, .enqueue = (void*)enqueue, .dispatch = (void*)dispatch,
    .runnable = (void*)runnable, .running = (void*)running, .stopping = (void*)stopping,
    .quiescent = (void*)quiescent, .name = "timpani" };
```

- `emit_job_record(m, now, extra_flags)`는 `JobRecord{workload_id_hash, task_id_hash, bind_gen = m->bind_gen, run_seq = m->run_seq, cpu, budget_used_us, t_act_ns = m->activation_ns, t_start_ns = m->t_start_ns, t_sleep_ns = now, flags = m->job_flags | extra_flags}`를 `job_trace_rb`에 제출한다. 실패는 기록 손실로만 보고한다(FR-B-023).
- `emit_fault()`는 `FaultEvent.run_seq`, `activation_ns`, `flags`를 `m` 기준으로 채우며, `bpf_ringbuf_reserve` 실패 시 `fault_drop_count++`(FR-B-023)다.
- `runnable`(TT 및 L2 DMISS 판정 제거 뒤의 인자)은 `stopping`에서 쓰지 않는다. struct_ops 시그니처를 맞추기 위해 인자는 남긴다.

### 4.5 검증기(verifier)·호환 메모
- `ops.runnable`/`ops.quiescent`/`ops.exit_task`는 sched_ext 6.12 struct_ops 멤버로 존재한다. `SCX_DEQ_SLEEP`은 vmlinux.h enum.
- `select_cpu`는 `nr_cpus_allowed == 1`인 태스크에 대해 호출되지 않을 수 있으므로 활성화 기록을 `runnable`로 옮긴 것이 정확성에도 필요하다.
- `promote_throttled_if_replenished` 반복 상한 `PROMOTE_BUDGET=16`(SRS NFR-002). TIMPANI-O가 CPU당 L2 태스크를 16개 이하로 수용하므로(§3.3) 윈도우가 끝난 서버가 스캔에서 빠지지 않는다.
- 예산 상태(`remaining_us`, `replenish_at_ns`)를 쓰는 곳은 `runnable`(활성화), `stopping`(차감), `dispatch`(승격)뿐이며 모두 태스크가 속한 배정 CPU의 런큐 잠금 아래에서 실행된다(L2 태스크는 배정 CPU의 런큐에만 있다는 D-06 전제). `timpani_replenish_kick`은 읽기만 한다.
- kfunc 이름은 현재 빌드(`TIMPANI_SCX_USE_NEW_DSQ_API`)가 쓰는 새 DSQ API로 통일한다: `scx_bpf_dsq_insert`, `scx_bpf_dsq_insert_vtime`, `scx_bpf_dsq_move`, `scx_bpf_dsq_move_set_vtime`, `scx_bpf_dsq_move_vtime`, `scx_bpf_dsq_move_to_local`. 이름은 `scx/compat.bpf.h`의 호환 매크로로 감싸고, 시그니처는 대상 커널의 `vmlinux.h`로 확인한다.
- `timpani_replenish_kick`의 `scx_bpf_kick_cpu` 호출 가능 여부와 컨텍스트 반환(`ctx_in`으로 다시 복사)은 커널에 의존 → **OI-9 PoC**(항목: DDR-008 D-07). 불가 시 §5.3 `KickBackend::KickerThreads`.
- `job_fsm.h`는 `__always_inline`·고정폭 타입만 사용하며 `bpf_helpers.h` 미포함 → 호스트 컴파일 가능.
- 실행 구간의 한계(REQ-008 §2, DDR-008 D-02): 미소비 Event가 있어 `ttsched_evt_wait()`가 수면 없이 복귀하면(R3) `quiescent`가 호출되지 않으므로 두 연속 Job은 **하나의 실행 구간**으로 관측된다. BPF는 Job 경계를 알 수 없다. Job 경계는 워커가 `wait`/`done`으로 알리며 SHM Job 로그에 기록된다(§5.4(a), §5.6). 실행 구간 레코드와 Job 기록은 `t_arr`·`t_begin`·`t_end` 기준으로 시각만 대응시킨다(DDR-008 D-16). 대응은 Job이 블로킹하지 않을 때 1:1이다.
- 연속 Job의 예산: 두 번째 Job이 같은 실행 구간 안에서 시작하면 새 예산을 받지 않는다(D-04 규칙 4). 구간이 끝나기 전 남은 예산을 함께 쓴다. 그 결과 뒤의 Job이 스로틀되거나 마감을 놓칠 수 있다. 이는 방지가 아니라 보고한다.
- 예산 차감의 단위: `running`~`stopping` 사이의 on-CPU 시간을 ns로 누산해 µs로 차감한다. 차감은 활성화부터 워커가 잠들 때까지(대기 진입 시스템 콜 경로 포함)의 실행을 모두 포함하며, 차감액이 잔여 예산에 **도달하면**(`us >= remaining_us`) 스로틀한다. 따라서 선언 예산 $C_s$는 한 Job이 실행 구간들에 걸쳐 쓰는 차감 실행 시간의 합보다 커야 한다(수용 판정 가정 6, DDR-008 D-04 (P-a)). 같거나 작으면 대기 진입 직전에 스로틀될 수 있다.
- 위반 후 동작(윈도우 용어): 윈도우가 끝나기 전에 활성화된 실행 구간은 `JOB_FLAG_CARRYOVER`를 갖고 남은 예산으로 실행된다. 도달이 MIT보다 가까웠거나 Job이 마감을 넘긴 경우가 여기에 해당한다. 서버가 도달 위상으로 돌아오는 과정은 DDR-008 D-04 "위반 후 동작"을 따른다. 그 예시는 윈도우 타임라인으로 기술한다(§5.8).
- 블로킹 Job: Job 중 블로킹하면 잠듦으로 실행 구간이 나뉜다. 블로킹 뒤의 깨움은 커널 활성화이며 열린 윈도우 안에 있으면 예산을 새로 주지 않는다(`cbs_on_activation`, 단 CARRYOVER로 표시). 블로킹 Job의 나뉜 구간은 Job 경계가 아니다. Job 경계는 SHM Job 로그가 전달한다. 수용 판정은 블로킹 Job에 보장을 주지 않는다(§3.3 가정 4).

---

## 5. TIMPANI-N 사용자 공간 상세

> 공통 규칙: TimerMaster가 SHM 슬롯 레이아웃을 소유한다(§5.6). 이 절과 §6의 상수는 §5.6에서 정의하고 §6은 이를 참조한다. 모든 SHM 원자 연산은 `seq_cst`다. RT 경로(TimerMaster 스레드, DeadlineMonitor, Replenisher, post, wait, done)에는 gRPC·할당·블로킹 I/O가 없다. 바인딩과 닫기는 비 RT 경로(테이블 적용 스레드 또는 TaskRegistry 스레드)에서 `bind_mu_`로 직렬화된다.

### 5.1 `main.cpp` — 테이블 적용 분기 (변경 요지)

```cpp
// (A) 레이어 모델 분기 (unchanged)
for (const auto& layer : partition.layers()) {
    if (layer.model() == EVENT_DRIVEN || layer.cbs_entries_size() > 0) apply_event_layer(partition, layer);
    if (layer.model() == TIME_TRIGGERED || layer.tt_slots_size() > 0)  apply_tt_layer(partition, layer);   // existing body
}

// (B0) NEW: pin the SHM slots of every L2 entry FIRST. Activation (below) needs the slot index.
//      Pinning only allocates a name → slot mapping; it changes no kind, configuration or counter (§5.6).
std::vector<EvtTaskEntry> evt_tasks = collect_l2_entries(partition);   // name, hashes, mit_us, deadline_us of every CbsConfig
EvtSlotTable evt_slots = timer_master.update_evt_slots(evt_tasks);    // same method name, narrowed meaning

// (B) apply_event_layer(): per CbsConfig e
if (!e.has_cpu() || !is_isolated_cpu(e.cpu())) {                                   // IF-ON-003: no partition-cpuset fallback
    apply_error("CbsConfig.cpu missing or not an isolated CPU: " + e.task_id()); continue;
}
uint32_t cpu = e.cpu();                                                            // CPU 0 is a valid explicit value
uint32_t mit = e.min_inter_arrival_us() ? e.min_inter_arrival_us() : e.period_us();   // FR-O-001 transition (O already resolves it)
L2Binding b = make_l2_binding(e, cpu, mit);                                        // {task_id, key, hashes, cpu, mit, deadline_us, period_us (T_s), budget_us (C)}
servers.push_back({key(e), cpu, (uint64_t)e.period_us() * 1000ULL, 0});            // → Replenisher (unchanged)
fault_monitor.set_task_params(key(e), e.period_us(), e.current_limit());           // T_s window for MIT reports; DDR-012 threshold
if (auto t = registry.tracked(e.task_id())) {                                      // already bound by an earlier table
    if (t->binding == b) continue;                                                 // D-17(a): unchanged → write NOTHING. No slot change, no re-activation
    unbind_l2_task(t->tid, t->binding, /*worker_exited=*/false);                    // changed: §5.1 (E) order; the in-flight Job is closed by the next activation (§5.6 P1)
    registry.remove_expected(e.task_id());                                         // forget the old (tid, parameters) so a later exit scan cannot bring them back
}
auto r = registry.resolve_now(e.task_id());                                        // std::optional<TaskRegistry::Resolved>
if (!r) { registry.add_unresolved(e.task_id(), b); continue; }                     // FR-N-003 (not an apply error)
if (auto g = bind_l2_task(r->tid, b)) registry.track(e.task_id(), r->tid, b, *g);  // bind_gen returned by the binder (§5.2)

// (C) bind_l2_task(tid, b): returns the bind_gen on success. Serialised by bind_mu_ (table apply thread and TaskRegistry
//     rescan thread may both call it). The task is NOT in SCHED_EXT yet when this begins.
std::optional<uint32_t> bind_l2_task(pid_t tid, const L2Binding& b) {
    std::lock_guard<std::mutex> lk(bind_mu_);
    const CpuSet allowed = allowed_cpus(tid);                                      // sched_getaffinity(tid); empty set if the read fails
    if (!allowed.contains(b.cpu)) {                                                // FR-N-033, CBS-008: before any write; the rescan binder (§5.2) runs this too
        log_error_rate_limited("assigned CPU not in allowed set: task=" + b.task_id + " cpu=" + std::to_string(b.cpu) + " allowed=" + allowed.str());   // logged, not apply_error
        registry.add_unresolved(b.task_id, b);                                     // stays unresolved; retried at the rescan period (FR-N-003)
        return std::nullopt;                                                       // no SCHED_EXT switch, no BPF state, no slot activation
    }
    CbsState st{}; st.budget_us = b.budget_us; st.period_us = b.period_us; st.deadline_us = b.deadline_us;
    st.remaining_us = b.budget_us; st.replenish_at_ns = 0; st.throttled = 0;      // no window yet: the first activation opens it (D-04 rule 1)
    bpf_loader.update_cbs_state(b.key, st);                                        // 1. the server first
    uint32_t bind_gen = timer_master.next_bind_gen(b.task_id);                     // 2. the gen the activation will publish (§5.6); not published yet
    TaskMeta meta{}; meta.scheduling_type = SCHED_TYPE_CBS; meta.assigned_cpu = b.cpu;
    meta.job_state = JOB_WAITING; meta.bind_gen = bind_gen; /* ... */
    bpf_loader.update_task_meta(tid, meta);                                        // 3. then the task meta (bind_gen inside)
    if (!apply_sched_ext(tid)) {                                                   // 4. class switch LAST: a wakeup right after it finds its server and meta
        bpf_loader.delete_task_meta(tid); bpf_loader.delete_cbs_state(b.key);
        apply_error("sched_ext switch failed: " + b.task_id); return std::nullopt;   // no activation: the slot stays NONE
    }
    EvtActivation a = timer_master.activate_evt(b.task_id, {b.mit, b.deadline_us, b.key}, bind_gen);   // 5. slot activation (protocol 1)
    fault_monitor.on_activated(b.key, a.gen, a.base_missed, a.base_mit_viol);     // count-path baselines (§5.5)
    deadline_monitor.on_activated(a.slot, b.key);                                   // per-slot BPF key table (§5.9)
    return bind_gen;
}

// unbind_l2_task(tid, b, worker_exited): the order matters (§5.6 protocol 5)
//   1. sched_setattr(tid, SCHED_OTHER) — the kernel takes the task out of the L2 DSQs (ESRCH on an exited task is ignored)
//   2. timer_master.deactivate_evt(b.task_id, worker_exited) — kind = NONE, closing sweep
//   3. delete task_meta_map[tid] and cbs_map[b.key]
// These are the only user-space writes to cbs_map / task_meta_map: always while the task is outside SCHED_EXT.

// (D) after loops — TT layer only (L2 slots are activated per binding above)
timer_master.same_tt_table(slots, hp, epoch)
    ? timer_master.keep_tt_threads()                                    // FR-N-006 / D-17(b): L2-only change → no TT thread restart
    : timer_master.set_schedule_table(slots, hp, epoch);
replenisher.set_servers(servers);
registry.start_rescan_if_pending();
job_trace.set_task_names(hash → name);                                  // for CSV output
event_sources.set_shm(timer_master.shm(), evt_slots);                   // EventSourceMonitor reads cumulative counters (§5.4(b))
node_client.send_table_applied(table.table_id(), apply_success, apply_error);
```

`allowed_cpus(tid)`는 `sched_getaffinity(tid)`로 허용 집합(`CpuSet`)을 읽는다. 읽기에 실패하면 빈 집합을 돌려주며, 그러면 바인딩하지 않는다. 이 검사는 `bind_l2_task`(C)의 첫 단계에서 하므로 테이블 적용 경로와 재식별 경로(§5.2 `rescan_loop`)가 모두 거친다. 이미 미해결 목록에 있는 이름을 다시 넣어도 목록은 바뀌지 않는다. 허용 CPU 검사(FR-N-033, CBS-008)는 위 (B)의 Isolated CPU 검사와 다르다. Isolated CPU 검사는 설정 오류이므로 `apply_error`로 기록한다. 허용 집합 검사는 워커 쪽 배치 문제이므로 `apply_error`를 쓰지 않고 로그를 남긴 뒤 미해결 목록에 둔다. 이 경우 `bind_l2_task`는 어떤 쓰기도 하기 전에 `nullopt`를 돌려주므로 SCHED_EXT 전환, BPF 상태 등록과 Timer Master 활성화가 없다. 바인딩 경로는 스레드의 affinity나 cgroup을 바꾸지 않는다. SCHED_EXT 전환 실패의 처리는 바꾸지 않는다.

제거·STOP 경로(`RecoverySignal`, 테이블 소멸)는 `unbind_l2_task(..., worker_exited=false)`를 호출한 뒤 `replenisher.remove(key)`, `fault_monitor.clear_task(key)`, `deadline_monitor.on_deactivated(slot)`, `job_trace.clear_task(key)`, `event_sources.clear_task(key)`, `registry.remove_expected(name)`를 수행한다(FR-N-005). 이 경로에서 `RUNNING` 기록은 `ABANDONED`가 되지 않고 마감 감시 아래 남는다. 그 뒤 워커가 끝내면 `done()`이 기록한다(§5.6 프로토콜 5). `kind = NONE` 뒤에도 가져가기 CAS가 닫기 sweep보다 먼저 성공한 Job은 RUNNING으로 남아 워커가 끝까지 처리한다(§6.4, A12).

### 5.2 `task_registry.{h,cpp}` — 재설계

```cpp
class TaskRegistry {
public:
    struct Resolved { pid_t tid; bool is_thread; };
    using OnResolved = std::function<void(const std::string& name, Resolved r)>;
    using OnExited   = std::function<void(const std::string& name, pid_t tid)>;

    explicit TaskRegistry(std::chrono::milliseconds rescan_period = 500ms);
    std::optional<Resolved> resolve_now(const std::string& comm);          // process first, then thread (PR #51 logic moved here)
    // Binder = bind_l2_task (§5.1 (C)). Returns the bind_gen on success, nullopt on failure. Set once at start-up.
    using Binder = std::function<std::optional<uint32_t>(pid_t tid, const L2Binding& b)>;
    void set_binder(Binder b);
    void add_unresolved(const std::string& comm, const L2Binding& b);      // not found yet: bind it when it appears
    void remove_expected(const std::string& comm);                         // entry left the table: forget it (unresolved or tracked)
    void track(const std::string& comm, pid_t tid, const L2Binding& b, uint32_t bind_gen);   // NEW bind_gen. Replaces an existing entry of the same name
    struct Tracked { pid_t tid; L2Binding binding; uint32_t bind_gen; };
    std::optional<Tracked> tracked(const std::string& comm) const;         // table apply: is this task already bound, and with which parameters
    void notify_exit(uint64_t task_id_hash, uint32_t bind_gen);            // from JobTraceCollector (JOB_FLAG_TASK_EXIT record carries the hash and bind_gen)
                                                                           // NEW: a record whose bind_gen is not the tracked binding's is STALE and ignored (counted)
    void start_rescan_if_pending();                                        // table-apply thread: wake the rescan thread if anything is unresolved or tracked
    void set_on_exited(OnExited cb);
    void start(); void stop();                                             // rescan thread (default policy, non-isolated)
private:
    void rescan_loop();      // while unresolved non-empty or tracked non-empty: scan /proc every period
    std::map<std::string, L2Binding> unresolved_;   // name → parameters to bind with
    std::map<std::string, Tracked>   tracked_;      // name → (tid, parameters, bind_gen). Exit detection walks the tids
    Binder binder_;
    std::mutex mu_; std::thread th_; std::atomic<bool> running_;
    uint64_t stale_exit_ignored_ = 0;               // counted, for the statistics log
};
```

`rescan_loop()`: (1) `unresolved_` 각 이름에 대해 `resolve_now()`; 성공 시 `binder_(tid, binding)`을 실행하고 값이 있으면 `tracked_`로 옮긴다(`bind_gen` 포함). (2) `tracked_` 각 TID에 대해 `/proc/<tid>` 존재를 확인한다. 없으면 `on_exited(name, tid)`를 호출한다. 그 호출은 닫기 규칙을 실행한다(`deactivate_evt(name, worker_exited=true)`, 즉 RUNNING → ABANDONED). 그 뒤 (name, binding)을 `unresolved_`로 되돌린다(FR-N-004). (3) 두 집합이 모두 비면 다음 테이블까지 휴면한다. 되돌릴 때 같은 이름이 이미 `unresolved_`에 있으면 덮어쓰지 않는다(테이블이 넣은 최신 파라미터가 우선이다).

`notify_exit(task_id_hash, bind_gen)`은 `tracked_`에서 같은 `task_id_hash`를 찾는다. **`bind_gen`이 추적 중인 바인딩과 다르면 오래된 기록으로 보고 무시한다**(`stale_exit_ignored_++`). 같은 바인딩이면 `on_exited`와 같은 경로를 탄다. 이것이 지연된 종료 기록이 새 바인딩을 비활성화하지 못하게 하는 규칙이다.

`set_binder(bind_l2_task)`는 시작 시퀀스에서 `start()`보다 먼저 한 번 호출한다. binder와 `on_exited` 콜백은 `mu_`를 **놓은 뒤** 호출한다(잠금 아래에서는 호출 대상의 스냅샷만 만든다). 콜백이 `track()`/`add_unresolved()`로 레지스트리에 다시 들어와도 교착하지 않는다. binder는 `bind_mu_`(§5.1 (C))를 잡으므로 테이블 적용 스레드와 직렬화된다. `T_bind` ≤ 주기 + 스캔 시간(≈ 수 ms) ≤ 1 s.

### 5.3 `replenisher.{h,cpp}` — 신규 (P1 설계에서 변경 없음)

이 컴포넌트는 P1 변경에서 바뀌지 않는다. 보충 윈도우 규칙(D-04)과 kick 프로토콜(D-07)은 Job 경계와 무관하다. TT 슬롯이 없는 CPU에서도 같은 방식으로 동작하며 TT 이벤트나 hyperperiod를 실행 조건으로 쓰지 않는다(FR-N-032, D-22). 마감 감시(DeadlineMonitor, §5.9)와 스레드를 합칠지는 열린 항목(F-12)이며 이 절의 결정이 아니다. 본문은 9209bc5 그대로 둔다.


```cpp
class Replenisher {
public:
    struct Server { uint64_t cbs_key; uint32_t cpu; uint64_t period_ns; uint64_t next_ns; };   // next_ns = next check time
    enum class KickBackend { BpfSyscallProg, KickerThreads };
    static constexpr uint64_t REPLENISH_RETRY_NS = 1'000'000;   // re-check interval for a server that is due but not promoted yet
    Replenisher(BpfLoader& bpf, KickBackend backend, int fifo_prio = 98);   // priority: default TM−1, to be confirmed by the PoC
    void set_servers(std::vector<Server> servers);   // table-apply thread (non-RT): builds the new heap OUTSIDE mu_, swaps it in under mu_
    void remove(uint64_t cbs_key);                   // under mu_: erase in place (no allocation), re-heapify; marks the in-flight server if it is this one
    void start();
    void stop();                                     // under mu_: running_ = false, broadcast cv_; then join
private:
    struct Snapshot { bool valid; bool throttled; uint32_t remaining_us; uint64_t replenish_at_ns; };
    void loop();                                     // SCHED_FIFO, non-isolated CPU
    Snapshot kick(const Server& s);                  // asks BPF to kick if due; returns the server snapshot. Never writes cbs_map
    pthread_mutex_t mu_;                             // PTHREAD_PRIO_INHERIT: the RT loop never waits behind a preempted normal thread
    pthread_cond_t  cv_;                             // CLOCK_MONOTONIC (pthread_condattr_setclock)
    std::vector<Server> heap_;                       // min-heap by next_ns (std::push_heap/pop_heap). Read and written under mu_ only
    uint64_t epoch_ = 0;                             // bumped by set_servers() only, guarded by mu_
    uint64_t inflight_key_ = 0;                      // the server the loop has popped and is kicking right now (guarded by mu_)
    bool inflight_valid_ = false, inflight_removed_ = false;
    bool running_ = false;                           // guarded by mu_
};

void Replenisher::set_servers(std::vector<Server> servers) {
    uint64_t now = mono_now();
    for (auto& s : servers) s.next_ns = now;                          // check every server once right away, then follow its window
    std::make_heap(servers.begin(), servers.end(), ByNext{});         // allocation and sorting happen here, outside mu_
    pthread_mutex_lock(&mu_);
    heap_.swap(servers); ++epoch_;                                    // short critical section: pointer swap only
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
}                                                                     // the old storage is freed here, on the caller's thread

void Replenisher::remove(uint64_t key) {
    pthread_mutex_lock(&mu_);
    heap_.erase(std::remove_if(heap_.begin(), heap_.end(), [&](const Server& s){ return s.cbs_key == key; }), heap_.end());   // shrinks: no allocation
    std::make_heap(heap_.begin(), heap_.end(), ByNext{});
    if (inflight_valid_ && inflight_key_ == key) inflight_removed_ = true;   // the loop holds THIS server right now: it must not push it back.
                                                                      // Other servers are unaffected (no epoch bump here)
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
}

void Replenisher::loop() {
    pthread_mutex_lock(&mu_);
    while (running_) {
        if (heap_.empty()) { pthread_cond_wait(&cv_, &mu_); continue; }            // no L2 server: sleep; never front() on an empty heap
        Server s = heap_.front(); uint64_t epoch = epoch_;
        if (mono_now() < s.next_ns) {                                              // sleep until the earliest check, the table changes, or stop()
            pthread_cond_timedwait(&cv_, &mu_, &to_ts(s.next_ns));                 // absolute CLOCK_MONOTONIC; no signal is used
            continue;                                                              // re-evaluate from the top: the heap may have been swapped
        }
        std::pop_heap(heap_.begin(), heap_.end(), ByNext{}); heap_.pop_back();
        inflight_key_ = s.cbs_key; inflight_valid_ = true; inflight_removed_ = false;
        pthread_mutex_unlock(&mu_);
        Snapshot st = kick(s);                                                     // BPF decides whether a kick is due (≤ 100 µs target, TMG-003)
        uint64_t now = mono_now();
        if (!st.valid)                                             s.next_ns = now + REPLENISH_RETRY_NS;   // the kick could not be run (transient error): try again soon, not after a whole period
        else if (st.throttled && st.replenish_at_ns <= now)        s.next_ns = now + REPLENISH_RETRY_NS;   // due but not promoted yet (TT slot, IPI in flight): look again soon
        else if (st.replenish_at_ns > now)                         s.next_ns = st.replenish_at_ns;         // follow the open window's end
        else                                                       s.next_ns = now + s.period_ns;          // no open window: look again in T_s
        pthread_mutex_lock(&mu_);
        if (epoch == epoch_ && !inflight_removed_) {                               // drop s only if the whole table was swapped or s itself was removed
            heap_.push_back(s); std::push_heap(heap_.begin(), heap_.end(), ByNext{});   // capacity is reserved
        }
        inflight_valid_ = false;
    }
    pthread_mutex_unlock(&mu_);
}

Replenisher::Snapshot Replenisher::kick(const Server& s) {
    if (backend_ == KickBackend::BpfSyscallProg) {
        ReplenishKickArgs a{}; a.cbs_key = s.cbs_key; a.cpu = s.cpu;
        if (bpf_.run_replenish_kick(&a) != 0) return {false, false, 0, 0};   // bpf_prog_test_run_opts(prog_fd, ctx_in=&a); the kernel copies ctx back
        return {true, a.throttled != 0, a.remaining_us, a.replenish_at_ns};
    }
    CbsState st;                                                      // fallback backend: read-only lookup, then poke the per-CPU kicker
    if (!bpf_.lookup_cbs_state(s.cbs_key, &st)) return {true, false, 0, 0};    // no such server yet (task not bound): nothing to do, same as the BPF program's answer
    if (st.throttled && mono_now() >= st.replenish_at_ns)
        kickers_[s.cpu]->poke();                                      // futex wake of a SCHED_FIFO 97 thread pinned to s.cpu → schedule() → ops.dispatch
    return {true, st.throttled != 0, st.remaining_us, st.replenish_at_ns};
}
```

- **상태를 쓰지 않는다**: Replenisher와 kick 프로그램은 `cbs_map`을 읽기만 한다. 예산 갱신과 승격은 BPF `dispatch`가 한다(§4.4). 이전 초안의 사용자 공간 보충(`apply_replenish_userspace`)과 kick 프로그램 안의 보충은 삭제했다 — 다른 CPU에서 진행 중인 예산 차감을 덮어쓰는 갱신 유실이 생기기 때문이다.
- **다음 확인 시각**: `replenish_at_ns`는 "갱신 시점 + $T_s$"로만 바뀐다. 확인 시점에 윈도우가 열려 있으면 그 끝에 다시 확인하고, 열린 윈도우가 없으면 `now + T_s`에 확인하므로 그 사이에 새로 열린 윈도우의 끝(≥ `now + T_s`)보다 늦지 않다. 윈도우가 끝났는데 아직 승격되지 않은 서버(TT 슬롯이 활성이어서 kick을 보내지 않은 경우, kick이 처리되기 전에 읽은 경우)는 승격될 때까지 1 ms마다 다시 확인한다. "TT 슬롯이 활성"은 `tt_active_on_cpu`의 판정, 곧 그 CPU에 실행 가능한 TT 태스크가 있다는 뜻이다. TT 태스크가 잠들면 CPU가 다음 태스크를 고르면서 dispatch가 호출되므로 kick 없이도 승격된다.
- **테이블 변경·종료 통지**: 조건 변수만 쓴다. 이전 초안의 `clock_nanosleep` + `SIGUSR2`는 처리기가 없으면 프로세스를 종료시키고 신호를 놓칠 수 있어 삭제했다. `epoch_`와 `running_`은 잠금 아래에서만 읽고 쓴다. `epoch_`는 테이블 전체를 교체할 때만 올린다. `remove()`는 루프가 꺼내 들고 있는 서버가 바로 그 서버일 때만 되돌려 넣지 않게 표시하므로, 다른 서버를 제거해도 진행 중이던 서버의 확인 일정은 사라지지 않는다. `stop()`은 잠금 아래에서 `running_ = false`로 바꾸고 `cv_`를 깨운 뒤 join한다.
- **할당**: 힙 저장소는 `set_servers()`가 잠금 밖에서 만들어 교체한다. `remove()`는 제자리에서 지우므로 할당하지 않는다. 루프는 `pop` 뒤에 `push`만 하므로 용량이 늘지 않는다.

`BpfLoader` 추가 API: `bool lookup_cbs_state(uint64_t, CbsState*)`(읽기 전용), `bool delete_cbs_state(uint64_t)`, `bool delete_task_meta(uint32_t)`, `int run_replenish_kick(ReplenishKickArgs*)`, `int get_job_trace_rb_fd()`, `bool set_trace_enabled(bool)`(bss 맵 쓰기), `bool lookup_task_meta(uint32_t, TaskMeta*)`.


### 5.4 `job_trace.{h,cpp}`, `event_source_monitor.{h,cpp}` — 수집기

**(a) `JobTraceCollector` — Job 로그와 실행 구간 레코드**

```cpp
class JobTraceCollector {
public:
    struct Hist { std::array<uint32_t, 1024> bins_10us{}; uint32_t overflow = 0; uint64_t count = 0; uint64_t sum_ns = 0; uint64_t max_ns = 0;
                  uint64_t percentile_ns(double p) const; };
    struct TaskStats {
        uint64_t jobs_ended, completed, discarded;                 // from the SHM Job log (worker-ended Jobs)
        uint64_t closed_cancelled, closed_abandoned;                // TIMPANI closures, from the TIMPANI-N closing records
        uint64_t missed, late_completion, detected_late;           // deadline results (§2.1 flags)
        uint64_t intervals, budget_episodes, throttled_intervals, carryover_intervals;   // execution-interval records (BPF)
        uint64_t job_log_drops;                                     // SHM Job log overflow (slot log_drop)
        Hist response;      /* t_end − t_arr per Job (Job log) */
        Hist interval_wake; /* t_start − t_act per execution interval (BPF); t_start == 0 excluded */
        uint64_t last_budget_used_us, max_budget_used_us;           // OBS-001
    };
    JobTraceCollector(BpfLoader& bpf, const std::string& node_id, volatile struct timpani_ttsched_shm* shm);
    void set_task_names(std::map<uint64_t, std::string> hash_to_name);
    void set_enabled(bool);                                   // writes trace_enabled; opens/closes CSV sink
    void set_on_task_exit(std::function<void(uint64_t task_hash, uint32_t bind_gen)>);   // → TaskRegistry::notify_exit
    void add_tt_jitter_sample(uint32_t cpu, uint32_t slot_idx, uint64_t target_ns, uint64_t actual_ns);   // from TimerMaster
    void record_closed(const ClosedRecord& c);                // from TimerMaster closing sweeps (non-RT path): cancelled / abandoned Jobs with t_close
    const TaskStats& stats(uint64_t key) const;
    void clear_task(uint64_t key);
    void start(); void stop();
private:
    void drain_job_log();                                     // per slot: read log_w (acquire), copy entries, store log_r (release); log_drop read for statistics
    static int rb_cb(void* ctx, void* data, size_t len);      // JobRecord (execution interval) → stats + CSV line
    // CSV (one line per record; order fixed):
    //   "job,<node>,<wl_hash>,<task>,<gen>,<seq>,<t_arr>,<t_dl>,<t_begin>,<t_end>,<outcome>,<miss_src>"   (Job log)
    //   "closed,<node>,<wl_hash>,<task>,<gen>,<seq>,<t_arr>,<t_dl>,<t_begin|0>,<t_close>,<CANCELLED|ABANDONED>,<missed>,<flags>"
    //   "ival,<node>,<wl_hash>,<task>,<bind_gen>,<run_seq>,<cpu>,<t_act>,<t_start>,<t_sleep>,<budget_used_us>,<flags>"   (BPF execution interval)
    //   "src,<node>,<wl_hash>,<task>,<gen>,<started>,<accepted>,<arrivals>,<overflow>,<revoked>,<cancelled>,<abandoned>,<missed>,<mit_viol>,<resp_max_ns>"
    //   "tt,<node>,<cpu>,<slot_idx>,<target_ns>,<actual_ns>,<jitter_ns>"
};
```

- **Job 로그 판독**: 워커가 쓴 항목만 읽는다. 항목 순서는 SPSC의 생산 순서다. 항목마다 `t_arr`, `t_dl`, `t_begin`, `t_end`, `gen`, `seq`를 가지므로 Event와 Job을 대응시키려고 실행 구간 레코드와 시각을 맞출 필요가 없다. 응답 시간 히스토그램은 `t_end − t_arr`다(`t_done − t_act`가 아니다).
- `miss_src = TTSCHED_MISS_END`(2, 워커 종료 전이에서 마감 실패가 설정됨)인 항목은 `LATE_COMPLETION`(0x8)으로 집계한다. `miss_src = TTSCHED_MISS_MONITOR`(1)인 항목은 DeadlineMonitor의 미스 기록과 같은 `(task, gen, seq)`이므로 FaultMonitor의 상세 경로에서 중복 제거한다(§5.5).
- **실행 구간 레코드**: `JobRecord`로 전달되는 BPF 실행 구간 레코드다(§4.1). 여기에는 Job 식별자가 없다. `bind_gen`과 `run_seq`로만 바인딩 안의 구간을 구별한다. `JOB_FLAG_TASK_EXIT` 레코드는 `on_task_exit(task_hash, bind_gen)`로 `TaskRegistry::notify_exit`에 넘긴다.
- 응답 시간 히스토그램은 `t_end − t_arr`로 계산한다. 구간 깨움 지연은 `t_start − t_act`다(`t_start == 0`이면 제외). 10 µs 빈 × 1024(= 10.24 ms) + overflow.
- 출력 파일: `<node_id>.jobtrace.csv`(기본 비활성; `-t` CLI 또는 `SIGUSR1`로 토글).

**(b) `EventSourceMonitor` — 누적 계수 판독(IF-V-004)**

```cpp
struct EventSourceSnapshot {            // cumulative counters of one slot (§5.6), read by atomic loads; not per registration
    uint64_t key;                       // wl_hash ^ task_hash
    uint64_t gen;                       // registration generation at read time
    uint64_t arrivals, accepted, overflow, revoked;                        // post side
    uint64_t started, completed, discarded, cancelled, abandoned, missed;  // worker and closing side
    uint64_t mit_viol;                  // arrival-based MIT violations, cumulative
    uint64_t resp_max_ns, last_arr_ns, last_viol_ns, t_sample_ns;
    uint64_t base_missed, base_mit_viol;   // baselines taken at activation of this registration (§5.6 P1)
};

class EventSourceMonitor {
public:
    using OnSnapshot = std::function<void(const EventSourceSnapshot&)>;   // → JobTraceCollector (src records)
    void set_shm(volatile struct timpani_ttsched_shm* shm, const EvtSlotTable& slots);   // at table apply: keeps the mapping of retained keys
    void clear_task(uint64_t key);
    std::vector<EventSourceSnapshot> poll();    // called from the FaultMonitor poll thread every 100 ms (not an RT path)
private:
    std::map<uint64_t, int> key_to_slot_;       // guarded by mu_
    std::mutex mu_;
};
```

- `poll()`는 각 슬롯의 누적 계수를 atomic load로 읽는다. **판독 순서**: `started`, `cancelled`, `completed`, … 나중에 `accepted`를 읽는다. 게시 쪽은 `publish` 뒤에 `accepted++`를 하므로, 워커의 `started++`가 `accepted++`보다 먼저 보일 수 있다. `accepted`를 마지막에 읽어도, 게시자가 (e)와 (f) 사이에서 멈추면 워커의 `started++`가 먼저 보일 수 있다. 그래서 `pending = accepted − started − cancelled`는 일시적으로 음수가 될 수 있다. 계산은 `max(0, accepted − started − cancelled)`로 하며, 정확한 값은 정지(quiescence) 상태에서만 얻는다(§5.6 계수 관계, A10).
- 계수는 누적 값이다. 폴링 사이의 변화는 유실되지 않는다(FR-N-026). 차이는 `uint64_t` 연산으로 구한다.
- 기준값은 등록 수명 안에서 바뀌지 않는다. 활성화 때 TimerMaster가 읽어 `EvtActivation`으로 넘긴다(§5.6). 활성화 뒤에 읽는 방식은 쓰지 않는다. 그 사이에 수락된 위반이 기준값에 묻히기 때문이다.
- **marker-active 전환, 원천 전환, 활성화 기준 이벤트 버림은 없다.** 이전 초안의 "K/M 원천 전환"과 `OnMarkerActive`는 폐지되었다.
- 테이블 재적용(`set_shm`)은 키→슬롯 표만 `mu_` 아래에서 교체한다. 계수와 기준값은 슬롯에 있으므로 재적용에 영향받지 않는다.

### 5.5 `fault_monitor.{h,cpp}` — 계수 경로와 상세 경로

FaultMonitor는 두 경로로 동작한다. 계수 경로는 무손실이고 보고의 근거다. 상세 경로는 best effort이며 보고에 식별자를 붙인다. 두 경로를 모두 설명하며 중복은 `(task, gen, seq)`로 제거한다.

```cpp
void set_task_params(Key key, uint32_t period_us, uint32_t current_limit);   // T_s (report window length, A15) and DDR-012 threshold
void on_activated(Key key, uint64_t gen, uint64_t base_missed, uint64_t base_mit_viol);   // NEW: count-path baselines of the new registration
void clear_task(Key key);
struct MitWindow { uint64_t start_ns = 0; uint32_t count = 0; };
struct L2State {
    uint64_t  gen = 0;                       // current registration (report only for this gen)
    uint64_t  base_missed = 0;               // slot.cnt.missed at activation
    uint64_t  base_mit_viol = 0;             // slot.cnt.mit_viol at activation
    uint64_t  mit_accounted = 0;             // mit_viol already put into a window
    MitWindow mit;                           // open aggregation window (≤ 1 report per T_s)
    uint64_t  last_episode_replenish_at = 0; // BUDGET_EXCEED de-duplication key
    bool      dmiss_reported = false;        // DDR-012 threshold state (once per registration)
};
std::map<Key, L2State> l2_;                  // guarded by mu_; callbacks run after mu_ is released

// Detail table: de-duplicated by (task, gen, seq). Bounded: DETAIL_KEEP entries per task (candidate 16), oldest evicted.
struct MissDetail { uint64_t gen; uint32_t seq; uint64_t t_arr_ns, t_dl_ns; uint8_t state_at_detect; uint8_t throttled; uint8_t miss_src; uint8_t closed; };
void detail_add(Key key, const MissDetail& d);   // returns false (no report) when the same (gen, seq) is already known

using FaultCallback = std::function<void(const FaultEvent&, uint32_t flags, uint32_t count, const MissDetail* detail)>;

void poll_tick();    // every 100 ms, in this order:
                     // (1) drain the fault ringbuf and DeadlineMonitor miss queue and the closing-miss list into local buffers (libbpf callbacks only copy);
                     // (2) snapshots = event_sources.poll();
                     // (3) process the buffered items (below);
                     // (4) flush elapsed MIT windows.
                     // (2) before (3): the counts used by (3) are never older than the buffered items' baselines.
```

**계수 경로 (무손실, 보고 근거)**

- 각 주기에 `snap.missed`와 `snap.mit_viol`을 읽는다. 현재 등록의 마감 실패 수는 `n = snap.missed − base_missed`다. 현재 등록의 MIT 위반 증분은 `snap.mit_viol − max(base_mit_viol, mit_accounted)`다.
- **DMISS 임계 규칙(DDR-012, 변경 없음)**: `n > current_limit`이고 `dmiss_reported == false`이면 `FaultInfo{DMISS, count = n}`을 한 번 보낸다. 같은 등록에서 다시 보내지 않는다. 승인 후에는 이 보고에 같은 등록의 가장 최근 상세 기록(있으면)의 `job_seq`, `job_gen`, `flags`를 붙인다. 승인 전에는 `count`만 싣는다(A18).
- **MIT_VIOLATION 보고**: 증분을 `add_mit(key, delta, now)`에 넣는다. 보고는 source당 $T_s$ 윈도우에 1건이며 `count`는 그 윈도우의 위반 수다(§2.1). `snap.mit_viol`의 증분만 쓰므로 BPF의 MIT 이벤트는 없다.
- 보고 형식은 원천과 무관하다(도달 기준이 유일한 원천).

**상세 경로 (best effort, 식별 정보 제공)**

- 입력은 세 가지다. (i) DeadlineMonitor의 미스 기록(§5.9), (ii) 워커 종료로 생긴 Job 로그 항목 중 `miss_src ≠ 0`인 것, (iii) TimerMaster 닫기 절차의 마감 실패(`closed = 1`, 시작 정리는 `DETECTED_LATE`). 워커가 대기 중 닫은 stale 세대 기록(`close_stale`)의 마감 실패는 `missed` 계수로만 전달되고 상세 기록은 남지 않는다(계수 경로가 기준이다).
- 각 항목은 `(task, gen, seq)`로 중복 제거한 뒤 상세 표에 넣는다. **`gen`이 현재 등록의 `gen`과 다르면 상세 표와 보고에서 제외한다**(다른 등록의 마감 실패는 기록과 통계에만 남는다). 현재 등록의 항목이면 `flags`를 다음처럼 계산한다. 승인 후 보고에 싣는다(승인 전에는 계산만 하고 싣지 않는다, A18).
  - `NOT_STARTED`(0x4): `state_at_detect == QUEUED`(미스 기록만 해당).
  - `THROTTLED`(0x2): 미스 기록의 `throttled` 값(감지 경로에서만).
  - `MIT_RELATED`(0x1): `snap.last_viol_ns ≥ t_arr − T_s`.
  - `LATE_COMPLETION`(0x8): Job 로그의 `miss_src == TTSCHED_MISS_END`.
  - `DETECTED_LATE`(0x10): 닫기 절차에서 설정됨(`closed = 1`).
  - `MIT_RELATED` 식은 DeadlineMonitor, 워커 종료, 정리 sweep이 설정한 모든 마감 실패에 같이 쓴다(FaultMonitor가 계산한다).
- 상세 기록은 위 계수 경로의 DMISS 보고에 붙는다. 붙이지 못한 상세 기록은 기록(트레이스) 출력에만 남는다. 상세 경로가 가득 차 버려진 항목은 `detail_drop`으로 세며 계수에는 영향이 없다.

**FaultInfo 보고와 시간 기준**

- 마감 실패 보고는 DDR-012의 임계 규칙을 따른다. 임계를 넘지 않은 개별 마감 실패는 보고되지 않는다. 그 기준은 열린 질문이다(§12). TMG-015(보고 지연)는 **보내진 보고**에 대해서만 정의된다.
- 닫기 절차에서 생긴 마감 실패는 기록과 통계에 남는다. 보고는 현재 등록의 임계 규칙을 따른다.

```cpp
// BPF fault_type → node_control.FaultType: explicit table, never an integer cast (numbers differ; DDR-008 D-10)
static node::v1::FaultType to_proto_fault(uint8_t bpf_type) {
    switch (bpf_type) {
    case FAULT_DMISS:                 return node::v1::DMISS;                  // 0 → 1 (TT path only)
    case FAULT_BUDGET_EXCEED:         return node::v1::BUDGET_EXCEED;          // 1 → 2
    case FAULT_MIT_VIOLATION:         return node::v1::MIT_VIOLATION;          // 2 → 4 (retired in BPF: not emitted; kept for the table)
    case FAULT_WAKEUP_LATENCY_EXCEED: return node::v1::WAKEUP_LATENCY_EXCEED;  // 3 → 5 (P2)
    case FAULT_WATCHDOG:              return node::v1::WATCHDOG;               // 4 → 3 (P2)
    default:                          return node::v1::UNKNOWN;
    }
}

// One roll-over helper for MIT windows: flush the elapsed window first, then add.
void add_mit(Key key, uint32_t n, uint64_t now) {
    MitWindow& w = l2_[key].mit;
    if (w.count && now - w.start_ns >= period_ns(key)) flush_mit(key);   // sends FaultInfo{MIT_VIOLATION, count}, then w = {}
    if (w.count == 0) w.start_ns = now;
    w.count += n;
}

// BUDGET_EXCEED (one report per throttle episode) — unchanged from D-10
// TT FAULT_DMISS — forwarded unchanged (DDR-012); not an L2 path.
```

`NodeClient::send_fault(...)`는 `type = to_proto_fault(...)`, `task_id`(hash → name), `node_id`, `timestamp_ns`(REALTIME), `count`를 채우고, 승인 후에는 `flags`, `job_seq`, `job_gen`도 채운다(FR-N-015, A18). `job_seq`와 `job_gen`은 DMISS 보고에서만 채우고, 상세가 없으면 0이다.

### 5.6 `timer_master.{h,cpp}` — SHM v2 레이아웃, 활성화, 비활성화, 시작 정리

이 절이 SHM v2 레이아웃의 단일 출처다. §6과 §5.8은 여기 정의된 상수와 구조체를 참조한다. 현행 `sample-apps/src/libttsched.h`는 v1(`TIMPANI_MAX_TASKS 32`, 슬롯의 `counter` 하나로 TT 깨움만 표현)이다. 이 절의 v2 구조는 설계이며 구현되지 않았다.

**상수 (제안, 빌드 상수)**

```c
#define TIMPANI_TTSCHED_MAGIC   0x54494D51U   /* "TIMQ" — SHM v2 magic value kept from the v2 draft */
#define TIMPANI_TTSCHED_VERSION 2U
#define TIMPANI_MAX_TASKS       64U           /* candidate (v1 header: 32) */
#define TTSCHED_EVT_QDEPTH      8U            /* Event record ring depth: ring occupancy bound = QUEUED + RUNNING + closed entries head has not passed yet (A13). E_FULL can occur with fewer live Events than QDEPTH; power of two; candidate */
#define TTSCHED_EVT_PAYLOAD_MAX 64U           /* inline payload bytes per Event; candidate */
#define TTSCHED_JOB_LOG_N       32U           /* Job log entries per slot (SPSC, lossy with drop counter); candidate */

#define TTSCHED_ST_FREE      0u               /* initial (zero-filled) state: not published, not terminal */
#define TTSCHED_ST_QUEUED    1u
#define TTSCHED_ST_RUNNING   2u
#define TTSCHED_ST_COMPLETED 3u               /* terminal */
#define TTSCHED_ST_DISCARDED 4u               /* terminal (policy discard by the worker) */
#define TTSCHED_ST_CANCELLED 5u               /* terminal (closed by TIMPANI before the Job started) */
#define TTSCHED_ST_ABANDONED 6u               /* terminal (closed by TIMPANI while running) */
#define TTSCHED_ST_REVOKED   7u               /* terminal (post rolled back; never an Event) */
#define TTSCHED_W_MISSED     (1ull << 8)      /* MISSED flag: set at most once per Job, never cleared */
#define TTSCHED_W_STATE(w)   ((uint32_t)((w) & 0xffu))
#define TTSCHED_W_SEQ(w)     ((uint32_t)((w) >> 32))
#define TTSCHED_W_MAKE(seq, st, missed) \
    ((((uint64_t)(seq)) << 32) | ((missed) ? TTSCHED_W_MISSED : 0ull) | (uint64_t)(st))
#define TTSCHED_W_IS_TERMINAL(st) ((st) >= TTSCHED_ST_COMPLETED)

#define TTSCHED_KIND_NONE    0U              /* slot kind: not in use */
#define TTSCHED_KIND_TT      1U              /* slot = TT activation point */
#define TTSCHED_KIND_EVT     2U              /* slot = active L2 Event source */
#define TTSCHED_MISS_NONE    0               /* Job log miss_src: no deadline miss */
#define TTSCHED_MISS_MONITOR 1               /* miss_src: MISSED was set before the worker's end transition */
#define TTSCHED_MISS_END     2               /* miss_src: MISSED was set by the worker's end transition */
```

**Event 레코드 (ring entry, 128 B)**

```c
struct ttsched_evt_rec {
    _Atomic uint64_t word;             /*   0  (seq << 32) | MISSED | state. The only word CASed after publication */
    _Atomic uint64_t gen;              /*   8  registration gen at acceptance */
    _Atomic uint64_t t_arr_ns;         /*  16  CLOCK_MONOTONIC arrival time */
    _Atomic uint64_t t_dl_ns;          /*  24  absolute deadline = t_arr_ns + deadline_us * 1000, fixed at acceptance */
    uint32_t len;                      /*  32  payload length, ≤ TTSCHED_EVT_PAYLOAD_MAX */
    uint32_t _pad0;                    /*  36 */
    uint8_t  payload[TTSCHED_EVT_PAYLOAD_MAX];   /* 40..103 */
    uint8_t  _reserved[24];            /* 104..127: zero, for later use */
};
```

**Job 로그 항목 (48 B, 워커만 생산)**

```c
struct ttsched_job_rec {
    uint64_t gen;                      /*  0 */
    uint32_t seq;                      /*  8 */
    uint8_t  outcome;                  /* 12  TTSCHED_EVT_COMPLETED (0) or TTSCHED_EVT_DISCARDED (1) */
    uint8_t  miss_src;                 /* 13  0 none; 1 MONITOR (set before the end transition); 2 END (set by the end transition) */
    uint16_t _pad;                     /* 14 */
    uint64_t t_arr_ns;                 /* 16 */
    uint64_t t_dl_ns;                  /* 24 */
    uint64_t t_begin_ns;               /* 32  worker took the Event (Job start) */
    uint64_t t_end_ns;                 /* 40  clock read in done() before the end transition (§6.2) */
};
```

**슬롯 (v2)**

```c
struct ttsched_cnt {                   /* cumulative counters over the life of the object; atomic fetch_add; monotonic */
    _Atomic uint64_t arrivals, accepted, overflow, revoked;      /* post side */
    _Atomic uint64_t started, completed, discarded, cancelled, abandoned, missed;   /* worker and closing side */
    _Atomic uint64_t mit_viol;                                   /* arrival-based MIT violations */
};                                     /* 88 bytes */

struct timpani_task_slot {             /* 2752 bytes */
    char     name[16];                 /*   0  task_id (comm, ≤ 15 bytes + NUL). Pinned for the object's lifetime (L2) */
    _Atomic uint32_t counter;          /*  16  futex word. TT: tick count. EVT: wake sequence (activation, deactivation and closing bump it) */
    _Atomic uint32_t kind;             /*  20  TTSCHED_KIND_NONE / TT / EVT */
    _Atomic uint64_t gen;              /*  24  upper 32 bits: low 32 bits of instance_id; lower 32 bits: bind count (bind_gen) */
    _Atomic uint32_t mit_us;           /*  32  configuration published at activation */
    _Atomic uint32_t deadline_us;      /*  36  configuration published at activation */
    _Atomic uint32_t head;             /*  40  ring head (free-running; index = value mod QDEPTH) */
    _Atomic uint32_t tail;             /*  44  ring tail (free-running; poster-owned) */
    _Atomic uint64_t last_arr_ns;      /*  48  arrival bookkeeping: t_arr of the most recent post (0 after activation) */
    _Atomic uint64_t last_viol_ns;     /*  56  t_arr of the most recent MIT-violating post (0 = none) */
    struct ttsched_cnt cnt;            /*  64..151 */
    _Atomic uint64_t resp_max_ns;      /* 152  maximum of t_end − t_arr, written only by the worker */
    _Atomic uint32_t log_w;            /* 160  Job log producer index (worker) */
    _Atomic uint32_t log_r;            /* 164  Job log consumer index (JobTraceCollector) */
    _Atomic uint64_t log_drop;         /* 168  Job log entries not written because the log was full */
    uint8_t  _pad2[16];                /* 176..191 */
    struct ttsched_evt_rec ring[TTSCHED_EVT_QDEPTH];        /* 192..1215 */
    struct ttsched_job_rec log[TTSCHED_JOB_LOG_N];          /* 1216..2751 */
};

struct timpani_ttsched_shm {           /* 64-byte header, then the slot array */
    _Atomic uint32_t magic;            /* TIMPANI_TTSCHED_MAGIC when ready, 0 otherwise */
    uint32_t version;                  /* TIMPANI_TTSCHED_VERSION */
    uint32_t n_tasks;
    _Atomic uint32_t generation;
    uint64_t instance_id;              /* nonzero, CLOCK_REALTIME ns at creation */
    uint64_t reserved;
    uint8_t  _pad[32];
    struct timpani_task_slot tasks[TIMPANI_MAX_TASKS];
};

#define TIMPANI_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)   /* C; C++ builds use static_assert */
TIMPANI_STATIC_ASSERT(sizeof(struct ttsched_evt_rec) == 128, "Event record is 128 bytes");
TIMPANI_STATIC_ASSERT(sizeof(struct ttsched_job_rec) == 48, "Job log entry is 48 bytes");
TIMPANI_STATIC_ASSERT(sizeof(struct ttsched_cnt) == 88, "11 counters of 8 bytes");
TIMPANI_STATIC_ASSERT(offsetof(struct timpani_task_slot, ring) == 192, "ring starts on a 64-byte boundary");
TIMPANI_STATIC_ASSERT(offsetof(struct timpani_task_slot, log) == 1216, "Job log follows the ring");
TIMPANI_STATIC_ASSERT(sizeof(struct timpani_task_slot) == 2752, "slot is 2752 bytes (43 cache lines)");
TIMPANI_STATIC_ASSERT(sizeof(struct timpani_ttsched_shm) == 64 + 64 * 2752, "object = header + 64 slots = 176192 bytes (< 256 KiB)");
```

- 객체 크기: 176192 B(약 172 KiB)로 256 KiB 상한 안이다. 슬롯 수와 큐 깊이를 바꾸면 이 크기와 static assert를 함께 바꾼다.
- `_Atomic`은 C 헤더다. C++ 쪽(TimerMaster, DeadlineMonitor, JobTraceCollector)은 같은 메모리 위치에 `__atomic` 내장 함수 또는 `std::atomic_ref`(C++20)로 `seq_cst` 접근을 한다. 레이아웃은 위 static assert로 고정한다.
- **보고 식별자**: `gen`의 상위 32비트는 `instance_id` 하위 32비트이고, 하위 32비트 `bind_gen`은 바인딩마다 1 증가한다(`next_bind_gen`). `(gen, seq)`는 같은 SHM 객체 안에서 `seq`가 한 바퀴 돌기 전(수락 2^32회 이내)까지 유일하다. 데몬 재시작 사이의 유일성은 `instance_id` 하위 32비트에 기대므로 확률적이다. 오프라인 분석은 trace에 기록된 64비트 `instance_id`로 데몬 실행을 구분한다(A14). `seq`는 게시 시점의 `tail` 값이다.
- **계수 관계** (정지 상태에서 성립): `accepted = started + cancelled_before_start + queued`, 여기서 `cancelled_before_start`는 `CANCELLED`로 닫힌 것이고 `queued`는 QUEUED 상태의 기록 수다. 진행 중인 연산 때문에 일시적으로 어긋날 수 있다(§5.4(b) 판독 순서).
- **쓰는 주체**: 게시자 — 기록 필드, `word` 게시, 롤백(MISSED가 있으면 CANCELLED, 아니면 REVOKED), `tail`, 도달 계수(`arrivals`, `accepted`, `overflow`, `revoked`, `mit_viol`), `last_arr_ns`, `last_viol_ns`, `counter++`와 깨움. 워커 — QUEUED → RUNNING, `t_begin`, 종료 전이와 `t_end`, `started/completed/discarded`, `resp_max_ns`, Job 로그 항목, `log_w`. DeadlineMonitor — 기록 `word`의 MISSED 비트(CAS)와 `missed++`. TimerMaster — `kind`, `name`, `mit_us`, `deadline_us`, `gen`, `last_arr_ns = 0`(활성화), 닫기 전이(CANCELLED, ABANDONED, 그리고 마감 실패 시 MISSED)와 그 계수, `counter++`와 깨움. `head`는 터미널 기록을 만난 주체(워커, TimerMaster, 가득 찬 링을 만난 게시자)가 CAS로 전진시킨다.

> `log_r`은 JobTraceCollector(판독 스레드)가 쓴다.

**링 규칙 (LLD 측 해석; 게시 쪽 코드와 교차 확인은 §6.2)**

- R-a `head`는 `s`에서 `s+1`로 CAS로만 전진하며, `entry[s].word = (s, 터미널)`일 때만 전진한다. 항목은 `head`가 지나간 뒤에만 재사용된다. 그러므로 QUEUED나 RUNNING 기록은 덮어쓰이지 않는다. 워커는 RUNNING 동안 payload를 복사한다.
- R-b 게시자만 `tail`과 `tail` 위치 항목의 필드를 쓴다. 순서: 필드 기록 → `word = (tail, QUEUED)` 저장(게시) → `tail + 1` 저장. 항목은 `word.seq`가 기대 seq와 같고 상태가 FREE가 아닐 때만 게시된 것이다. 게시자가 마지막 두 단계 사이에서 죽었다면, `tail` 위치 항목이 게시되어 있는 것을 본 다음 게시는 `tail`을 먼저 전진시킨다(§6.2 복구).
- R-c 소유자가 아닌 판독자(감시자, TimerMaster 닫기, 도우미)는 `word`를 읽고, 필드를 읽고, `word`를 다시 읽는다. 두 값이 다르면 재시도하거나 건너뛴다. 상태를 바꾸는 것은 읽은 값에 대한 CAS뿐이다. 감시자와 닫기 절차는 `head`·`tail`로 범위를 정하지 않고 모든 QDEPTH 항목을 훑으며 상태(QUEUED/RUNNING)로 고른다.

**프로토콜 1 — 활성화 (TimerMaster, 바인딩 시점)**

활성화는 워커 스레드가 바인딩되고 SCHED_EXT로 바뀐 뒤에 한다(§5.1 (C) 단계 5). 그래서 수락된 Event에는 관리되는 워커가 있다. 대상은 모든 L2 task다.

```text
activate_evt(name, cfg, bind_gen):          // table apply / rescan thread, under bind_mu_
  slot = pinned slot of name                // §5.1 (B0)
  1. slot.kind = NONE                       // seq_cst. No new post can pass step (a) of §6.2 from now on
                                             //   (normally already NONE; defensive when a re-bind follows a non-closed slot)
  2. closing sweep, mode ABANDON_RUNNING    // §5.6 닫기 규칙 (all QDEPTH entries)
  3. slot.mit_us = cfg.mit_us; slot.deadline_us = cfg.deadline_us; slot.last_arr_ns = 0
  4. base_missed = slot.cnt.missed; base_mit_viol = slot.cnt.mit_viol      // after the sweep, before publication
  5. slot.gen = ((u64)(uint32)instance_id << 32) | bind_gen                // seq_cst
  6. slot.kind = EVT                                                        // seq_cst, publication of the slot
  7. slot.counter++; FUTEX_WAKE(&slot.counter, INT_MAX)                     // wake any waiter left from an earlier binding
  return { slot_idx, gen, base_missed, base_mit_viol }
```

- 단계 4의 기준값은 닫기 규칙(단계 2)이 끝난 뒤, 게시(단계 6) 전에 읽는다. 그래서 닫기가 만든 마감 실패는 옛 등록의 계수로 들어가고, 새 등록의 계수에는 들어가지 않는다. 기준값 읽기와 게시 사이에 들어온 Event는 새 등록의 계수다.
- `next_bind_gen(name)`은 `lower32(slot.gen) + 1`을 읽어 돌려주기만 한다. 게시하지 않는다. 그래서 `TaskMeta.bind_gen`을 활성화보다 먼저 쓸 수 있다(§5.1 (C) 단계 2).
- `ring indices`(`head`, `tail`)는 초기화하지 않는다. 값은 free-running이며, `head`는 터미널 기록을 지나가면서 전진한다. 활성화가 게시자가 쓰는 인덱스를 건드리지 않는다(P1 design brief v3 §10, 검토 기록).

**프로토콜 2–4 (게시·대기·종료)** — 전체 절차는 §6.2에서 정의한다. 이 절에서는 TimerMaster가 알아야 할 점만 적는다.

- 게시 단계 (a)는 `kind`만 본다(`kind != EVT`이면 E_INACTIVE, 변화 없음). `magic`은 보지 않는다. TT 재발행이 `magic`을 토글해도 게시는 거절되지 않는다. 시작 정리(프로토콜 7)는 `kind = NONE`으로 게시를 거절한다(A2).
- 대기(프로토콜 3)는 `kind`가 EVT가 아니면 E_INACTIVE로 복귀한다. 비활성화가 `counter`를 올리고 깨우므로 잠든 대기자도 놓치지 않는다. 검사와 가져가기 CAS 사이의 틈은 §6.4 P1 한계에 적는다(A12).
- 종료(프로토콜 4)는 `W0`의 seq가 열린 Job의 seq와 다르거나 `W0`이 ABANDONED이면 E_INACTIVE다(A6). 닫기 규칙이 RUNNING을 닫은 뒤 늦게 끝낸 워커나, 항목이 재사용된 뒤 끝내려는 워커는 이 경로로 복귀한다.

**프로토콜 5 — 비활성화와 닫기 규칙 (TimerMaster)**

비활성화 호출자는 두 곳이다. 테이블에서 빠지거나 STOP될 때(§5.1 제거 경로)와 워커 종료가 감지될 때(§5.2 `on_exited`).

```text
deactivate_evt(name, worker_exited):
  slot = pinned slot of name
  1. slot.kind = NONE                                        // seq_cst, first
  2. closing sweep, mode = worker_exited ? ABANDON_RUNNING : KEEP_RUNNING
  3. help head: advance head past terminal entries (R-a)
  4. slot.counter++; FUTEX_WAKE(&slot.counter, INT_MAX)      // waiters see kind = NONE and return E_INACTIVE
```

**닫기 절차 (sweep)** — 모든 QDEPTH 항목에 대해 수행한다.

```text
closing_sweep(slot, mode, now):
  for i in 0 .. QDEPTH-1:
    loop:                                                     // R-c: read word, read fields, CAS on the value read
      w = ring[i].word                                         // seq_cst
      st = STATE(w)
      if st == QUEUED:                 new_st = CANCELLED
      elif st == RUNNING and mode == ABANDON_RUNNING: new_st = ABANDONED
      else: break                                              // terminal, FREE, or RUNNING kept under deadline monitoring
      t_dl = ring[i].t_dl_ns                                   // read after word; the CAS below validates the occupant
      miss = (now > t_dl) and not (w & MISSED)                  // strict: t > t_dl
      nw = MAKE(SEQ(w), new_st, (w & MISSED) or miss)
      if CAS(ring[i].word, w, nw):
          if new_st == CANCELLED: cnt.cancelled++ ; else cnt.abandoned++
          if miss: cnt.missed++; record {closed=1, miss = DETECTED_LATE} to the closing-miss list
          record ClosedRecord{gen, seq, t_arr, t_dl, t_begin?, t_close = now, new_st, missed} to JobTraceCollector
          break
      else: retry loop                                         // a concurrent end or monitor mark changed the word
```

- 닫기 규칙(시작 정리와 활성화 단계 2)은 `RUNNING`을 무조건 `ABANDONED`로 닫는다(`mode = ABANDON_RUNNING`). 비활성화(프로토콜 5)는 워커가 살아 있으면 `KEEP_RUNNING`으로 두어 워커가 나중에 끝낼 수 있게 하고 마감 감시가 계속 본다.
- **Job 로그에는 쓰지 않는다**. 닫힌 Job의 기록은 TimerMaster가 자신의 추적 출력(`ClosedRecord`, JobTraceCollector의 `closed,` 줄)에 쓴다(§5.4(a)). Job 로그는 워커 전용 SPSC다.
- 닫기 sweep이 MISSED를 설정하는 것은 이 CAS에서만이다. 성공한 CAS가 MISSED를 더할 때만 `missed++`다. 닫기 절차에서 생긴 마감 실패의 플래그는 DETECTED_LATE(0x10)다(§2.1).
- 정리 뒤 `counter`를 올리고 깨운다. 대기 중인 소비자는 `kind`를 보고 E_INACTIVE로 복귀한다.

**프로토콜 6 — 마감 결과 (결정)**

MISSED는 정확히 한 번 성공한 CAS로 설정된다. 설정 주체는 다음 넷 중 하나다. (1) DeadlineMonitor(기록이 QUEUED나 RUNNING이고 `now > t_dl`), (2) 워커의 종료 전이(`t_end > t_dl`), (3) 닫기 sweep(`now > t_dl`), (4) 워커가 대기 중에 수행하는 stale 세대 취소 `close_stale`(`now > t_dl`, §6.4). 설정한 주체가 `missed++`를 한다. 그러므로 어느 쪽이 감지하든 Job당 한 번만 계수된다. 게시 롤백은 기존 MISSED를 유지할 뿐 설정하지 않는다(A11). (3)과 (4)의 마감 실패는 닫기 절차의 미스이므로 DETECTED_LATE(0x10)를 설정한다. `t_end ≤ t_dl`로 게시된 완료는 나중에 수집되어도 마감 실패가 아니다. 어떤 후속 전이도 MISSED를 지우지 않는다.

**프로토콜 7 — 데몬 재시작 (시작 정리)**

객체 생명주기는 9209bc5에서 이어받으며 LLD §5.6 절차를 따른다. 시작 시 객체 `/timpani_ttsched`가 이미 있으면(이전 데몬의 비정상 종료) 다음 순서로 처리한다.

1. 객체를 연다.
2. `magic = 0`으로 무효화한다. 새 `ttsched_init()`은 무효 객체를 매핑하지 않는다. 게시를 거절하는 것은 3단계의 `kind = NONE`이다(게시 (a)는 `kind`만 본다, A2).
3. 모든 슬롯에 `kind = NONE`을 쓴다.
4. 모든 슬롯에 닫기 규칙(`ABANDON_RUNNING`)을 적용한다. 마감을 넘긴 기록에는 MISSED가 설정되고 `DETECTED_LATE`로 기록된다.
5. 모든 슬롯의 `counter++`와 `FUTEX_WAKE(INT_MAX)`로 대기자를 깨운다.
6. 최종 계수를 로그에 남긴다.
7. `shm_unlink`한다.
8. `O_CREAT|O_EXCL`로 새 객체를 만들고 `fchmod(fd, 0666)`으로 권한을 맞춘 뒤(`shm_open`의 모드는 umask에 깎인다) `version = 2`, 새 `instance_id`(0이 아닌 값)를 쓴다.

- 무효화(2–3단계)를 닫기 규칙보다 먼저 하는 것이 중요하다. 그래야 무효화 전에 수락된 게시는 닫기 규칙으로 정리되고, 이후의 게시는 `kind`가 NONE이므로 E_INACTIVE를 받는다.
- 옛 객체에 붙어 있던 앱은 E_INACTIVE를 받고 `ttsched_init()`으로 새 객체에 다시 붙는다. `seq`는 새 객체에서 0부터 다시 시작한다. `gen`이 다르므로 `(gen, seq)`는 유일하다.
- 현행 생성자의 "있으면 그대로 연다" 동작은 없앤다.

**P1 한계 (이 절에 속하는 것)**

- 재등록 경계의 게시: 재등록 중인 게시 하나는 새 등록의 `arrivals`/`mit_viol` 계수에 들어갈 수 있다(등록당 최대 1건).
- 마지막 기록의 `missed++`: 이전 바인딩의 마지막 Job이 워커의 종료 전이로 MISSED를 설정했고 `missed++`가 아직 실행되지 않은 상태에서 새 등록의 기준값을 읽으면, 그 `missed++`는 새 등록에 들어간다(바인딩당 최대 1건). 감시(DeadlineMonitor)의 `missed++`도 CAS 뒤에 실행되므로 기준값 뒤에 들어갈 수 있다(바인딩당 최대 1건, A9).
- 데몬 중단 중: 옛 객체는 여전히 `kind = EVT`다. 게시는 수락되고 워커는 계속 처리한다. 워커의 종료 전이만 늦은 완료를 판정한다. 큐에서 또는 실행 중에 만료된 Event는 아무도 감지하지 않는다. 그런 마감 실패는 다음 시작 정리에서 `DETECTED_LATE`로 늦게 확인되고 최종 통계에만 나타난다. 실시간 보고는 없다. P1에는 데몬 하트비트가 없다.
- 게시자 사망(게시 중간): 공개 뒤 `tail` 전진 전에 죽으면 다음 게시가 R-b로 그 항목을 복구해 `accepted`를 올리고 그 게시의 (f)가 워커를 깨운다. `tail` 전진 뒤 (f) 앞에서 죽으면 `accepted` 계수 한 건이 빠지고 복구되지 않는다. 두 경우 모두 워커는 다음 게시나 TimerMaster 변화 전까지 깨지 않으며, 그 Event는 감시된다(§6.4, A8).
- 신뢰: SHM 객체에 쓸 수 있는 프로세스는 모든 슬롯을 망가뜨릴 수 있다(§6, F-14).

**TT 슬롯**

- TT 슬롯은 같은 객체의 같은 크기 슬롯을 쓴다. 기존 핸드셰이크는 그대로다. `magic = 0` → TT 슬롯 `counter++` + 깨움 → 이름 기록 → `n_tasks`(release) → `generation++`(release) → `magic = TIMPANI_TTSCHED_MAGIC`(release). 유지되는 TT 태스크는 이전 인덱스를 쓰고, 새 TT 태스크는 EVT에 배정된 적이 없는 가장 낮은 빈 인덱스를 쓴다.
- "전 슬롯 `counter` 증가"는 **TT 슬롯에만** 적용한다. EVT 슬롯의 `counter`는 활성화·비활성화·닫기에서만 올린다.

**EVT 슬롯의 배정과 유지 (`update_evt_slots`)**

- **고정 배정**: L2 `task_id`에 한 번 배정한 슬롯은 데몬 수명 동안 그 이름의 것이다(`evt_slot_of_name_`). 다른 이름에 재사용하지 않고 0으로 지우지 않는다. 새 이름은 한 번도 쓰지 않은 슬롯(0으로 초기화된 상태)을 받는다. 남은 슬롯이 없으면 그 엔트리는 적용 오류다.
- **이름만 쓴다**: `name`을 기록한다. `kind`, `mit_us`, `deadline_us`, `gen`, 계수, 기준값은 쓰지 않는다. 그래서 테이블 재발행 중에도 게시·대기가 끊기지 않고 미소비 Event가 보존된다.
- **유지되는 바인딩**: 파라미터가 같으면 §5.1 (C)의 D-17(a)에 따라 아무것도 쓰지 않는다. 파라미터가 바뀌면 바인딩을 해제하고(프로토콜 5) 다시 바인딩하며(프로토콜 1), 그때 새 `gen`이 발행된다.
- `EvtSlotTable`은 `key → {slot index}`다. 기준값은 이 표에 없고 활성화 결과(`EvtActivation`)로 넘어간다.

```cpp
struct EvtTaskEntry { std::string task_name; uint64_t task_id_hash; uint64_t workload_id_hash; uint32_t mit_us; uint32_t deadline_us; };
struct EvtConfig { uint32_t mit_us; uint32_t deadline_us; uint64_t cbs_key; };
struct EvtActivation { int slot; uint64_t gen; uint64_t base_missed; uint64_t base_mit_viol; };
struct EvtSlotInfo { int idx; };
using EvtSlotTable = std::map<uint64_t, EvtSlotInfo>;     // key (wl_hash ^ task_hash) → slot

EvtSlotTable update_evt_slots(const std::vector<EvtTaskEntry>& evt_tasks);     // pin names only (§5.1 (B0))
uint32_t next_bind_gen(const std::string& name) const;                         // lower32(gen) + 1, not published
EvtActivation activate_evt(const std::string& name, const EvtConfig& cfg, uint32_t bind_gen);   // protocol 1
void deactivate_evt(const std::string& name, bool worker_exited);             // protocol 5
void startup_cleanup();                                                        // protocol 7, before the new object is created
bool same_tt_table(const std::vector<SlotEntry>& slots, uint64_t hp, uint64_t epoch) const;   // unchanged
void keep_tt_threads();                                                        // NEW: L2-only change, TT threads kept
volatile struct timpani_ttsched_shm* shm() const;                              // unchanged
```

- 기존 `set_schedule_table(slots, hp, epoch)`는 TT 슬롯과 TT 스레드만 다룬다. EVT 인자와 반환은 없어진다(이름 변경 없이 시그니처에서 뺀다. 제안, 승인 필요).
- `cpu_thread_loop`: 지터 샘플은 `JobTraceCollector::add_tt_jitter_sample()`로 전달한다(락프리 SPSC 링, RT 안전). stdout 통계는 유지한다.

### 5.7 CLI/설정 추가
`-t` (job trace on), `-r <ms>` (rescan period, default 500), `-k syscall|thread` (kick backend, default syscall), `-T` (trace sink path). 모두 선택이다. 마감 감시 주기(`DL_SCAN_PERIOD_US`)는 CLI 옵션이 아니라 빌드 상수다(§5.9).

### 5.8 timpani-n 테스트 (GTest 타깃 신설: `timpani-n/tests/`)

모든 테스트는 AAA 구조를 따른다. SHM 프로토콜 경쟁 테스트는 호스트 스레드 두 개 이상으로 같은 객체를 쓴다. 각 쌍에 대해 한 테스트가 있다(§5.6 seq_cst 쌍).

| 테스트 | 대상 |
|:--|:--|
| `JobFsmTest.RunnableStartsIntervalOnlyFromWaiting` / `PreemptionInsideIntervalDoesNotRestart` / `SleepEndsInterval` / `BlockingResumeStartsNewInterval` | `job_fsm.h` (FR-B-001~003, 실행 구간) |
| `JobFsmTest.NoMitCheckOnShortSpacing` (간격이 MIT보다 짧아도 상태와 플래그는 변하지 않는다) / `NoDeadlineVerdictOnSleep` (늦은 잠듦에도 DMISS 기록이 없다) | FR-B-024 (D-05, D-20) |
| `CbsChargeTest.NanosecondCarry` / `ThrottleTransitionOnce` | FR-B-012/013 |
| `CbsWindowTest.ConformingActivationAlwaysRefills` (DDR-008 D-04 반례: T=18 ms, C=2 ms, 활성화 16.0/34.0 ms → 스로틀 없음) / `EarlyActivationKeepsRemainingAndMarksCarryover` / `ToleranceAbsorbsWakeJitter` (간격 17.95 ms: 보충) / `AfterViolationWindowShiftsThenRecoversWithSlack` (D-04 "위반 후 동작" 타임라인: 0/15/33 ms, 여유가 있으면 복귀) / `ExactBudgetJobIsThrottled` (차감액 = $C_s$ 경계) / `NoRefillWhileActiveAndNotThrottled` / `PromotionRefillsAndOpensWindowFromNow` / `ConsumptionPerWindowBounded` | `cbs_on_activation`, `cbs_try_promote` (FR-B-014/017) |
| `QueueOrderTest.RequeueKeepsActivationRank` / `PromotedJobGoesToTail` | `queue_key_ns` (FR-B-016) |
| `TaskRegistryTest.ResolvesLateThread` (fork+prctl 후 ≤1 s) / `DetectsExit` / `BinderMayReenterRegistry` | FR-N-003/004 |
| `TaskRegistryTest.StaleExitRecordIgnored` (bind_gen이 다른 종료 기록은 추적 중인 새 바인딩을 비활성화하지 않는다) / `ExitRecordOfCurrentBindingDeactivates` | FR-N-004, FR-N-029 |
| `TaskRegistryTest.BindReturnsGenAndTracksIt` / `FailedSchedExtLeavesSlotNone` (`sched_ext` 전환 실패 → 활성화 없음, kind = NONE 유지) | FR-N-005, FR-N-029 |
| `ReplenisherTest.HeapOrder` / `SwapOnTableChangeWakesLoop` / `RearmsFromReturnedReplenishAt` / `RetriesWhileDueButNotPromoted` / `RemoveDuringKickDoesNotPushBack` / `RemoveOfOtherServerKeepsInflightServer` / `StopWakesEmptyLoop` / `NeverWritesCbsMap` / `EmptyHeapSleeps` (kick 백엔드 mock) | FR-N-010 (변경 없음, §5.3) |
| `ApplyTest.UnchangedEntryWritesNothing` (재적용 후 `run_seq`·`remaining_us`·슬롯 `gen`·`kind` 불변) / `ChangedEntryUnbindsDeactivatesThenActivatesNewGen` (`run_seq`는 0에서 다시 시작, 슬롯 계수는 유지) / `ServerExistsBeforeSchedExt` / `MissingCpuIsApplyError` | FR-N-005, IF-ON-003, D-17 |
| `ShmV2LayoutTest.StaticAssertsHold` (크기와 오프셋: 128 B 레코드, 48 B Job 로그, 2752 B 슬롯, 176192 B 객체) | §5.6, IF-L-004 |
| `ShmV2Test.PinnedSlotKeepsNameAcrossReAdd` / `UpdateEvtSlotsDoesNotTouchKindOrCounters` / `TtRepublishDoesNotBumpEvtCounter` / `StaleObjectKilledAndRecreatedAtStartup` (새 `instance_id`, 모드 0666) | IF-L-001/002, FR-N-006 |
| `ActivationTest.ClosesLeftoverRecordsBeforePublishing` (옛 QUEUED → CANCELLED, 옛 RUNNING → ABANDONED, 마감을 넘긴 것은 MISSED 추가 후 계수) / `BaselineReadAfterSweep` (옛 등록의 닫기 마감 실패는 새 등록의 `base_missed`에 들어가지 않는다) / `GenIsInstanceHighBindLow` (상위 32비트 = instance_id 하위, 하위 32비트 = bind_gen) / `NextBindGenDoesNotPublish` | §5.6 프로토콜 1 |
| `ActivationTest.KindNoneBeforeSweepDefensive` (EVT 상태에서 재활성화 → 단계 1이 게시를 막는다) | §5.6 프로토콜 1 단계 1 |
| `DeactivationTest.KindFirstThenSweep` (닫기 중 게시 시도 → E_INACTIVE 또는 닫힌 뒤 게시) / `QueuedClosedCancelledNotAnEvent` / `RunningKeptWhenWorkerAlive` (RUNNING은 KEEP_RUNNING에서 남고, 감시 대상으로 남는다) / `RunningAbandonedWhenWorkerExited` / `WakesWaiterWithKindNone` | §5.6 프로토콜 5 |
| `StartupCleanupTest.OrderMagicFirstThenKindThenSweep` (순서 기록 검사) / `OverdueRecordsGetMissedDetectedLate` (DETECTED_LATE 0x10) / `CountersLoggedBeforeUnlink` | §5.6 프로토콜 7 |
| `ProtocolRaceTest.PublishVsKindNone` (게시 단계 (f)의 `kind`/`gen` 재확인과 비활성화의 `kind = NONE` 경쟁 → 게시 수락 또는 롤백, 이중 계수 없음) | §5.6 seq_cst 쌍 (poster: publish → read kind/gen) |
| `ProtocolRaceTest.ActivationGenVsPost` (게시가 gen을 읽은 뒤 활성화가 gen을 바꾸면 롤백 경로로 간다) | §5.6 seq_cst 쌍 (TimerMaster: write kind → read words) |
| `ProtocolRaceTest.CloseVsWorkerEnd` (닫기 CAS와 워커 종료 CAS 경쟁 → 정확히 하나가 이긴다, 마감 실패 중복 없음) | §5.6 프로토콜 4, 6 |
| `ProtocolRaceTest.MonitorVsWorkerEndSameBoundary` (`t_end = t_dl` 근처의 완료와 감시 CAS → 완료가 `t_end ≤ t_dl`이면 MISSED 없음, 아니면 정확히 한 번) | §5.6 프로토콜 4, 6, §5.9 |
| `EvtDeadlineRaceTest.ReadsWordFieldsWordAndSkipsChanged` (R-c 재확인) | §5.6 R-c |
| `FaultMonitorTest.BudgetExceedOncePerEpisode` / `DmissThresholdUnchanged` (count > limit에서 한 번) / `DmissCountUsesRegistrationBaseline` (옛 등록의 계수는 빠진다) | FR-N-012~014 |
| `FaultMonitorTest.MitWindowAggregation` (T_s 윈도우당 1건, count = 위반 수) / `MitDeltaNotLostBetweenPolls` / `MitCountEqualsArrivalCounterOnly` (BPF MIT 이벤트 없음) | FR-N-012/014, D-05 |
| `FaultMonitorTest.DetailDedupByTaskGenSeq` (같은 (gen, seq)의 미스 기록과 Job 로그 항목 → 상세 한 건) / `DetailOfOtherGenerationNotReported` / `DetailDropCountedNotCounted` (상세 버림은 계수에 영향 없음) | FR-N-031 |
| `FaultMonitorTest.FlagsNotStartedThrottledMitRelatedLateCompletionDetectedLate` (각 비트의 설정 조건) / `MitRelatedUsesArrivalTime` (`last_viol_ns ≥ t_arr − T_s`, 윈도우가 넘어간 뒤에도 유지) / `DmissNotMitRelatedWhenViolationIsOlderThanTs` | FR-N-031, PRT-007 |
| `JobTraceTest.HistogramPercentiles` / `ResponseIsEndMinusArrival` (`t_end − t_arr`) / `IntervalWakeExcludesZeroStart` / `TracksMaxBudgetUsed` / `TaskExitRecordNotifiesRegistryWithBindGen` / `ClosedRecordHasNoBeginWhenNeverStarted` | FR-N-020~022 |
| `EventSourceMonitorTest.SnapshotPendingClampedWhilePosterStalled` (게시자가 (e)와 (f) 사이에 멈춘 동안 `pending`이 0으로 고정된다, A10) / `CumulativeCountersNotLostBetweenPolls` / `BaselineFromActivationNotFromPoll` / `BaselineKeptAcrossReapply` | FR-N-026 |
| `TimerMasterTest.SlotPinnedToNameAcrossReAdd` / `CloseRecordWrittenByTimerMasterNotByWorker` (닫힌 Job이 Job 로그에 쓰이지 않는다) | IF-L-004, §5.6 |

### 5.9 `deadline_monitor.{h,cpp}` — 신규 (DeadlineMonitor)

> 이 컴포넌트는 P1에서 새로 추가된다. 모든 Event의 절대 마감 `t_dl`을 큐 대기, 실행, 선점, 스로틀, 유휴 CPU 상태와 무관하게 감시한다. 완료 콜백을 기다리지 않는다. 결과는 Job당 한 번이다. TT 슬롯이 없는 CPU에서도 같은 방식으로 동작하며 TT 이벤트나 hyperperiod 경계를 실행 조건으로 쓰지 않는다(FR-N-032, D-22).

**스레드와 시각**

- 전용 SCHED_FIFO 스레드. 우선순위 97(후보, TimerMaster 99와 Replenisher 98보다 낮고 일반 앱 RT 스레드보다 높다, A1). 격리되지 않은 CPU에서 돈다. Replenisher가 이 기능을 이미 제공한다고 가정하지 않는다. 두 스레드를 합칠지는 F-12다.
- 시각: `CLOCK_MONOTONIC`. 주기 대기는 `clock_nanosleep(TIMER_ABSTIME)`이며 신호를 쓰지 않는다. TT 경로의 `CLOCK_REALTIME` 사용은 건드리지 않는다.
- 힙 할당 없음, 블로킹 I/O 없음, gRPC 없음. 미스 기록 큐는 정적 배열이다.

**주기와 비용 (후보)**

- `DL_SCAN_PERIOD_US = 1000`(후보). 스캔 한 번은 `TIMPANI_MAX_TASKS × TTSCHED_EVT_QDEPTH = 512`개 기록을 읽는다. 기록 하나당 원자 로드 몇 개다. 마감 실패 하나당 CAS, `missed++`, 최대 한 번의 BPF 맵 조회, 큐 넣기 하나가 추가된다.
- 비용 상한: 한 스캔의 최악은 512 기록 판독과 마감 실패 512건 처리다(실제로는 Job 수 제한으로 훨씬 적다). 스캔 시간과 깨움 지터의 측정 목표는 PoC 항목(OI-9 인접)이다.

아래 루프는 주기 스캔 기준을 유지한다. 가장 이른 절대 마감의 단일 타이머 대안은 DDR-008 D-20/F-10, REQ-008 OI-22의 설계 검토·타깃 PoC 사항이며 이 스케치에는 도입하지 않는다. 완료 시각 경합 OI-20도 미결이다.

**감시 루프 (C++ 스케치)**

```cpp
void DeadlineMonitor::scan_once(uint64_t now) {                 // `now` read ONCE per scan (CLOCK_MONOTONIC)
    for (int idx = 0; idx < TIMPANI_MAX_TASKS; ++idx) {
        volatile struct timpani_task_slot* s = &shm_->tasks[idx];   // kind is NOT checked: a RUNNING record of a deactivated source stays monitored
        for (uint32_t i = 0; i < TTSCHED_EVT_QDEPTH; ++i) {
            volatile struct ttsched_evt_rec* r = &s->ring[i];
            uint64_t w = r->word.load(seq_cst);                  // R-c: word, fields, word again
            uint32_t st = TTSCHED_W_STATE(w);
            if ((st != TTSCHED_ST_QUEUED && st != TTSCHED_ST_RUNNING) || (w & TTSCHED_W_MISSED)) continue;
            uint64_t t_dl = r->t_dl_ns.load(seq_cst);
            uint64_t gen  = r->gen.load(seq_cst);
            uint64_t t_arr = r->t_arr_ns.load(seq_cst);
            if (r->word.load(seq_cst) != w) continue;            // changed meanwhile: skip; the next scan decides
            if (gen != s->gen.load(seq_cst)) continue;           // earlier registration: change no state (A9, D-19, brief §3)
            if (now <= t_dl) continue;                           // strict: a miss needs now > t_dl
            uint64_t nw = w | TTSCHED_W_MISSED;
            if (!r->word.compare_exchange_strong(w, nw)) continue;   // CAS on the value read: a worker end or closing sweep won
            s->cnt.missed.fetch_add(1, seq_cst);                 // only the successful CAS counts
            MissRecord m{idx, gen, TTSCHED_W_SEQ(w), t_arr, t_dl, now, st, read_throttled_if_any(idx)};   // CbsState read only here
            if (!miss_queue_.push(m)) miss_drop_.fetch_add(1);   // full: the mark and the count are kept; only the detail is dropped
        }
    }
}
```

- 위 스케치의 `read_throttled_if_any`는 감지된 기록에서만 호출하며, `cbs_key_of_[idx]`로 BPF `cbs_map`을 한 번 읽는다(비차단 맵 조회). 키가 없으면 `throttled = 0`이다.
- `now > t_dl`은 엄격 부등식이다(REQ-008 §2.5, PRT-009). 정시 완료 `t_end ≤ t_dl`이 MISSED를 얻지 못하는 이유다.

**슬롯별 BPF 키 표**

- `cbs_key_of_[idx]`(원자)는 활성화(§5.1 (C) 단계 5)에서 `on_activated(slot, cbs_key)`로 쓰고, 비활성화에서 0으로 돌린다. 감시 스레드는 읽기만 한다.

**미스 기록과 큐 (SPSC)**

- `MissRecord { slot, gen, seq, t_arr_ns, t_dl_ns, t_detect_ns, state_at_detect, throttled }`. 고정 크기다.
- 큐는 SPSC 정적 배열이다(후보 256). 생산자는 감시 스레드, 소비자는 FaultMonitor의 `poll_tick`이다.
- **큐가 가득 차면**: MISSED 표시와 `missed++`는 이미 CAS와 원자 연산으로 끝났으므로 유지된다. 기록만 버리고 `miss_drop_`을 센다. 계수 경로는 계수에 의존하므로 보고 근거는 흔들리지 않는다. 상세 경로만 줄어든다.

**결과 규칙 (Job당 한 번, 경합)**

- 감시 CAS는 워커 종료 전이(§5.6 프로토콜 4)와 같은 word에 대해 경쟁한다. 워커가 먼저 완료를 게시했고 `t_end ≤ t_dl`이면 `now > t_dl`인 감시는 완료된 기록을 보지 못한다(상태가 터미널이므로 건너뜀). 감시가 먼저 MISSED를 설정했으면 워커의 전이 CAS가 실패하고 워커는 시계를 다시 읽는다(§6.2 done (i)–(iv)). 기록된 `t_end`와 MISSED는 어긋나지 않는다.
- 늦게 수집된 정시 완료는 마감 실패가 아니다(감시는 터미널 기록을 보지 않는다. 판정은 게시 시점의 `t_end`로 한다).

**재시작·비활성화 연동**

- 데몬이 멈추면 감시도 멈춘다. 시작 정리(§5.6 프로토콜 7)에서 늦게 발견된 마감 실패는 `DETECTED_LATE`로 계수된다. 실시간 감지는 없다(§1.3).
- 비활성화된 source의 RUNNING 기록(워커가 살아 있음)은 터미널이 될 때까지 감시된다. 닫기 규칙으로 ABANDONED가 되면 감시 대상이 아니다.

**측정 항목 (PoC, OI-9 인접)**: 1 kHz 스캔의 비용과 지터를 타깃 CPU에서, TT 부하 아래와 선택한 CPU에서 측정한다. 이벤트 기반 깨움(주기 스캔 대신)이 합의된 상한에 필요한지 판단한다(F-10).

**DeadlineMonitor 인터페이스 (제안)**

```cpp
class DeadlineMonitor {
public:
    struct MissRecord { int slot; uint64_t gen; uint32_t seq; uint64_t t_arr_ns, t_dl_ns, t_detect_ns; uint8_t state_at_detect; uint8_t throttled; };
    DeadlineMonitor(volatile struct timpani_ttsched_shm* shm, BpfLoader& bpf, int fifo_prio = 97);   // A1: candidate, fixed by the PoC
    void on_activated(int slot, uint64_t cbs_key);      // table apply / rescan thread: per-slot BPF key (§5.1 (C))
    void on_deactivated(int slot);                       // clears the key (0)
    void start(); void stop();
    bool pop_miss(MissRecord* out);                      // consumer: FaultMonitor::poll_tick (SPSC)
    uint64_t miss_drops() const;                         // detail-path drops, reported in statistics
private:
    void loop();                                         // clock_nanosleep(TIMER_ABSTIME) every DL_SCAN_PERIOD_US
    void scan_once(uint64_t now);
    static constexpr uint64_t DL_SCAN_PERIOD_US = 1000;  // candidate; build constant
    static constexpr size_t   MISS_Q_N = 256;            // candidate
    std::atomic<uint64_t> cbs_key_of_[TIMPANI_MAX_TASKS];   // 0 = none
};
```

**시간 기준 및 지연 항목**

- 감지 지연(TIM-009) = `t_detect − t_dl` 상한 = 스캔 주기 + 감시 스레드 깨움 지터(후보 2 ms, 합의·측정 필요).
- 보고 지연(TIM-010) = FaultInfo 보고 시각 − MISSED 설정 시각(후보 200 ms, 합의·측정 필요). 이 값은 경계이며 보장이 아니다.
- 두 값은 Linux 타깃 PoC에서 측정하기 전까지 후보다. "마감 경과 시 감지"는 지연 0을 뜻하지 않는다(REQ-008 PRT-009 제한).

**DeadlineMonitor 단위 테스트**(`DeadlineMonitorTest.*`)는 아래 표에 나열한다:

| 테스트 | 검증 |
|:--|:--|
| `DeadlineMonitorTest.DetectsWhileQueued` (QUEUED, 워커 없음, 마감 경과 → MISSED와 NOT_STARTED) | PRT-009, FR-N-027/028 |
| `DeadlineMonitorTest.DetectsWhileRunning` (RUNNING, 워커가 CPU를 빼앗김 → MISSED, THROTTLED는 CbsState에 따름) | PRT-009, FR-N-027 |
| `DeadlineMonitorTest.DetectsWhileIdleCpu` (실행 가능한 태스크 없음에서도 감지) | FR-N-027 |
| `DeadlineMonitorTest.NoDoubleCountAgainstConcurrentDone` (워커 종료 CAS와 감시 CAS 경쟁 → missed 한 번) | FR-N-028, §5.6 프로토콜 6 |
| `DeadlineMonitorTest.RetryOnChangedWord` (R-c 재확인이 다르면 건너뛴다) | §5.6 R-c |
| `DeadlineMonitorTest.StrictGreaterThan` (`now == t_dl`은 미스가 아님) | REQ-008 §2.5, PRT-009 |
| `DeadlineMonitorTest.TerminalRecordOfDeactivatedSlotUntouched` (deactivated 슬롯의 터미널 기록은 건드리지 않는다) | FR-N-029 |
| `DeadlineMonitorTest.OldGenQueuedRecordNoMissAfterReactivation` (현재 세대 g2와 다른 gen g1의 QUEUED 기록은 `now > t_dl`이어도 MISSED가 없고 `missed`는 변하지 않는다, 감시 gen 검사 A9) | FR-N-029 |
| `DeadlineMonitorTest.StaleGenRecordChangesNothingOfNewRegistration` (옛 세대의 MISSED 기록과 계수: 새 기준값과 새 계수가 변하지 않는다, AC-008-15(c)) | FR-N-029, AC-008-15 |
| `DeadlineMonitorTest.FullQueueKeepsMarkAndCount` (큐가 가득 차면 MISSED와 `missed`는 유지되고 `miss_drops`만 증가) | FR-N-031 |
| `DeadlineMonitorTest.KeyTableClearedOnDeactivation` | §5.9 |
| `DeadlineMonitorTest.ScanCostBounded` (512 기록 스캔 상한, 호스트 측정 목표 — 실제 타깃 측정은 PoC) | §5.9 비용 상한 |

---


---

## 6. `libttsched` v2 (`sample-apps/src/libttsched.{h,c}`) — 명시적 Event 연결 API

> SRS OI-14 종결: 별도 라이브러리 없이 `libttsched`를 확장한다(DDR-008 D-03).
> 이 절은 명시적 Event 연결의 앱 쪽 계약(`ttsched_evt_post`, `ttsched_evt_wait`, `ttsched_evt_done`)과 구현 요지다. 공개 API, 상수, 공유 메모리 참조는 **제안이며 Project Leader 승인 대상**이다. 승인 전에는 구현하지 않는다. 구현·검증 완료를 주장하지 않는다.
> 개념(Event, Job, 도달 시각, 마감)은 REQ-008 §2가 정의한다. libttsched는 마감을 판정하지 않고 BPF도 Job을 세지 않는다. 마감 판정은 TIMPANI-N의 DeadlineMonitor(§5.9)가 한다. 이 절의 코드는 수락 규약과 경합 규칙을 보이는 스케치이며 컴파일 검증을 거치지 않았다.

### 6.1 공개 헤더와 호출 규약 (제안)

상수와 구조체의 단일 출처는 §5.6이다. 공개 헤더는 앱이 보는 부분만 노출하고, SHM 구조체와 내부 상수는 내부 헤더 `libttsched_shm.h`(신규, §5.6 내용을 그대로 담는다)에 둔다.

**실행 문맥(D-24).** API는 호출 스레드에서 실행되며 SHM은 수동적 저장소다. 등록 L2 워커 TID는 `SCHED_EXT`/timpani-n 커널 BPF CBS 대상이고, wait·done의 실제 CPU 실행 비용도 워커 CBS에 포함된다(블로킹 대기 시간 자체는 CPU 비용이 아니다). post 비용은 호출한 생산자 문맥에 속하며 프로세스 전체가 워커 CBS 대상이 되는 것은 아니다. 입력 어댑터·다른 앱 스레드의 정책 선택·관리는 P1 범위 밖이다. 배치·환경 검증은 유지하며, 워커만 격리하는 조건과 N의 TID 이동/검증 책임 미결 사항은 DDR-008 D-24와 HLD-008 §5.3을 따른다. 이 설명은 현재 바인딩 스케치에 cgroup 이동을 추가하지 않는다.

```c
/* sample-apps/src/libttsched.h — v2 public part (proposal, Project Leader approval required) */
#include <stdint.h>

#define TTSCHED_EVT_PAYLOAD_MAX 64U      /* value fixed in §5.6 (single source); inline payload bytes per Event */

/* Return codes. A call that returns a negative value created nothing. */
#define TTSCHED_EVT_E_INACTIVE (-1)      /* source not active (not registered, worker not bound, removed, daemon down, SHM not writable) or the Job was closed by TIMPANI */
#define TTSCHED_EVT_E_FULL     (-2)      /* queue full: rejected, counted as overflow */
#define TTSCHED_EVT_E_INVAL    (-3)      /* bad argument: len > PAYLOAD_MAX, t_arr in the future or earlier than the previous arrival, bad task_id, bad outcome */
#define TTSCHED_EVT_E_STATE    (-4)      /* wait while a Job is open, or done with no open Job */

enum { TTSCHED_EVT_COMPLETED = 0, TTSCHED_EVT_DISCARDED = 1 };

typedef int ttsched_evt_t;               /* source handle: >= 0 on success, < 0 on error */
struct ttsched_evt_id    { uint64_t gen; uint32_t seq; };
struct ttsched_evt_event { uint64_t gen; uint32_t seq; uint32_t len;
                           uint64_t t_arr_ns; uint64_t t_deadline_ns;
                           uint8_t payload[TTSCHED_EVT_PAYLOAD_MAX]; };

/* Non-RT. Resolves the handle for `task_id` (1..15 bytes) in the per-process table (16 entries). The handle works even
 * when the source is not registered yet: posts return E_INACTIVE until it is. Attaches to the SHM object if it exists. */
ttsched_evt_t ttsched_evt_open(const char *task_id);

/* Post one Event. 0: accepted — the Event exists, has identity (gen, seq) and deadline t_arr + D, and is monitored.
 * Negative: nothing was created (E_FULL: counted as overflow; E_INACTIVE after an arrival was counted: counted as revoked).
 * t_arr_ns == 0 means "now" (CLOCK_MONOTONIC read in the call). id_out may be NULL. */
int ttsched_evt_post(ttsched_evt_t src, uint64_t t_arr_ns, const void *payload, uint32_t len,
                     struct ttsched_evt_id *id_out);

/* Worker: wait for the next Event of this task's source and start its Job (exactly one Event).
 * 0: *ev filled, Job open. E_INACTIVE: cannot wait now (a back-off of TTSCHED_BACKOFF_NS runs inside the call) → call
 * ttsched_init() and retry. E_STATE: the previous Job was not ended with ttsched_evt_done(). */
int ttsched_evt_wait(struct ttsched_evt_event *ev);

/* Worker: end the open Job. Mandatory for every Job; there is no implicit completion at the next wait.
 * 0: recorded. E_STATE: no open Job. E_INVAL: outcome is neither COMPLETED nor DISCARDED; the Job is ended as DISCARDED
 * and the open flag is cleared (done always closes the open Job).
 * E_INACTIVE: the Job had already been closed by TIMPANI (ABANDONED); the open flag is cleared. */
int ttsched_evt_done(int outcome);

/* Process attach. Maps (or re-maps when instance_id changed) the SHM object and resolves this thread's slot by thread name
 * (= task_id). Non-RT. The one re-attach call of a process. */
void ttsched_init(void);
int  ttsched_wait_next_period(void);     /* L1 (TT), unchanged */
```

호출 규약과 상태:

- 공개 API 명칭은 게시(post)·Event 대기(wait)·종료(done)다(REQ-008 §2). wait는 처리할 Event가 있을 때까지 기다리고 같은 호출 안에서 Job 시작을 확정·기록하여 Event를 인계한 뒤 반환한다. 이미 QUEUED Event가 있으면 블로킹 없이 반환할 수 있다. 별도 공개 취득 API는 없다.
- **Job 시작 상태 전이(`QUEUED → RUNNING`), 즉 처리 시작 확정**은 wait 내부의 CAS(compare-and-swap)로 수행한다. CAS는 읽어 둔 상태 워드와 현재 값이 같을 때만 새 값으로 바꾸는 원자적 조건부 갱신이다. 추가 큐·공개 API·스케줄러 연산도, 페이로드 복사도 아니다. §6.2의 실제 순서는 상태·세대 확인 → MISSED를 보존하는 전이 CAS(실패하면 재조회) → 내부 `take()`에서 `t_begin` 기록, 열린 Job 설정, `started` 증가 → 메타데이터·페이로드를 호출자가 준 `ev` 버퍼에 복사 → 0 반환이다. `take()`는 공개 API가 아니며, 복사는 CAS 성공 뒤에 수행한다.
- QUEUED/RUNNING은 SHM Event 레코드의 논리적 생명주기이지 BPF DSQ 소속·현재 CPU 실행 여부가 아니다. 워커가 CPU에서 이전 Job을 처리하는 동안 다음 Event는 QUEUED일 수 있다. 시작된 Job의 RUNNING은 선점·스로틀·블로킹 중에도 종료 전까지 유지된다.

- 워커 루프: `for (;;) { if (ttsched_evt_wait(&ev) < 0) { ttsched_init(); continue; } process(&ev); ttsched_evt_done(TTSCHED_EVT_COMPLETED); }`. `ttsched_evt_done()`은 생략할 수 없다. 완료로 간주하는 암묵 종료는 없다. 실패한 Job을 성공으로 기록할 수 있기 때문이다.
- `ttsched_evt_wait()`가 E_INACTIVE를 반환하기 전에 짧은 back-off(후보 10 ms, 9209bc5와 같음)를 수행한다. 데몬이 내려가 있거나 태스크가 아직 바인딩되지 않았을 때 재시도 루프가 CPU를 태우지 않게 하기 위해서다. back-off는 RT 비용 상한 밖이다.
- `ttsched_evt_t` 핸들은 `task_id`를 가리킨다. `ttsched_init()` 뒤에도 유효하며, 다음 사용 때 이름으로 슬롯을 다시 찾는다(고정 슬롯 표의 한정된 순회, 시스템 콜 없음). 게시자가 E_INACTIVE를 받아도 게시 안에서 다시 매핑하지 않는다. 비 RT 경로인 `ttsched_init()`을 부르면 이후 게시가 수락된다.
- 스레드 지역 상태(워커 스레드마다): 이 스레드의 슬롯 포인터와 인덱스, 스레드 이름(`task_id`), 열린 Job 플래그, 열린 Event의 사적 복사본(`gen`, `seq`, `t_arr`, `t_dl`, `t_begin`)과 Job이 속한 슬롯 포인터. 사적 복사본은 Job 로그를 만들 때만 쓰며 공유 기록을 다시 읽지 않는다.
- 프로세스 전역 상태: SHM 매핑 포인터와 쓰기 가능 여부(원자), 핸들 표(16개, 이름, 슬롯 힌트). 매핑 교체는 `ttsched_init()`과 `ttsched_evt_open()`만 `g_init_lock` 아래에서 한다. 게시와 대기는 잠금을 잡지 않는다. 같은 객체에 `ttsched_init()`을 반복 호출해도 매핑 수는 늘지 않는다(A7).
- 호출 권한: 한 source에 게시하는 문맥은 한 번에 하나다(§6.4). source의 wait와 done을 부르는 스레드는 하나다. 같은 comm의 다른 스레드가 wait 또는 done을 부르면 계약 위반이다(REQ-008 §2.7: 워커 스레드는 source당 하나다. A-03은 comm 이름만 정한다).
- 도달 시각: `t_arr_ns == 0`이면 게시 호출이 CLOCK_MONOTONIC을 읽는다. 값을 주면 CLOCK_MONOTONIC이어야 하고, 현재 시각보다 미래이거나 같은 source의 이전 도달보다 이르면 E_INVAL이다. 현재보다 이른 값은 그대로 받아들인다. 어댑터가 이미 쓴 시간은 응답 시간에 포함되며 수용 판정이 덮지 않는다(§3.3 가정 5).
- 비용 상한: post, wait(차단 없는 경우), done은 제한된 원자 연산 수, 최대 64 B의 `memcpy`, 최대 시스템 콜 1회(futex)를 쓴다. 힙과 잠금을 쓰지 않으며 post와 done은 차단하지 않는다. `ttsched_evt_open()`과 `ttsched_init()`의 매핑은 비 RT 경로에서 한다. 매핑이 쓸 수 없으면 post와 wait는 E_INACTIVE를 반환하며 스스로 다시 매핑하지 않는다. 이것이 "지연 초기화와 RT 호출 상한" 검토 의견에 대한 답이다.
- 신뢰(P1): SHM 객체에 쓸 수 있는 모든 프로세스는 신뢰 대상이다. 잘못 쓰는 쓰기 주체는 어떤 슬롯도 망가뜨릴 수 있다. 객체별 격리는 P2다(F-14).

**제거된 API와 개념**: 이전 초안의 `ttsched_evt_notify`, 배치 count `n`을 돌려주는 `ttsched_evt_wait`, 앱 소유 payload 큐와 invalidate·clear 규약, `TTSCHED_EVT_RESET`, `TTSCHED_EVT_BITSET` 및 마커 wait point는 폐지되었다. 이유는 변경 이력과 §6.3에 기록한다.

### 6.2 구현 스케치

아래 코드는 설계 스케치다. 모든 공유 메모리 접근은 C11 기본 순서인 `seq_cst` 원자 연산을 쓴다(`atomic_load`, `atomic_store`, `atomic_fetch_add`, `atomic_compare_exchange_strong`). 명시하지 않은 필드 접근은 §5.6의 `_Atomic` 선언을 따른다.

```c
/* sample-apps/src/libttsched.c — v2 sketch (proposal). Linux, C11. Not compiled. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "libttsched.h"
#include "libttsched_shm.h"          /* internal: §5.6 structs, QDEPTH, JOB_LOG_N, W_* macros, KIND_*, MISS_* */

#define TTSCHED_SHM_PATH          "/timpani_ttsched"
#define TTSCHED_NAME_MAX          15
#define TTSCHED_EVT_MAX_SOURCES   16
#define TTSCHED_BACKOFF_NS        10000000L   /* candidate 10 ms (§6.1) */
#define TTSCHED_MIT_TOL_NS        0ULL        /* tol_mit, candidate 0: arrival stamps carry no wake-up jitter */
#define TTSCHED_QMASK             (TTSCHED_EVT_QDEPTH - 1)

struct evt_src { char name[TTSCHED_NAME_MAX + 1]; int slot_hint; int used; };
static struct evt_src g_src[TTSCHED_EVT_MAX_SOURCES];       /* handle table, guarded by g_init_lock on claim */
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;   /* non-RT paths only */
static _Atomic(struct timpani_ttsched_shm *) g_shm;         /* process-wide mapping; NULL = not attached */
static _Atomic int g_shm_writable;

static __thread char g_tl_name[TTSCHED_NAME_MAX + 1];       /* this thread's task_id */
static __thread int  g_tl_slot = -1;                        /* index hint into the current mapping */
static __thread int  g_tl_open;                             /* the open-Job flag */
static __thread struct timpani_task_slot *g_tl_sl;          /* slot the open Job belongs to (kept across re-attach) */
static __thread struct { uint64_t gen, t_arr, t_dl, t_begin; uint32_t seq; } g_tl_cur;   /* private copy */

static inline uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Shared (not FUTEX_PRIVATE): the word lives in a MAP_SHARED object used by several processes. */
static void futex_wait(_Atomic uint32_t *word, uint32_t expected)
{
    syscall(SYS_futex, (uint32_t *)word, FUTEX_WAIT, expected, NULL, NULL, 0);   /* EAGAIN / EINTR: the caller rescans */
}

static void futex_wake(_Atomic uint32_t *word, int n)
{
    syscall(SYS_futex, (uint32_t *)word, FUTEX_WAKE, n, NULL, NULL, 0);
}

static int backoff_inactive(void)
{
    struct timespec ts = { 0, TTSCHED_BACKOFF_NS };
    nanosleep(&ts, NULL);
    return TTSCHED_EVT_E_INACTIVE;
}

/* Bounded scan of the pinned names. An unpinned slot (empty name) never matches. No syscall. */
static int find_slot(const struct timpani_ttsched_shm *shm, const char *name)
{
    for (int i = 0; i < TIMPANI_MAX_TASKS; i++)
        if (shm->tasks[i].name[0] != '\0' && strncmp(shm->tasks[i].name, name, TTSCHED_NAME_MAX) == 0)
            return i;
    return -1;
}

static struct timpani_task_slot *resolve_source(ttsched_evt_t src, struct timpani_ttsched_shm **shm_out)
{
    struct timpani_ttsched_shm *shm = atomic_load(&g_shm);
    if (shm == NULL || !atomic_load(&g_shm_writable) || src < 0 || src >= TTSCHED_EVT_MAX_SOURCES || !g_src[src].used)
        return NULL;
    int i = g_src[src].slot_hint;
    if (i < 0 || i >= TIMPANI_MAX_TASKS || strncmp(shm->tasks[i].name, g_src[src].name, TTSCHED_NAME_MAX) != 0) {
        i = find_slot(shm, g_src[src].name);
        g_src[src].slot_hint = i;
        if (i < 0) return NULL;
    }
    *shm_out = shm;
    return &shm->tasks[i];
}

static struct timpani_task_slot *thread_slot(struct timpani_ttsched_shm *shm)
{
    if (g_tl_name[0] == '\0') return NULL;                       /* ttsched_init() was not called on this thread */
    if (g_tl_slot < 0 || g_tl_slot >= TIMPANI_MAX_TASKS ||
        strncmp(shm->tasks[g_tl_slot].name, g_tl_name, TTSCHED_NAME_MAX) != 0) {
        g_tl_slot = find_slot(shm, g_tl_name);
        if (g_tl_slot < 0) return NULL;
    }
    return &shm->tasks[g_tl_slot];
}

/* ---- ring helpers (R-a: head moves only by CAS over a terminal entry with the same seq) ---- */
static void advance_head(struct timpani_task_slot *sl, uint32_t seq)
{
    uint32_t expected = seq;
    atomic_compare_exchange_strong(&sl->head, &expected, seq + 1);   /* failure: someone else advanced it */
}

static void help_head(struct timpani_task_slot *sl)
{
    for (;;) {
        uint32_t h = atomic_load(&sl->head);
        uint64_t w = atomic_load(&sl->ring[h & TTSCHED_QMASK].word);
        if (TTSCHED_W_SEQ(w) != h || !TTSCHED_W_IS_TERMINAL(TTSCHED_W_STATE(w)))
            return;
        advance_head(sl, h);
    }
}

static void job_log_append(struct timpani_task_slot *sl, const struct ttsched_job_rec *jr)
{
    uint32_t wi = atomic_load(&sl->log_w);                        /* single producer: the worker */
    uint32_t ri = atomic_load(&sl->log_r);                        /* consumer index, acquire pair */
    if (wi - ri >= TTSCHED_JOB_LOG_N) {
        atomic_fetch_add(&sl->log_drop, 1);                       /* lossy by design; counters are unaffected */
        return;
    }
    sl->log[wi % TTSCHED_JOB_LOG_N] = *jr;
    atomic_store(&sl->log_w, wi + 1);                             /* publish the entry */
}

static int post_accepted(struct ttsched_evt_id *id_out, uint64_t gen, uint32_t seq)
{
    if (id_out) { id_out->gen = gen; id_out->seq = seq; }
    return 0;
}

/* ---- post (protocol 2) ---- */
static int rollback(struct timpani_task_slot *sl, struct ttsched_evt_rec *r, uint32_t seq, uint64_t g,
                    struct ttsched_evt_id *id_out)
{
    for (;;) {
        uint64_t w = atomic_load(&r->word);
        if (TTSCHED_W_SEQ(w) != seq || TTSCHED_W_STATE(w) != TTSCHED_ST_QUEUED) {
            atomic_fetch_add(&sl->cnt.accepted, 1);               /* taken or closed by someone else: it was an Event */
            return post_accepted(id_out, g, seq);
        }
        if (w & TTSCHED_W_MISSED) {                               /* the monitor counted the miss: keep it, close the record */
            if (atomic_compare_exchange_strong(&r->word, &w, TTSCHED_W_MAKE(seq, TTSCHED_ST_CANCELLED, 1))) {
                atomic_fetch_add(&sl->cnt.accepted, 1);
                atomic_fetch_add(&sl->cnt.cancelled, 1);
                return post_accepted(id_out, g, seq);
            }
        } else if (atomic_compare_exchange_strong(&r->word, &w, TTSCHED_W_MAKE(seq, TTSCHED_ST_REVOKED, 0))) {
            atomic_fetch_add(&sl->cnt.revoked, 1);                /* rolled back: never an Event, not accepted */
            return TTSCHED_EVT_E_INACTIVE;
        }
        /* CAS failure: the word changed under us; read it again */
    }
}

int ttsched_evt_post(ttsched_evt_t src, uint64_t t_arr_ns, const void *payload, uint32_t len,
                     struct ttsched_evt_id *id_out)
{
    if (len > TTSCHED_EVT_PAYLOAD_MAX || (len > 0 && payload == NULL))
        return TTSCHED_EVT_E_INVAL;                               /* nothing counted */
    struct timpani_ttsched_shm *shm = NULL;
    struct timpani_task_slot *sl = resolve_source(src, &shm);
    if (sl == NULL)
        return TTSCHED_EVT_E_INACTIVE;

    /* (a) kind only (A2): magic is not checked here; the first check, not counted */
    if (atomic_load(&sl->kind) != TTSCHED_KIND_EVT)
        return TTSCHED_EVT_E_INACTIVE;

    /* (b) registration generation, arrival time validation (not counted on failure) */
    const uint64_t g    = atomic_load(&sl->gen);
    const uint64_t now  = mono_ns();
    const uint64_t last = atomic_load(&sl->last_arr_ns);
    if (t_arr_ns == 0)
        t_arr_ns = now;
    if (t_arr_ns > now || (last != 0 && t_arr_ns < last))
        return TTSCHED_EVT_E_INVAL;

    /* (c) arrival bookkeeping: from here on the post is an arrival (accepted, full or revoked) */
    const uint64_t mit_ns = (uint64_t)atomic_load(&sl->mit_us) * 1000ULL;
    atomic_store(&sl->last_arr_ns, t_arr_ns);
    atomic_fetch_add(&sl->cnt.arrivals, 1);
    if (last != 0 && mit_ns != 0 && t_arr_ns - last + TTSCHED_MIT_TOL_NS < mit_ns) {
        atomic_store(&sl->last_viol_ns, t_arr_ns);
        atomic_fetch_add(&sl->cnt.mit_viol, 1);
    }

    /* (d) R-b recovery: a published entry at tail whose tail was not advanced (poster died after publication) */
    uint32_t tail = atomic_load(&sl->tail);
    struct ttsched_evt_rec *r = &sl->ring[tail & TTSCHED_QMASK];
    uint64_t w = atomic_load(&r->word);
    if (TTSCHED_W_STATE(w) != TTSCHED_ST_FREE && TTSCHED_W_SEQ(w) == tail) {
        atomic_store(&sl->tail, ++tail);                          /* the recovered record is an accepted Event */
        atomic_fetch_add(&sl->cnt.accepted, 1);
        r = &sl->ring[tail & TTSCHED_QMASK];
    }
    uint32_t head = atomic_load(&sl->head);
    if (tail - head >= TTSCHED_EVT_QDEPTH) {
        help_head(sl);                                            /* pass terminal records (R-a) */
        head = atomic_load(&sl->head);
        if (tail - head >= TTSCHED_EVT_QDEPTH) {
            atomic_fetch_add(&sl->cnt.overflow, 1);
            return TTSCHED_EVT_E_FULL;
        }
    }

    /* (e) fill the entry (the poster is its only writer while it is FREE or passed), publish, advance tail */
    const uint64_t t_dl = t_arr_ns + (uint64_t)atomic_load(&sl->deadline_us) * 1000ULL;
    atomic_store(&r->gen, g);
    atomic_store(&r->t_arr_ns, t_arr_ns);
    atomic_store(&r->t_dl_ns, t_dl);
    r->len = len;
    if (len > 0) memcpy(r->payload, payload, len);
    atomic_store(&r->word, TTSCHED_W_MAKE(tail, TTSCHED_ST_QUEUED, 0));   /* publication */
    atomic_store(&sl->tail, tail + 1);

    /* (f) re-check the registration: store (publication) then load (kind, gen) */
    if (atomic_load(&sl->kind) == TTSCHED_KIND_EVT && atomic_load(&sl->gen) == g) {
        atomic_fetch_add(&sl->cnt.accepted, 1);
        atomic_fetch_add(&sl->counter, 1);                        /* wake word, after publication */
        futex_wake(&sl->counter, 1);
        return post_accepted(id_out, g, tail);
    }
    return rollback(sl, r, tail, g, id_out);
}
```

```c
/* ---- wait (protocol 3) ---- */
static void close_stale(struct timpani_task_slot *sl, struct ttsched_evt_rec *r, uint32_t seq)
{
    uint64_t w = atomic_load(&r->word);
    if (TTSCHED_W_SEQ(w) != seq || TTSCHED_W_STATE(w) != TTSCHED_ST_QUEUED)
        return;
    const int late = !(w & TTSCHED_W_MISSED) && mono_ns() > atomic_load(&r->t_dl_ns);
    const uint64_t nw = TTSCHED_W_MAKE(seq, TTSCHED_ST_CANCELLED, ((w & TTSCHED_W_MISSED) != 0) || late);
    if (atomic_compare_exchange_strong(&r->word, &w, nw)) {       /* only the successful CAS counts */
        atomic_fetch_add(&sl->cnt.cancelled, 1);
        if (late) atomic_fetch_add(&sl->cnt.missed, 1);
    }
}

static int take(struct timpani_task_slot *sl, struct ttsched_evt_rec *r, uint32_t seq, uint64_t gen,
                struct ttsched_evt_event *ev)
{
    /* the record is RUNNING now: R-a keeps it from being overwritten until the end transition and head advance */
    g_tl_cur.gen    = gen;
    g_tl_cur.seq    = seq;
    g_tl_cur.t_begin = mono_ns();                                 /* Job start */
    g_tl_cur.t_arr  = atomic_load(&r->t_arr_ns);
    g_tl_cur.t_dl   = atomic_load(&r->t_dl_ns);
    g_tl_sl   = sl;
    g_tl_open = 1;
    atomic_fetch_add(&sl->cnt.started, 1);
    ev->gen = gen;
    ev->seq = seq;
    ev->len = r->len;
    ev->t_arr_ns = g_tl_cur.t_arr;
    ev->t_deadline_ns = g_tl_cur.t_dl;
    if (r->len > 0) memcpy(ev->payload, r->payload, r->len);
    return 0;
}

int ttsched_evt_wait(struct ttsched_evt_event *ev)
{
    if (g_tl_open)
        return TTSCHED_EVT_E_STATE;
    struct timpani_ttsched_shm *shm = atomic_load(&g_shm);
    if (shm == NULL || !atomic_load(&g_shm_writable))
        return backoff_inactive();
    struct timpani_task_slot *sl = thread_slot(shm);
    if (sl == NULL)
        return backoff_inactive();

    for (;;) {
        const uint32_t c = atomic_load(&sl->counter);             /* wake word FIRST, before the scan */
        if (atomic_load(&sl->kind) != TTSCHED_KIND_EVT)
            return backoff_inactive();
        const uint32_t s = atomic_load(&sl->head);
        struct ttsched_evt_rec *r = &sl->ring[s & TTSCHED_QMASK];
        uint64_t w = atomic_load(&r->word);
        const uint32_t st = TTSCHED_W_STATE(w);

        if (TTSCHED_W_SEQ(w) != s || st == TTSCHED_ST_FREE || st == TTSCHED_ST_RUNNING) {
            /* nothing takeable at head. A RUNNING record of a gone worker is closed by TimerMaster, which wakes us */
            futex_wait(&sl->counter, c);
            continue;
        }
        if (TTSCHED_W_IS_TERMINAL(st)) {
            advance_head(sl, s);
            continue;
        }
        /* QUEUED at head */
        const uint64_t rec_gen = atomic_load(&r->gen);
        if (rec_gen != atomic_load(&sl->gen)) {                   /* stale generation: never processed */
            close_stale(sl, r, s);
            continue;
        }
        if (!atomic_compare_exchange_strong(&r->word, &w,
                TTSCHED_W_MAKE(s, TTSCHED_ST_RUNNING, (w & TTSCHED_W_MISSED) != 0)))
            continue;                                              /* lost to a sweep or a monitor mark: scan again */
        return take(sl, r, s, rec_gen, ev);
    }
}
```

```c
/* ---- done (protocol 4: read word, then clock, then CAS; retry) ---- */
int ttsched_evt_done(int outcome)
{
    if (!g_tl_open)
        return TTSCHED_EVT_E_STATE;
    const int bad_outcome = (outcome != TTSCHED_EVT_COMPLETED && outcome != TTSCHED_EVT_DISCARDED);
    if (bad_outcome)
        outcome = TTSCHED_EVT_DISCARDED;                          /* end the Job as DISCARDED and report E_INVAL below */

    struct timpani_task_slot *sl = g_tl_sl;
    struct ttsched_evt_rec *r = &sl->ring[g_tl_cur.seq & TTSCHED_QMASK];
    const uint32_t seq = g_tl_cur.seq;
    const uint32_t st_end = (outcome == TTSCHED_EVT_COMPLETED) ? TTSCHED_ST_COMPLETED : TTSCHED_ST_DISCARDED;
    uint64_t w0, t_end;
    int added;

    for (;;) {
        w0 = atomic_load(&r->word);                               /* (i) W0 */
        if (TTSCHED_W_SEQ(w0) != seq || TTSCHED_W_STATE(w0) == TTSCHED_ST_ABANDONED) {   /* closed by TIMPANI, maybe reused (A6) */
            g_tl_open = 0;
            return TTSCHED_EVT_E_INACTIVE;
        }
        /* W0는 열린 seq의 RUNNING이다. RUNNING 항목은 head가 넘기지 못하고, ABANDONED와 seq 불일치는 위에서 걸러진다 (§6.3) */
        t_end = mono_ns();                                        /* (ii) clock after W0 */
        const int late = t_end > g_tl_cur.t_dl;                   /* strict: t > t_dl */
        added = late && !(w0 & TTSCHED_W_MISSED);
        const uint64_t nw = TTSCHED_W_MAKE(seq, st_end, ((w0 & TTSCHED_W_MISSED) != 0) || late);   /* (iii) */
        if (atomic_compare_exchange_strong(&r->word, &w0, nw))    /* (iv) CAS from W0 */
            break;
        /* failed: a sweep or a monitor mark changed the word. Read W0 again (a mark makes the clock late). */
    }

    if (added)
        atomic_fetch_add(&sl->cnt.missed, 1);                     /* the CAS that added MISSED counts it */
    if (outcome == TTSCHED_EVT_COMPLETED) atomic_fetch_add(&sl->cnt.completed, 1);
    else                                  atomic_fetch_add(&sl->cnt.discarded, 1);
    const uint64_t resp = t_end - g_tl_cur.t_arr;
    if (resp > atomic_load(&sl->resp_max_ns))
        atomic_store(&sl->resp_max_ns, resp);                     /* single writer: the worker */

    uint8_t miss_src = TTSCHED_MISS_NONE;
    if (w0 & TTSCHED_W_MISSED) miss_src = TTSCHED_MISS_MONITOR;   /* marked before the end transition */
    else if (added)            miss_src = TTSCHED_MISS_END;       /* marked by this transition */
    const struct ttsched_job_rec jr = {
        .gen = g_tl_cur.gen, .seq = seq, .outcome = (uint8_t)outcome, .miss_src = miss_src,
        .t_arr_ns = g_tl_cur.t_arr, .t_dl_ns = g_tl_cur.t_dl,
        .t_begin_ns = g_tl_cur.t_begin, .t_end_ns = t_end,
    };
    job_log_append(sl, &jr);                                      /* private copy only */
    advance_head(sl, seq);
    g_tl_open = 0;                                                /* always cleared */
    return bad_outcome ? TTSCHED_EVT_E_INVAL : 0;
}
```

```c
/* ---- non-RT: open, attach ---- */
ttsched_evt_t ttsched_evt_open(const char *task_id)
{
    if (task_id == NULL || task_id[0] == '\0' || strlen(task_id) > TTSCHED_NAME_MAX)
        return TTSCHED_EVT_E_INVAL;
    int h = -1;
    pthread_mutex_lock(&g_init_lock);
    for (int i = 0; i < TTSCHED_EVT_MAX_SOURCES && h < 0; i++)
        if (g_src[i].used && strcmp(g_src[i].name, task_id) == 0) h = i;
    for (int i = 0; i < TTSCHED_EVT_MAX_SOURCES && h < 0; i++)
        if (!g_src[i].used) {
            h = i;
            strncpy(g_src[i].name, task_id, TTSCHED_NAME_MAX);
            g_src[i].slot_hint = -1;
            g_src[i].used = 1;
        }
    pthread_mutex_unlock(&g_init_lock);
    if (h < 0)
        return TTSCHED_EVT_E_FULL;
    ttsched_init();                                               /* attaches if the object exists */
    return h;
}

static struct timpani_ttsched_shm *map_object(int fd, int writable)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(struct timpani_ttsched_shm))
        return NULL;
    void *p = mmap(NULL, sizeof(struct timpani_ttsched_shm),
                   writable ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        return NULL;
    struct timpani_ttsched_shm *m = p;
    if (atomic_load(&m->magic) != TIMPANI_TTSCHED_MAGIC || m->version != TIMPANI_TTSCHED_VERSION) {
        munmap(p, sizeof *m);                                     /* magic 0 during daemon start-up or a foreign object */
        return NULL;
    }
    return m;
}

void ttsched_init(void)
{
    pthread_mutex_lock(&g_init_lock);
    int writable = 1;
    int fd = shm_open(TTSCHED_SHM_PATH, O_RDWR, 0);
    if (fd < 0) { writable = 0; fd = shm_open(TTSCHED_SHM_PATH, O_RDONLY, 0); }
    if (fd >= 0) {
        struct timpani_ttsched_shm *cur = atomic_load(&g_shm);
        struct timpani_ttsched_shm *m = map_object(fd, writable);
        if (m != NULL && cur != NULL && m->instance_id == cur->instance_id) {
            munmap(m, sizeof *m);                                 /* same object: keep the current mapping, drop the fresh one (A7) */
        } else if (m != NULL) {
            atomic_store(&g_shm_writable, writable);
            atomic_store(&g_shm, m);                              /* replaced mappings are kept (§6.4, A7) */
        }
        close(fd);
    } else {
        atomic_store(&g_shm, NULL);                               /* object gone: posts and waits return E_INACTIVE */
    }
    char tname[TTSCHED_NAME_MAX + 1] = {0};
    pthread_getname_np(pthread_self(), tname, sizeof tname);
    memcpy(g_tl_name, tname, sizeof g_tl_name);
    g_tl_slot = -1;
    pthread_mutex_unlock(&g_init_lock);
}
```

**호출별 원자 접근 수 (A25, 후보, 측정 전)**: 위 스케치에서 셌다. `post` 성공 경로(도움·복구·롤백 없음)는 원자 접근 21회다(적재 12, 저장 6, RMW 3: `arrivals`, `accepted`, `counter`). futex 시스템 콜은 최대 1회다. `wait`의 가져가기 경로(깨움 대기 없음)는 12회다(적재 10, RMW 2: 가져가기 CAS, `started`). 대기 경로는 스캔마다 적재 4회와 futex 1회를 더한다. `done` 성공 경로(시계 읽기 제외)는 8회다(적재 4, 저장 1, RMW 3: 종료 CAS, `completed`, head CAS). 게시의 도움·복구·롤백, `done`의 로그 손실 계수, 마감 실패 CAS는 위 수에 더해진다. 이 수는 측정 전 후보이며, SRS FR-L-004의 상한은 이 절을 가리킨다.

### 6.3 인터리빙 논거

- **R-a, 덮어쓰기 금지.** `head`는 항목 `s`를 CAS로만 넘기며, 그 항목의 단어가 `(s, 터미널)`일 때만 넘긴다. 항목 인덱스의 재사용은 `head`가 넘긴 뒤에만 일어나므로 QUEUED나 RUNNING 기록의 필드는 게시자가 다시 쓰지 않는다. 워커는 take CAS로 RUNNING을 얻은 뒤에 payload와 시각을 복사하며, 그 뒤 종료 전이와 `head` 전진 전까지 항목은 고정된다.
- **FIFO와 한 번만 가져가기.** 워커만 `head` 위치의 항목을 가져간다. `head`는 터미널만 넘기고, 가져가기는 `(s, QUEUED)`를 `(s, RUNNING)`으로 바꾸는 CAS다. 같은 항목을 두 번 가져갈 수 없다. 닫기 절차의 CAS(QUEUED→CANCELLED)와 워커의 가져가기 CAS는 같은 단어를 두고 경쟁하며 정확히 하나가 이긴다. 진 쪽은 다시 읽는다.
- **깨움 유실 없음.** 대기자는 깨움 워드 `c`를 스캔 전에 읽는다. 게시(f)와 TimerMaster의 활성화, 비활성화, 닫기는 상태를 바꾼 뒤에 `counter`를 올리므로, 대기자가 스캔에서 게시를 놓쳤다면 그 뒤의 `counter` 증가가 `FUTEX_WAIT(c)`의 값 비교를 실패시키거나 깨운다. 게시 (f)가 실패해 REVOKED가 된 경우에는 깨우지 않아도 된다. 거부된 게시는 Event가 아니므로 워커가 할 일이 없고, 다음 수락된 게시나 TimerMaster의 다음 변화가 워커를 깨운다.
- **게시와 비활성화(저장 후 적재 쌍).** 게시자는 항목을 공개(`word` 저장)한 뒤 `kind`와 `gen`을 읽는다. TimerMaster는 `kind = NONE`을 저장한 뒤 모든 항목의 `word`를 읽는다. `seq_cst`의 전역 순서에서 두 적재 중 적어도 하나는 상대의 저장을 본다. 게시자가 `kind`를 EVT로 보면 닫기 절차가 그 항목을 보고 CANCELLED로 닫는다. 게시자가 NONE 또는 다른 `gen`을 보면 (f)의 롤백이 REVOKED나 CANCELLED를 처리한다. 둘 다 놓치는 경우는 없다. 두 쪽의 전이는 각각 성공한 CAS 한 번이므로 계수는 한 번만 올라간다.
- **등록 경계를 걸친 게시.** 게시자가 `g`를 읽은 뒤 활성화가 `gen`을 바꾸면 (f)가 실패하고 롤백으로 간다. 활성화의 닫기 절차가 그 항목을 CANCELLED로 닫았다면 게시자는 `accepted`만 올리고 E_INACTIVE가 아닌 0을 반환한다(Event가 생겼고 닫혔다). 닫기 전이었다면 REVOKED가 된다. 워커는 `gen`이 다른 QUEUED 기록을 CANCELLED로 닫는다. 기준값(`base_*`) 읽기 전후의 계수가 섞일 수 있는 경계는 §6.4의 한계다.
- **게시 롤백 대 워커 가져가기.** 롤백은 QUEUED인 항목만 CAS로 바꾼다. 워커가 먼저 RUNNING으로 가져갔다면 롤백은 "이미 처리 중"을 보고 수락으로 끝낸다. 롤백이 먼저 REVOKED로 바꿨다면 워커의 CAS가 실패하고 워커는 터미널인 항목을 `head`로 넘긴다. REVOKED 항목은 가져가기 대상이 되지 않는다.
- **종료와 닫기 절차.** 워커의 종료 CAS(ii–iv)와 닫기 절차의 CAS(RUNNING→ABANDONED)는 같은 단어를 두고 경쟁한다. 종료가 이기면 닫기 절차는 터미널 항목을 건너뛴다. 닫기가 이기면 워커의 W0 재읽기가 ABANDONED를 보고 E_INACTIVE로 끝난다. 두 경우 모두 `abandoned` 또는 `completed/discarded` 중 하나만 계수된다.
- **종료 전 seq 검사(A6).** 종료 (i)은 W0의 seq를 열린 Job의 seq와 비교한다. ABANDONED 항목은 터미널이므로 head가 넘긴 뒤 같은 칸을 seq s+QDEPTH가 재사용할 수 있다. seq를 보지 않으면 done의 CAS가 뒤따르는 Event를 덮어쓴다. `RaceTest.AbandonedEntryReusedBeforeDone`이 이 경로를 본다.
- **완료와 감시 경합.** 감시 CAS와 종료 CAS가 같은 단어에 경쟁한다. 종료의 CAS가 먼저면 감시는 터미널을 보고 건너뛴다. 감시가 먼저 MISSED를 붙였다면 종료의 CAS가 실패하고, 종료는 W0를 다시 읽고 시계를 다시 읽는다. 감시가 `now > t_dl`을 본 뒤에 붙인 표시는 종료가 읽는 시계보다 앞서므로 기록된 `t_end`가 `t_dl`보다 늦다. 따라서 기록된 `t_end`와 MISSED 플래그는 어긋나지 않는다. 종료 CAS가 성공하면 `t_end`는 CAS 직전의 시계이며, 그 시각에 MISSED가 없었다. MISSED를 더하는 쪽은 한 번만 `missed++`를 한다.
- **닫기 절차와 가져가기·종료·감시의 동시성.** 닫기 절차는 모든 QDEPTH 항목을 훑고 항목마다 읽은 값에 대해서만 CAS한다. QUEUED는 CANCELLED로, RUNNING(워커 종료 시)은 ABANDONED로 바꾼다. 감시와 같은 항목에서 마감을 넘긴 경우 MISSED를 같은 CAS에 넣고 `missed++`를 한 번 한다. 닫기 절차는 `head`와 `tail`로 범위를 정하지 않으므로 `head` 전진과 경합해도 항목을 놓치거나 두 번 닫지 않는다(R-c).
- **데몬 재시작.** 시작 정리는 `magic = 0`을 쓰고 이어서 `kind = NONE`을 쓴다. 게시 (a)는 `kind`만 보므로(A2) `kind = NONE`이 게시를 막는다. 그 전에 (a)를 통과한 게시는 시작 정리의 닫기 절차 전에 공개되었다면 CANCELLED로 정리되고, 시작 정리 뒤에 공개되었다면 (f)가 `kind`를 NONE으로 보고 REVOKED로 바꾼다. 어느 경우에도 옛 객체의 항목은 새 객체에 영향을 주지 않는다. 옛 객체에 붙어 있던 워커는 E_INACTIVE를 받고 `ttsched_init()`으로 새 객체에 붙는다. `gen`이 새 `instance_id`를 담으므로 `(gen, seq)`는 재시작 사이에서 확률적으로 유일하다(A14).

### 6.4 P1 한계

- **게시 문맥은 source당 하나다.** 게시에는 잠금이 없다. 두 문맥이 같은 source에 동시에 게시하면 그 source의 큐만 깨질 수 있고, libttsched는 이를 감지하지 않는다. 여러 생산자 게시는 P2다(F-13). 게시자는 워커 프로세스의 스레드이거나 SHM 객체에 쓸 권한이 있는 다른 프로세스다.
- **신뢰 대상 쓰기.** SHM 객체에 쓰는 모든 프로세스는 신뢰 대상이다. 객체별 격리와 데몬 하트비트는 P2다(F-14).
- **데몬 중단.** 데몬이 비정상 종료하면 옛 객체는 `kind = EVT`로 남는다. 게시는 수락되고 워커는 계속 처리한다. 워커의 종료 전이만 늦은 완료를 판정한다. 큐에서나 실행 중에 기한을 넘긴 Event는 아무도 실시간으로 감지하지 않는다. 다음 데몬의 시작 정리가 `DETECTED_LATE`로 확인하며 최종 통계에만 나타난다. 하트비트는 없다.
- **재등록 경계 계수 1건.** 활성화 중간에 들어온 게시 하나의 `arrivals`, `mit_viol`은 새 등록의 계수에 들어갈 수 있다(등록당 최대 1건). 이전 바인딩의 마지막 Job이 워커 종료 전이로 MISSED를 붙였고 `missed++`가 아직 실행되지 않은 채 새 등록의 기준값을 읽으면 그 `missed++`는 새 등록에 들어간다(바인딩당 최대 1건). 두 경우 모두 계수 한 건의 귀속이 바뀔 뿐 판정과 Event 처리에는 영향이 없다.
- **게시자 사망(A8).** 게시가 항목을 공개한 뒤 프로세스와 함께 죽으면 그 Event는 공개되어 있고 감시된다. (i) `tail` 전진 전에 죽었으면 다음 게시가 R-b로 복구해 `accepted`를 올리고, 그 게시의 (f)가 워커를 깨운다. (ii) `tail` 전진 뒤 (f) 전에 죽었으면 `accepted` 계수 한 건이 빠지고 복구되지 않는다. 두 경우 모두 워커는 다음 게시나 TimerMaster 변화 전까지 깨지 않으므로, 그 사이 이 Event는 처리되지 않고 마감 감시만 받는다. 게시가 (e) 이전에 죽으면 항목은 공개되지 않아 흔적이 없다.
- **u32 순환.** `seq`는 객체당 2^32번째 수락 뒤에 되돌아간다. 1 kHz 게시에서 약 49.7일이다. 링 논리는 순환에 안전하다(QDEPTH가 2의 거듭제곱이고 비교는 u32 등식이다). 그러나 `(gen, seq)`의 유일성은 같은 `gen` 안에서 2^32번 게시 이내에서만 보장된다.
- **활성화 틈.** 바인딩에서 `sched_ext` 전환(§5.1 (C) 4단계)과 슬롯 활성화(5단계) 사이에 게시는 E_INACTIVE를 받는다. 이 틈은 수락되지 않으므로 계수 불일치를 만들지 않는다. 이 틈을 받아들일지는 열린 이슈다(§12, L-14).
- **TT 재발행 중 게시.** 게시 (a)는 `kind`만 본다(A2). TT 테이블 재발행이 `magic`을 토글해도 EVT 게시는 거절되지 않는다. `ttsched_init()`은 `magic`과 `version`을 검사하므로 무효 객체를 새로 매핑하지 않는다. 이 결정으로 L-15는 닫힌다(§12).
- **wait 취소 API 없음.** 앱이 대기 중인 워커를 Event 없이 깨우는 수단은 P1에 없다(§12, L-8).
- **가져가기와 비활성화 틈(A12).** 워커가 `kind` 검사를 통과한 뒤 비활성화가 `kind = NONE`을 쓰고 닫기 sweep이 그 항목에 닿기 전에 가져가기 CAS가 성공하면 Job이 시작된다. 이 Job은 RUNNING으로 남아 워커가 끝까지 처리하고 `done()`으로 기록하며, 마감 감시를 계속 받는다. P1에서 이 동작을 허용한다. 가져가기 직전에 `kind`를 다시 읽으면 틈은 줄지만 닫히지는 않는다.
- **ttsched_init 매핑(A7).** 같은 객체에 `ttsched_init()`을 반복 호출해도 매핑 수는 늘지 않는다. 객체가 교체되면 옛 매핑은 남는다(한계).

### 6.5 테스트 목록 (libttsched 호스트 테스트, 신규 `sample-apps/tests/libttsched/`)

호스트에서 스레드 두 개 이상으로 같은 객체를 쓰는 테스트다. 경쟁 창을 넓히려고 지연 주입 지점(`TTSCHED_TEST_HOOK`, 테스트 빌드에서만 컴파일)을 둔다. 실행했다는 주장은 없다.

| 테스트 | 검증 |
|:--|:--|
| `LibttschedTest.FirstPostOnFreshObjectUsesSeqZero` (0으로 채운 객체, 첫 게시가 `(0, QUEUED)`를 공개하고 id는 `(gen, 0)`) | §6.2 게시 (e), FREE 상태 |
| `LibttschedTest.PublishedTailNotAdvancedIsRecovered` (게시자가 tail 전진 전에 죽은 상황을 재현하면 다음 게시가 tail을 먼저 전진시키고 `accepted`를 한 번 올린다) | R-b 복구 |
| `LibttschedTest.OversizedPayloadRejectedWithoutCounting` / `FutureOrDecreasingArrivalRejectedWithoutCounting` | E_INVAL, 계수 변화 없음 |
| `LibttschedTest.InactiveSourceNotAnArrival` (kind NONE이면 E_INACTIVE이고 `arrivals`가 늘지 않는다) | §6.2 게시 (a) |
| `LibttschedTest.MitViolationCountedOnlyAsCount` (MIT보다 가까운 도달은 수락되고 `mit_viol`만 올린다) | D-05, §5.6 |
| `LibttschedTest.FullQueueCountsOverflowAndHelpsHead` (터미널 항목은 head 도움으로 넘기고, 여전히 가득하면 E_FULL과 `overflow`) | R-a, 게시 (d) |
| `LibttschedTest.WaitTakesOldestFirst` | FIFO (R4) |
| `LibttschedTest.WaitInactiveBacksOffThenReturnsEInactive` | §6.1 back-off |
| `LibttschedTest.WaitWhileJobOpenIsEState` / `DoneWithoutOpenJobIsEState` | §6.1 호출 규약 |
| `LibttschedTest.InvalidOutcomeEndsJobAsDiscarded` (E_INVAL을 돌려주고 열린 플래그는 지워진다) | E_INVAL, done 호출 규약 |
| `LibttschedTest.DoneOnTimeIsNotMissedEvenWhenCollectedLate` (`t_end = t_dl`은 미스가 아니다) | REQ-008 §2.5, PRT-009 |
| `LibttschedTest.DoneOneTickAfterDeadlineIsMissedOnce` | 엄격 부등식, 계수 한 번 |
| `LibttschedTest.DoneAfterAbandonedReturnsEInactiveAndClearsOpen` | 프로토콜 4 (i) |
| `LibttschedTest.JobLogOverflowCountsDropOnly` | Job 로그 손실 계수, 카운터 불변 |
| `LibttschedTest.StaleGenerationRecordCancelledByWait` | §6.2 close_stale |
| `LibttschedTest.InitReattachesAfterInstanceChange` | §6.1 재초기화 |
| `LibttschedTest.HandleSurvivesInitAndResolvesByName` | 핸들 재해석 |
| `RaceTest.PostVsDeactivateStoreThenLoad` (두 스레드, 게시의 publish→load kind와 비활성화의 store kind→load word, 둘 다 놓치지 않음) | 저장 후 적재 쌍 1 |
| `RaceTest.ActivationGenVsPostStoreThenLoad` (활성화의 gen 저장과 게시의 gen 적재) | 저장 후 적재 쌍 2 |
| `RaceTest.WaitNoLostWakeupAgainstPublish` (대기자의 c 읽기와 게시의 공개 및 counter 증가 사이) | 깨움 유실 없음 |
| `RaceTest.TakeVsCloseExactlyOnce` (가져가기 CAS와 닫기 CAS) | FIFO와 한 번 가져가기 |
| `RaceTest.PostRollbackVsWorkerTake` (롤백 CAS와 가져가기 CAS, 결과는 수락으로 일치) | 롤백 대 가져가기 |
| `RaceTest.PostPublishedAfterDeactivationRevoked` (게시 (a) 통과 뒤 비활성화, 공개 뒤 (f)에서 REVOKED) | 게시 롤백 경로 |
| `RaceTest.CloseVsWorkerEndAbandon` (닫기 CAS와 종료 CAS, 정확히 하나가 이긴다, 종료가 졌다면 E_INACTIVE) | 프로토콜 4 (i)–(iv) |
| `RaceTest.MonitorVsDoneSameBoundary` (`t_end`가 `t_dl` 근처일 때 감시 CAS와 종료 CAS, 기록된 `t_end`와 MISSED가 일치하고 `missed`는 한 번) | 프로토콜 4, 6 |
| `RaceTest.MonitorMarkBetweenClockAndCas` (감시 표시가 종료의 시계 읽기와 CAS 사이에 들어오면 종료가 시계를 다시 읽고 late로 기록) | 프로토콜 4 재시도 |
| `RaceTest.HeadHelpVsHeadHelp` (두 도우미의 head CAS, 한 번만 전진) | R-a |
| `RaceTest.StaleGenCancelVsPostRollback` | close_stale 대 롤백 |
| `RaceTest.DaemonRestartInvalidateVsPost` (kind = NONE 이후의 게시는 E_INACTIVE이고 계수되지 않는다, A2) | 프로토콜 7 |
| `RaceTest.AbandonedEntryReusedBeforeDone` (RUNNING 기록이 ABANDONED로 닫혀 head가 넘긴 뒤 같은 칸에 seq s+QDEPTH가 게시된 상태에서 워커가 done을 부르면 E_INACTIVE이고 그 항목을 덮어쓰지 않는다) | 프로토콜 4 (i), A6 |
| `RaceTest.TakeAfterKindNoneCompletesNormally` (kind = NONE 뒤에도 가져가기 CAS가 sweep보다 먼저 성공하면 Job은 정상 종료되어 기록되고 감시를 받는다) | §6.4 가져가기와 비활성화 틈, A12 |
| `RaceTest.RecoveredPublishedRecordWakesWorkerOnlyOnNextPost` (tail 전진 전에 죽은 게시자의 항목은 다음 게시의 (f)에서 워커를 깨운다) | 게시 (d)와 (f), A8 |
| `LibttschedTest.DiedAfterTailAdvanceMissesOneAcceptedCount` (tail 전진 뒤 (f) 전에 죽은 게시자의 항목은 `accepted`를 한 건 빼고 복구되지 않으며, 다음 게시가 워커를 깨운다) | A8 (ii), §6.4 |
| `LibttschedTest.RepeatedInitDoesNotGrowMappings` (같은 객체에 `ttsched_init()`을 1000회 호출한 뒤 `/proc/self/maps`의 매핑 수가 변하지 않는다) | A7, §6.1 |

---

## 7. 샘플 앱

### 7.1 `sample-apps/src/evttask.c` (신규, 범용 Event 생성기 — 수용 기준의 기준 검증 수단)

Event 소스의 종류(타이머, datagram, pipe, 자기 생성)만 바꾸고 TIMPANI 설정과 워커 코드는 그대로 둔다. 앱 쪽 명시적 연결은 `ttsched_evt_open`, `ttsched_evt_post`, `ttsched_evt_wait`, `ttsched_evt_done`만 쓴다. `--marker`와 "어댑터 K" 경로는 없다.

```text
evttask --name cbs_task --source {timerfd|udp:<port>|unixdgram:<path>|pipe|self} --mit-us 18000 --wcet-us 1500
        [--burst N] [--overrun-us U] [--midjob-block-us B] [--count K] [--log FILE]

threads:
  worker (comm = --name):
     ttsched_init();
     loop:
        if (ttsched_evt_wait(&ev) < 0) { ttsched_init(); continue; }     // E_INACTIVE: back-off already done inside wait
        run_nsqrt_for(wcet_us +/- overrun_us);                             // one Event = one Job (R4)
        if (midjob_block_us) nanosleep(midjob_block_us);                   // blocking inside a Job: observed, not covered by admission (§3.3 assumption 4)
        ttsched_evt_done(TTSCHED_EVT_COMPLETED);                           // mandatory
  source (not an L2 task; the single posting context of the source):
     src = ttsched_evt_open(--name);
     timerfd / self: generate at MIT (MIT x N with --burst N); post with t_arr = 0 (now)
     udp / unixdgram / pipe: receive one datagram per Event; post it inline (payload = 4-byte generator sequence number)
     every post: append (gen, seq, t_arr, outcome) to --log for offline correlation with the TIMPANI-N Job log
```

- `--midjob-block-us`는 A-06 제약을 재현하는 옵션이 아니다. Job 중 블로킹은 허용되고 관측되므로, 이 옵션은 블로킹이 응답 시간과 수용 판정의 보장 밖이라는 경우를 만든다.
- `--count K`로 K개 Event 뒤에 생성을 멈춘다. 워커의 `done` 결과는 `--log`에 남긴다.
- `cbstask.c`는 유지하되 README에서 `evttask`를 권장 생성기로 표기한다.
- **C 측정용 옵션(필요한 도구 작업, 미구현, HLD-008 §8.2, OI-23):** 입력 내용 선택(payload 내용 지정), 기록된 입력 순서의 재생(replay), 워커가 `ttsched_evt_wait` 복귀 직후와 다음 호출 직전에 `CLOCK_THREAD_CPUTIME_ID`를 읽어 그 차를 Job당 CPU 시간으로 `--log`에 남기는 기능이다(`done` 포함. wait 안에서 쓰는 CPU 시간은 들어가지 않는다). 도달 간격은 `--mit-us`와 `--burst`로 주기와 묶음만 만들 수 있다. 위 세 항목은 설계 목록이며 `evttask.c`에 아직 없다. 옵션 이름은 구현 때 정한다.

### 7.2 FCW 어댑터 축소 (`examples/someip/radar_based_fcw/fcw_consumer.cpp`)

현재 파일은 `timpani_rt.h`, `timpani_evt_register`, `timpani_evt_mark_arrival`, `timpani_cbs_bind_thread`, `cbs_cv`를 쓴다. 수정 항목과 이유는 HLD-008 §8.4에 있다. 여기서는 수정 뒤의 코드 모양을 보인다.

| 현재 호출 (파일 내 위치) | 수정 뒤 |
|:--|:--|
| `#include <timpani_rt.h>` (7행) | `#include <libttsched.h>` |
| `timpani_rt_init()` (main, 187행) | 삭제. 워커가 `ttsched_init()`을 부른다 |
| `timpani_evt_register(wl, task, svc, inst, evt, 18000)` (main, 198행) | `g_src = ttsched_evt_open("cbs_task")`. MIT는 워크로드 선언(`min_inter_arrival_us`)이며 앱 코드에 없다 |
| `timpani_evt_mark_arrival(svc, inst, evt, seq)` (on_obstacle_alert_received, 142행) | `ttsched_evt_post(g_src, t_arr, target, sizeof(*target), &id)` |
| `cbs_cv.wait(lock, ...)`, `cbs_cv.notify_one()`, `notify_all()` (62, 74, 150, 227행) | `ttsched_evt_wait` / `ttsched_evt_done` 호출. 조건 변수와 큐는 삭제 |
| `timpani_cbs_bind_thread()` / `timpani_cbs_unbind_thread()` (86, 129행) | 삭제 |
| `cbs_queue`(`std::queue`, `cbs_mtx`, `cbs_cv`) | 삭제. 수락된 Event의 payload는 `ev.payload`로 바로 온다 |

수정 뒤 코드 모양(스케치, 컴파일 검증 전):

```cpp
#include <libttsched.h>

static ttsched_evt_t g_src = -1;                       // handle of the L2 task "cbs_task"
static std::mutex g_post_mtx;                          // one posting context: vsomeip may call the handler from several threads
static std::atomic<uint64_t> g_overflow{0}, g_rejected{0}, g_invalid{0};

// vsomeip handler (middleware thread). No service, instance or event IDs reach TIMPANI.
void on_obstacle_alert_received(const std::shared_ptr<vsomeip::message>& msg) {
    const uint64_t t_arr = clock_ns(CLOCK_MONOTONIC);     // first statement (A17): lock wait and parsing fall inside the response time
    /* payload check and RadarTarget parsing (16 bytes): unchanged */
    std::lock_guard<std::mutex> lk(g_post_mtx);           // one posting context per source (§6.4)
    ttsched_evt_id id;
    const int rc = ttsched_evt_post(g_src, t_arr, target, sizeof(*target), &id);
    if (rc == TTSCHED_EVT_E_FULL)          g_overflow++;   // not an Event: the worker never sees it
    else if (rc == TTSCHED_EVT_E_INACTIVE) g_rejected++;   // workload not active or daemon down: not an Event
    else if (rc < 0)                       g_invalid++;
}

void cbs_worker_loop() {
    pthread_setname_np(pthread_self(), "cbs_task");       // task_id = comm (unchanged)
    ttsched_init();
    for (;;) {
        struct ttsched_evt_event ev;
        if (ttsched_evt_wait(&ev) < 0) { ttsched_init(); continue; }   // E_INACTIVE: back-off already done inside wait
        const RadarTarget* target = reinterpret_cast<const RadarTarget*>(ev.payload);
        run_collision_check(*target);                     // -be / --overrun-ms injection (CPU burn), unchanged
        ttsched_evt_done(TTSCHED_EVT_COMPLETED);          // mandatory; use TTSCHED_EVT_DISCARDED only for an explicit app policy
    }
}

int main(int argc, char* argv[]) {
    /* option parsing: --overrun-ms, -be, --burst-ms, --tt-overrun-ms unchanged; --wakeup-delay-ms moves to the receive side */
    g_src = ttsched_evt_open("cbs_task");
    if (g_src < 0) return EXIT_FAILURE;
    std::thread(cbs_worker_loop).detach();            // the worker starts before the receive path (HLD-008 §8.1)
    /* vsomeip app init, request_service, register_message_handler, subscribe, app->start(): unchanged */
    /* print g_overflow / g_rejected / g_invalid */
    return 0;
}
```

- `t_arr`은 핸들러의 첫 문장에서 읽는다(A17). 수신 처리와 잠금 대기는 응답 시간에 들어간다(HLD-008 §8.2). 같은 source의 post가 여러 스레드에서 불리면 `t_arr` 순서가 어긋나 E_INVAL이 날 수 있다. 핸들러가 한 스레드에서만 불린다는 H-8 확인을 이 절의 전제로 두며, 확인 전에는 E_INVAL을 `g_invalid`로 계수한다.
- 워커는 프레임별 콘솔 출력(`std::cout`)을 하지 않는다. 로그 링은 추가하지 않는다(A20). 프레임과 Job의 대응은 Job 로그와 TIMPANI 추적의 `(gen, seq, t_arr)`로 한다. 결과로 FCW 데모의 프레임 단위 콘솔 출력은 줄어든다(§12, L-16).
- `DISCARDED`는 앱이 명시적으로 폐기하는 정책이 있을 때만 쓴다. FCW에는 그런 정책이 없다.
- 생산자(`mock_radar_producer.cpp`)는 `#include <timpani_rt.h>`를 지우고 `timpani_rt_init()`과 `timpani_tt_register(...)`를 `pthread_setname_np(pthread_self(), "tt_task"); ttsched_init();`으로 바꾼다. 나머지는 그대로다.
- 제거: `libtimpani_rt`(`timpani_rt.h`, `timpani_rt_shm.h`, `libtimpani_rt.cpp`), `/timpani_rt` SHM, 라이브러리의 MIT 판정과 3 ms E2E 판정. 유지: `--overrun-ms`, `-be`, `--burst-ms`, `--tt-overrun-ms`. `--wakeup-delay-ms`는 수신 측으로 옮긴다.

---

## 8. 타이밍 분석 (Worst-case paths)

아래 값은 계획용 후보값이다. 합의되지 않았고 Linux 타깃에서 측정되지 않았다. 측정 방법 열은 어떤 기록으로 값을 얻는지 적는다. 측정 전의 값을 보장으로 쓰지 않는다.

| 경로 | 구성 요소 | 예산(후보) | 측정 방법 | 대응 |
|:--|:--|:--|:--|:--|
| 도달 → 실행 가능 (post → runnable, $L_{post}$) | 게시 (a)–(f) 원자 연산과 futex 깨움 → 워커 활성화(`runnable`) | ≤ 50 µs p99.99 (후보) | Job 로그의 `t_arr`와 같은 워커 대기 상태에서 대응하는 실행 구간 레코드의 `t_act` 차 | TMG-016, TIM-011 |
| 도달 → 첫 실행 (delivery, $L_{del,max}$ = $L_{post}$ + $L_{wake}$) | 위 + 런큐 → 첫 실행(TIM-001, $L_{wake}$ = 50 µs 후보, 유휴이고 TT 없는 CPU) | ≤ 100 µs 후보 (수용식의 $L_{del,max}$) | Job 로그 `t_arr`와 실행 구간의 `t_start`(0 제외) 차 | TMG-016, TIM-011, §3.3 |
| 활성화 → 실행 개시 (TT 중) | 위 + $B_{tt}$ (TT 연속 점유) | ≤ $B_{tt}$ + 50 µs (후보) | 실행 구간 `t_start − t_act`를 TT 슬롯 기록과 대조 | TMG-002 |
| 윈도우 끝(`replenish_at`) → 실행 가능 (스로틀된 서버, 유휴 CPU) | 깨움 지터 → `bpf_prog_test_run` → `kick_cpu` → `dispatch` promote(≤ 16 반복) | ≤ 100 µs p99.99 (후보) | Replenisher 로그와 `remaining_us` 복원 시각 | TMG-003 |
| 예산 초과 | 슬라이스 만료 = 틱 경계(HZ=1000: 최대 1 ms) + `stopping` 차감 | δ_ovr = 1 ms (후보, nohz_full에서 틱 재시작 지연 측정) | BUDGET_EXCEED 시각과 `budget_used_us`, 틱 주기 대조 | TMG-004, §3.3 δ_ovr |
| 마감 감지 (TIM-009) — t_dl → MISSED 설정 | DeadlineMonitor 주기 스캔(1 ms 후보) + 감시 스레드 깨움 지터. 대기·실행·스로틀·유휴 모두 포함 | ≤ 2 ms 후보 (scan 1 ms + 지터) | 미스 기록의 `t_detect − t_dl`; 큐 대기, 스로틀, 유휴 CPU 시나리오별 | TMG-014, §5.9 |
| 마감 실패 보고 (TIM-010) — MISSED 설정 → 보고 수신 | FaultMonitor 폴링(≤ 100 ms) → gRPC Write → O 수신 | ≤ 200 ms 후보 (보내진 보고에 대해서만) | 미스 기록 `t_detect`와 O 수신 로그 타임스탬프 차 | TMG-015, §5.5 |
| 완료 수집 지연 | 워커 종료 뒤 TIMPANI-N 수집까지 | 바운드 없음. 마감 판정에 영향이 없다(§1.2) | Job 로그 `t_end`로 판정하므로 측정 대상이 아니다 | PRT-009 (수집 지연은 마감 실패가 아니다). TMG-005는 철회 |
| 게시·대기·완료 비용 | 원자 접근 post 21회, wait 가져가기 12회, done 8회(§6.2에서 센 후보 값), 최대 64 B 복사, futex 최대 1회 (post) | ≤ 2 µs 후보 (측정 필요) | 호스트 마이크로벤치와 타깃 측정 (post 평균과 p99) | TMG-011 |

- TMG-005(완료 시점 DMISS 판정)는 SRS에서 철회되었다. 마감 감지는 TMG-014가 맡고, 완료 시점의 판정은 완료 기록의 MISSED(`LATE_COMPLETION`)로 남는다.
- 표에 없는 TMG 항목은 최악 경로가 아니라 시험으로 확인한다: TMG-007(바인딩 시간 $T_{bind}$, §5.2의 재스캔 주기 + 스캔 시간), TMG-009(계수 정확성, §10 AC-008-01), TMG-010(CPU 고정, §10 AC-008-06), TMG-012(폴트 보고 내용, §10 AC-008-11), TMG-013(재적용, §10 AC-008-09).
- L1 영향(TMG-008): L2 변경으로 TT 경로에 더해진 코드는 `select_cpu`의 분기 1개와 `dispatch`의 `cpu_is_isolated` 검사뿐이다. kick 프로그램은 TT 슬롯이 활성인 CPU에 kick을 보내지 않는다(§4.4). 감시 스레드는 TT 슬롯과 같은 CPU를 쓰지 않도록 CPU를 고르며, 그 영향은 PoC 항목 P-1에서 측정한다.

---

## 9. 오류 처리 및 실패 모드

| 상황 | 처리 |
|:--|:--|
| L2 태스크 미발견 (테이블 적용 시) | 적용 실패가 아니다. 미해결 목록 → 재스캔. `TableApplied.success=true`, 로그 WARN |
| `cbs_map` / `task_meta_map` 갱신 실패 | 해당 엔트리만 `apply_error`로 기록하고 나머지를 계속한다 |
| ringbuf 공간 부족 | `fault_drop_count++`. JobTraceCollector가 주기적으로 읽어 WARN |
| `timpani_replenish_kick` 로드·실행 실패 | 시작 시 감지하면 `KickBackend::KickerThreads`로 자동 전환하고 로그 ERROR |
| SHM v1 소비자 (구 `libttsched`) | magic 또는 version이 맞지 않으므로 게시·대기가 E_INACTIVE를 받는다. **NFR-003(TT 무회귀)의 명시적 예외**: v1로 빌드된 앱은 다시 빌드해야 한다. 배포 순서: 앱을 `libttsched` v2로 빌드 → timpani-n(v2)과 함께 교체 |
| MIT = 0 (전환 규칙도 불가), 또는 T > MIT, D > MIT | O에서 `ParamInvalid` 거부 (§3.2) |
| task_id가 15 B 초과, 또는 노드 안에서 중복 | O에서 `ParamInvalid` 거부 (§3.2) |
| 태스크 종료 | `exit_task` 기록의 `bind_gen`이 추적 중인 바인딩과 같으면 deactivate(워커 종료 → 닫기 규칙, RUNNING → ABANDONED, 마감을 넘겼으면 MISSED와 DETECTED_LATE). 다르면 무시(stale). 그 뒤 미해결로 복귀 |
| source 미등록 또는 비활성, SHM 객체 없음(데몬 기동 전 또는 정상 종료 후) | 게시는 E_INACTIVE이며 도달로 계수되지 않는다. 앱은 `ttsched_init()` 뒤 재시도한다. 등록 뒤 핸들을 다시 얻을 필요가 없다 |
| 게시 중 SHM을 읽기 전용으로만 매핑 | 게시와 대기가 E_INACTIVE를 받는다. 다시 매핑하지 않는다. 쓰기 권한은 `0666`이다(§5.6, OI-15) |
| 큐 가득 참 (E_FULL) | `overflow++`. 거절은 Event가 아니다. 앱은 거절을 세고 처리하지 않는다. 워커가 밀린 경우 `arrivals − accepted`로 관측한다 |
| 활성 Job 중 Event 도달 | QUEUED로 누적(R2). 현재 Job이 `done`을 부르면 다음 `wait`가 수면 없이 복귀한다(R3). 유실 없음 |
| 활성 Job 중 `wait` 호출 또는 Job 없이 `done` 호출 | E_STATE. 호출 순서 위반이다(앱 버그) |
| 잘못된 인자 (payload 초과, 미래 또는 이른 t_arr, 잘못된 outcome) | E_INVAL. 게시의 인자 오류는 아무것도 계수하지 않는다. done의 outcome이 잘못되면 Job을 DISCARDED로 끝내고(`discarded` 계수) E_INVAL을 돌려준다 |
| 게시자가 (a)–(e) 사이에서 죽음 | 공개 전이면 흔적이 없다. 공개 뒤 tail 미전진이면 다음 게시가 복구하고 `accepted`를 올린다(R-b). tail 전진 뒤 (f) 이전이면 `accepted`가 한 건 빠지고 복구되지 않는다. 두 경우 모두 워커는 다음 게시나 TimerMaster 변화 전까지 깨지 않으며, 그 사이 Event는 마감 감시만 받는다(§6.4, A8) |
| 워커가 Job 중 죽음 | RUNNING 기록은 TaskRegistry의 종료 감지 → 닫기 규칙(ABANDONED)으로 닫는다. 감시는 RUNNING이 터미널이 될 때까지 계속 본다. 마감을 넘겼으면 MISSED와 DETECTED_LATE |
| 워커가 done 전 또는 닫힌 뒤 done 호출 | ABANDONED이면 E_INACTIVE이고 열린 플래그를 지운다. 닫기 규칙이 RUNNING을 닫은 뒤의 늦은 종료는 이 경로로 복귀한다 |
| 데몬 비정상 종료 (SHM 객체가 남고 `kind = EVT`) | 게시는 계속 수락되고 워커는 계속 처리한다. 워커의 종료 전이만 늦은 완료를 판정한다. 큐 대기와 실행 중 만료는 실시간으로 감지되지 않으며, 다음 시작 정리에서 `DETECTED_LATE`로 늦게 확인된다. 실시간 보고 없음(§6.4, §1.3) |
| 데몬 재시작 (정상 또는 비정상 종료 후) | 시작 정리: `magic = 0` → 모든 슬롯 `kind = NONE` → 닫기 규칙 → 깨움 → 최종 계수 기록 → unlink → 새 객체 생성(§5.6 프로토콜 7). 앱은 E_INACTIVE를 받고 `ttsched_init()`으로 새 객체에 붙는다. `seq`는 0부터 다시 시작하지만 `gen`이 달라서 `(gen, seq)`는 재시작 사이에서 확률적으로 유일하다(A14) |
| 데몬 재시작 중 게시 (`kind` NONE 구간 또는 옛 객체) | (a)에서 `kind`가 NONE이면 거부되어 E_INACTIVE이다. (f)에서 `kind`가 NONE으로 보이면 롤백한다(A2). 공개 뒤 정리된 항목은 CANCELLED나 REVOKED로 닫히며 새 객체에 영향이 없다 |
| 감시 스레드 정지 또는 과부하 (monitor stall) | 감지가 TIM-009 밖으로 늦어진다. 마감 실패는 워커 종료나 닫기 규칙에서 늦게 확인된다. 감시 스레드의 주기 카운터를 프로세스 내부 로그에 남기고 주기 초과를 WARN한다. 계수 경로는 종료 시점에 맞춰진다 |
| 미스 기록 큐 가득 참 (miss-record queue overflow) | MISSED 표시와 `missed++`는 CAS와 원자 연산으로 이미 끝났으므로 유지된다. 기록만 버리고 `miss_drops`를 센다. 계수 경로의 보고 근거는 영향이 없고 상세 경로만 줄어든다 |
| Job 로그 가득 참 (Job log overflow) | 항목을 버리고 `log_drop`을 센다. 워커 계수(`completed`, `missed` 등)는 영향이 없다. 해당 Job의 상세 행이 없으며 추적에 `log_drop` 차이로 나타난다 |
| 상세 경로 중복 (같은 (gen, seq)의 미스 기록과 Job 로그) | `(task, gen, seq)`로 중복 제거하여 상세 한 건으로 남긴다 |
| 재등록 경계의 게시 1건, 마지막 Job의 `missed++` 1건 | 계수 한 건이 이전 또는 새 등록에 귀속될 수 있다(§6.4 한계). 판정과 Event 처리에는 영향이 없다 |
| 테이블 재적용 (파라미터 불변) | 아무것도 쓰지 않는다. 슬롯의 `gen`, `kind`, 계수, `run_seq`가 그대로다(§5.1 (C) D-17(a)) |
| 테이블 재적용 (파라미터 변경) | 닫기 규칙으로 바인딩을 해제하고 새 `gen`으로 다시 활성화한다. 진행 중 Job은 옛 `gen`의 기록으로 닫힌다 |
| TT 재발행 (`magic` 토글) | 게시는 거절되지 않는다(A2). 게시 (a)와 대기는 EVT 슬롯의 `kind`만 보며, TT 재발행은 EVT 슬롯의 `kind`를 바꾸지 않는다(§6.4, §12 L-15 닫힘) |
| 같은 comm을 가진 스레드가 노드에 여럿 | 첫 번째 매칭을 쓰고 WARN(현행). 한 source의 wait 또는 done을 부르는 스레드가 둘이면 계약 위반이다(§6.1) |
| L2 task 수가 SHM 슬롯 수(64)를 넘음 | 그 엔트리는 적용 오류다. 슬롯은 이름에 고정 배정되므로(§5.6) 데몬 재시작 전에는 재사용되지 않는다 |
| `CbsConfig.cpu` 없음 또는 격리 CPU 아님 | 그 엔트리는 적용 오류(§5.1 (B)) |
| 활성화 틈 (sched_ext 전환 뒤 슬롯 활성화 전) | 게시는 E_INACTIVE이고 계수되지 않는다(§6.4, §12 L-14) |
| Job 중 블로킹 (mid-job blocking) | Job 경계는 명시적이므로 Job은 계속 하나다. 실행 구간은 블로킹마다 나뉜다(§4.5). 블로킹 뒤 깨움은 열린 윈도우 안에서 예산을 새로 주지 않는다. 블로킹 Job은 수용 판정의 보장 밖이며 마감은 감시된다 |

---

## 10. 테스트 훅 및 수용 기준 매핑

AC-008-01 ~ 19 전체를 매핑한다. AC-008-13 ~ 16은 신규다. AC-008-17 ~ 19는 v5.1에서 추가했다. 수단은 `evttask`(§7.1), FCW(§7.2), `mock_pullpiri.py`, GTest 또는 절차다. "훅"은 판정에 쓰는 기록이나 계수다. Job 로그는 `(gen, seq)`로 Event와 대응한다.

| AC-008 | 수단 | 훅 |
|:--|:--|:--|
| 01 (한 Event–한 Job, 1:1) | (a) `evttask --source timerfd`, `udp`, `pipe`, `self` 각 100,000 Event, MIT 준수. (b) 같은 조건에서 생성기 로그 수 확인 | 정지 상태에서 `completed + discarded + cancelled + abandoned = accepted`(I1). 생성기의 `(gen, seq)` 집합 = Job 로그의 `(gen, seq)` 집합(완료·폐기분). `started = completed + discarded + abandoned`. MIT를 어기는 구간이 없으므로 `mit_viol = 0` |
| 02 (TT 무회귀) | TT 테이블 고정 + `evttask --burst` A/B, 1 h | 추적의 tt 행 지터 p99.99, FaultMonitor의 L1 DMISS = 0 |
| 03 (CBS 예산 집행, BUDGET_EXCEED) | `evttask --overrun-us`, `fcw_consumer -be` | `cbs_map` 샘플(bpftool), BUDGET_EXCEED 수(에피소드당 1회), Replenisher 로그와 `remaining_us` 복원 시각 |
| 04 (마감 감지와 스로틀, 한 번만) | (a) `evttask --overrun-us`로 D 초과, FCW `--overrun-ms 10`. (b) 실행 시간은 D 미만이지만 뒤에서 기다리는 경우: `evttask --burst N` 또는 D보다 긴 TT 블록과 겹치게 도달 | Job마다 마감 실패 1회(중복 0). (승인 후) (b)에서 `NOT_STARTED`, `THROTTLED` 플래그. 미스 기록의 `t_detect − t_dl` ≤ TIM-009 후보. O 수신 로그 ≤ TIM-010 후보(보내진 보고만) |
| 05 (외부 생성기, 전달 지연) | `evttask --source udp` 외부 생성기(타 노드 또는 CPU) | L_wake = `t_start − t_act`를 Gap 상태(TIM-001)와 TT 점유 상태(TIM-002)로 나누어 p99.99 집계. L_post = `t_act − t_arr`(TIM-011). L_del = `t_start − t_arr`(TMG-016, Job 로그 `t_arr`와 실행 구간 `t_start`) |
| 06 (격리 CPU 집합) | 1 h 정격 + 폭주 | `perf sched` 또는 `sched_switch` 트레이스에서 `cbs_task`의 CPU 집합이 배정 CPU 하나뿐이다 |
| 07 (MIT 위반의 계수와 보고) | `evttask --burst N` (간격 MIT/3). FCW 재현: `mock_radar_producer --burst-ms 6` | `mit_viol` = 생성기 위반 수(도달 기준). 보고된 MIT_VIOLATION의 `count`를 모두 더하면 `mit_viol` 증분과 같다(보고는 T_s 윈도우마다 한 건이다, §5.5). (승인 후) 마감 실패의 `flags & 0x1`(MIT_RELATED). 다른 L2 task의 마감 실패 = 0 |
| 08 (수용 판정) | GTest `GlobalSchedulerTest.FifoAdmission_*`(기존 태스크 재검사 포함), `SchedInfoServiceTest.RejectRestoresPreviousWorkload` | 단위 테스트 결과(§3.5) |
| 09 (추가·제거 반복 안정성) | `mock_pullpiri.py` 추가·제거 10회 | 데몬 PID 불변. `bpftool map dump`의 잔존 0. SHM의 슬롯 이름 고정 유지, 계수는 누적. 이전 `bind_gen`의 종료 기록이 새 바인딩을 해제하지 않는다(`TaskRegistryTest.StaleExitRecordIgnored`) |
| 10 (기동 지연) | `evttask`를 테이블 적용 후 시작 | `Registered CBS task` 로그 시각 − 기동 시각 |
| 11 (폴트 보고 내용) | `mock_pullpiri.py` 수신 로그 | 유형, `count`, (승인 후) `flags`, `job_seq`, `job_gen`. 승인 전에는 `count`만 확인한다 |
| 12 (미들웨어 식별자 분리) | `grep -rniE 'someip\|vsomeip\|service_id' timpani-o timpani-n sample-apps` = 0. `evttask --source timerfd/udp/unixdgram/pipe/self`를 TIMPANI 설정 변경 없이 전환하여 01~07 재확인 | 0건, 그리고 01~07 결과가 소스 종류와 무관하다. (`instance_id`는 SHM 객체의 식별 필드(§5.6)이며 미들웨어 식별자가 아니므로 검사 패턴에 넣지 않는다) |
| 13 (A 처리 중 B 도달: 독립 마감, 연속 Job 분리) | (a) `evttask` 워커가 긴 `--wcet-us`로 A를 처리하는 동안 B가 도달. (b) A 완료 직후 B가 이미 QUEUED인 상황(`--burst 2`, 수면 없이 연속) | (a) B의 `t_dl = t_arr_B + D`가 A의 시각으로 바뀌지 않는다(Job 로그의 `t_dl`). (b) Job 로그에 두 항목(서로 다른 `seq`, B의 `t_begin`이 A의 `t_end`보다 뒤다. 차이는 done 뒤 wait 호출까지의 지연이다). 실행 구간 기록은 한 건(R3)이다(§4.5). 판정은 Job별: B만 늦으면 B만 마감 실패 |
| 14 (대기·스로틀 중 감지, 늦은 수집, 중복 없음, 폐기·취소) | (a) 큐 대기: 워커를 `--midjob-block-us`로 붙잡아 다음 Event가 `t_dl`을 넘기게 한다. (b) 스로틀: `--overrun-us`로 예산을 소진하고 대기 Event가 기한을 넘긴다. (c) 정시 완료의 늦은 수집: 테스트 훅으로 JobTraceCollector 수집을 지연. (d) 마감 뒤 폐기 또는 CANCELLED: 테스트 훅으로 종료 전이를 `t_dl` 뒤에 둔다 | (a) 미스 기록 `t_detect − t_dl` ≤ TIM-009 후보, 플래그 NOT_STARTED. (b) THROTTLED. (c) 마감 실패 없음(`t_end ≤ t_dl`). (d) 마감 뒤 종료 또는 닫기는 마감 실패로 계수, `missed`는 Job당 1회(`RaceTest.MonitorVsDoneSameBoundary`와 함께 확인) |
| 15 (수락·넘침·취소·재등록·재시작에서 다른 Event의 데이터를 처리하지 않음; 옛 세대 기록이 상태를 바꾸지 않음) | (a) 넘침: 워커를 붙잡고 QDEPTH를 넘는 `evttask --burst`. (b) 재등록: `mock_pullpiri.py`로 파라미터 변경 중 부하. (c) 데몬 재시작 중 부하 | payload에 생성기 순번을 넣어 워커가 받은 payload 순번과 `(gen, seq)`에 대응하는 생성기 순번이 같다(불일치 0). `overflow`는 E_FULL 수와 같다. 재등록 뒤 옛 `gen`의 기록은 계수와 상태를 바꾸지 않는다(`ActivationTest.ClosesLeftoverRecordsBeforePublishing`, `DeadlineMonitorTest.StaleGenRecordChangesNothingOfNewRegistration`) |
| 16 (FCW와 비-SOME/IP 앱의 같은 계약: 간섭) | (a) 서로 다른 격리 CPU: FCW(`-be`로 과실행)와 비-SOME/IP 앱(HLD-008 §8의 Unix datagram 센서 앱)을 함께 실행. (b) 같은 CPU, 재검증된 파라미터: FCW D를 6000 µs로 올리고 두 번째 앱의 D를 §3.3 식이 통과하도록 정한다. (c) 기본 FCW 파라미터 + 같은 CPU의 두 번째 L2 task | (a)와 (b): 다른 앱의 마감 실패 = 0, 응답 시간 분포가 과실행 없는 기준과 같은 범위(TMG-016 후보 안). (c): 수용 판정이 `DeadlineInfeasible`로 거부한다(`GlobalSchedulerTest.FifoAdmission_SecondL2Rejected`와 `mock_pullpiri` 적용 로그). 숫자 근거는 HLD-008 §8과 §3.3 산술이다 |
| 17 (TT 없는 CPU의 CBS 실행, FR-O-022·FR-N-032·FR-B-025) | (a) TT 슬롯이 없는 노드 또는 혼합 노드의 TT 없는 CPU에 L2 task를 배치하고, `evttask --source timerfd` 또는 `udp`로 첫 Event를 보낸다. (b) 마지막 L2 활동 뒤 $T_s$ 이상 쉰 뒤 Event를 보낸다. (c) `evttask --overrun-us`로 예산을 소진한 뒤 같은 CPU에 다른 활동을 두지 않는다. (d) 스로틀 중이나 QUEUED 상태에서 마감이 지나게 둔다. BPF 트레이스로 TT_START와 TT_END를 센다(계획 테스트 `EvtIntegration.TtFreeCpuCbsLifecycle`) | (a) 수락, 처리, 종료와 Job 기록이 한 건 생긴다. (b) 시작 지연이 TIM-011과 TIM-001의 합의 상한 안이고, 시작 구간에 $C_s$가 주어진다. (c) 보충이 TIM-003의 합의 상한 안에 끝난다. (d) 마감 실패가 TIM-009의 합의 상한 안에 감지된다. 전 구간에서 그 CPU의 TT_START와 TT_END는 0건이다 |
| 18 (TT 최초 추가·마지막 TT 제거 시 L2 보존, RTU-001) | L2 엔트리는 그대로 두고, 그 CPU의 첫 TT 태스크 추가와 마지막 TT 태스크 제거를 `mock_pullpiri.py`로 10회 반복한다. 반복 중에는 Event 부하를 건다(계획 테스트 `EvtIntegration.TtAddRemoveKeepsL2State`) | 잔여 예산과 보충 윈도우가 초기화되지 않는다. 열린 Event의 `(gen, seq)`와 `t_dl`이 바뀌지 않는다. 갱신 때문에 생긴 CANCELLED와 ABANDONED가 0건이다. 데몬 PID가 바뀌지 않는다. Timer Master 스레드 재구성과 epoch 갱신은 일어날 수 있으며 L2 상태 초기화의 사유가 아니다 |
| 19 (잘못된 cpuset 배치의 명시적 실패, CBS-008) | 배정 CPU가 워커의 허용 CPU 집합 밖에 있도록 cpuset을 일부러 준 뒤 `mock_pullpiri.py`로 등록한다. 그 뒤 cpuset을 바로잡는다(계획 테스트 `TaskRegistryTest.WrongCpusetFailsExplicitly`) | 바인딩을 시도한 시점에 오류 로그가 남는다. 그 태스크는 SCHED_EXT 정책으로 전환되지 않는다. BPF 스케줄러가 계속 동작하고, 같은 노드의 다른 L1과 L2는 영향을 받지 않는다(L1 DMISS 0). cpuset을 바로잡은 시각부터 지연 식별 상한(TIM-006, T_bind) 안에 바인딩된다(측정 시작점은 정정 시각) |

---

## 11. 파일 변경 목록 및 구현 순서 (P1 WBS)

| 순서 | 작업 | 파일 | 의존 |
|:--|:--|:--|:--|
| O-1 | proto 필드 추가(§2): `schedinfo` TaskInfo 11, FaultInfo 6–8. `node_control` CbsConfig 9–10, FaultInfo 10–13(`job_seq`, `job_gen` 포함, 승인 범위에 따라 축소). 생성 코드 | `timpani-o/proto/*.proto`, Rust `timpani_rust/timpani-o/proto/schedinfo.proto`(필드만) | 승인 필요(§2, L-11), L-6 |
| O-2 | MIT, T, D 검증(`validate_l2_task`: 0 < T ≤ MIT, D ≤ MIT, C ≤ D), transition 규칙, task_id 중복 거부, 복원 | `schedinfo_service.{h,cpp}`, `global_scheduler.h` | O-1 |
| O-3 | 단일 수용식(`L_del_max`, `δ_ovr`), worst-fit의 C/T_s, `compute_tt_blocking`, min-gap 거부, 2-레이어 발행, table_id | `global_scheduler.{h,cpp}`, `publication_coordinator.cpp` | O-2 |
| O-4 | 폴트 유형 보존과 `count`, `flags` 전달 (`job_seq`, `job_gen`은 승인 시) | `orchestrator_service.cpp`, `fault_client.{h,cpp}` | O-1 |
| O-5 | GTest 추가(§3.5) | `timpani-o/tests/*` | O-2~4 |
| N-1 | BPF 단순화: `maps.h`(TaskMeta `bind_gen`, `run_seq`, JobRecord `t_sleep_ns`, 실행 구간 플래그 정리), `job_fsm.h`(MIT 검사와 L2 DMISS 제거), ops(`runnable`, `quiescent`, `exit_task`의 `bind_gen`) | `timpani-n/src/bpf/*` | — |
| N-2 | `libttsched` v2 재작성: 공개 헤더, `post`, `wait`, `done`, `open`, `init`, 내부 헤더 `libttsched_shm.h`(신규), 호스트 테스트(§6.5) | `sample-apps/src/libttsched.{h,c}`, `sample-apps/src/libttsched_shm.h`(신규), `sample-apps/tests/libttsched/`(신규) | 승인(L-9, API) |
| N-3 | TimerMaster SHM v2: 레이아웃과 static assert, 활성화(프로토콜 1), 비활성화와 닫기 규칙(프로토콜 5), 시작 정리(프로토콜 7), `update_evt_slots`(이름만) | `timer_master.{h,cpp}` | N-2(레이아웃 공유) |
| N-4 | TaskRegistry 재설계(`bind_gen`, stale 종료 무시, `bind_mu_`), main.cpp 분기(B0, C, E 단계) | `task_registry.{h,cpp}`, `main.cpp`, `bpf_loader.{h,cpp}` | N-1, N-3 |
| N-5 | DeadlineMonitor(신규): 주기 스캔, CAS 표시, 미스 기록 SPSC 큐, 슬롯별 BPF 키 표 | `deadline_monitor.{h,cpp}`(신규) | N-3 |
| N-6 | JobTraceCollector(Job 로그와 실행 구간), EventSourceMonitor(누적 계수), FaultMonitor(계수 경로와 상세 경로), NodeClient 필드 | `job_trace.{h,cpp}`, `event_source_monitor.{h,cpp}`, `fault_monitor.*`, `grpc/node_client.*` | N-1, N-3, N-5, O-1 |
| N-7 | Replenisher와 kick 백엔드: 변경 없음(§5.3). 테스트 유지 | `replenisher.{h,cpp}`, `bpf_loader.*` | N-0 |
| N-0 | PoC(OI-9): SYSCALL 프로그램의 `scx_bpf_kick_cpu`, 컨텍스트 반환, 유휴 CPU 승격 지연, TT 지터 영향 (기존 항목) | `timpani-n/src/bpf/*`(실험 브랜치) | — |
| N-8 | timpani-n GTest 타깃(§5.8, §5.9) | `timpani-n/tests/*`, `CMakeLists.txt` | N-1~6 |
| S-1 | `evttask.c`(신규), FCW 어댑터 축소(`cbs_queue`와 조건 변수 삭제), `libtimpani_rt` 폐기, yaml과 json의 MIT 필드 | `sample-apps/src/evttask.c`, `examples/someip/*`, `scripts/*` | N-2, O-1 |
| D-1 | 문서 정합: DDR-002 §7, DDR-005, DDR-007 §4.2 / §4.5(보충 의미) / §4.5.3 / §7, DDR-012, `structure.md`, README, CHANGELOG | `doc/**` | 전체 |
| T-1 | 통합 스크립트 `scripts/verify_event_driven.sh`: AC-008-01~12와 17~19 자동화 가능 항목, 13~16 절차 | `scripts/` | 전체 |

- **PoC 항목**: P-1 DeadlineMonitor 1 kHz 스캔의 비용과 지터를 타깃 CPU에서 측정(TT 부하 아래와 선택 CPU에서). 이벤트 기반 깨움이 합의된 상한에 필요한지 판단한다(F-10). P-2 post·wait·done의 원자 연산 비용과 $L_{post}$(post → runnable)를 측정한다. P-3 $\delta_{ovr}$를 nohz_full과 틱 재시작 지연 아래에서 측정한다. P-4 유휴 CPU에서 $L_{wake}$(TIM-001)와 $L_{del,max}$를 측정하고, TT 부하 아래의 깨움(TIM-002)은 $B_{tt}$ 항의 검증에 쓴다. N-0 PoC는 기존 항목을 유지한다.
- **제거된 작업 항목**: `ttsched_evt_notify` 구현과 테스트, 배치 count 대기, 앱 소유 큐와 invalidate·clear 로직, `TTSCHED_EVT_RESET` 처리, 마커 전환과 K/M 원천 전환, BPF의 MIT 검사와 `FAULT_MIT_VIOLATION` 발행, BPF의 L2 DMISS 판정과 조기 DMISS.
- 권장 병렬화: (O-1→O-2→O-3→O-5), (N-1 ∥ N-2) → N-3 → (N-4 ∥ N-5) → N-6 → N-8. S-1은 N-2와 O-1 뒤에 한다. 각 묶음은 별도 PR(`feat(timpani-o): ...`, `feat(timpani-n): ...`, `feat(sample-apps): ...`)로 나눈다.

---

## 12. 오픈 이슈 (LLD 수준)

| # | 이슈 | 영향 |
|:--|:--|:--|
| L-1 | `scx_bpf_kick_cpu`의 SYSCALL 프로그램 호출 가능성과 컨텍스트 반환(OI-9) — 커널 6.12.x 빌드에서 PoC. 함께 확인: 플래그 0 kick이 TT 실행 중인 CPU의 지터에 주는 영향, Replenisher 우선순위(TM−1) | §5.3 백엔드 선택 |
| L-2 | 실행 구간 첫 기록: `ops.runnable`이 `sched_setattr(SCHED_EXT)` 전환 시점에 호출되는지(전환 직후 첫 실행 구간의 관측 왜곡). 첫 실행 구간에 표시 플래그를 둘지 | 통계 정확도(실행 구간 기록) |
| L-3 | `trace_enabled`를 `.bss` 변수로 둘지 1-엔트리 ARRAY 맵으로 둘지(libbpf 스켈레톤 접근 편의) | 구현 편의 |
| L-4 | 핸들 해석 규칙 — **종결**: `ttsched_evt_t`는 task_id를 가리키며, 게시마다 슬롯 힌트를 이름으로 검증하고 어긋나면 이름으로 다시 찾는다. 슬롯 인덱스의 전역 캐시 무효화 규칙은 없다(§6.1, §6.2) | — |
| L-5 | `JobTraceCollector` CSV와 기존 `gpdata` 포맷 통합 여부. HLD-008 H-2가 담당하며 여기서는 추적하지 않는다 | 도구 체인 |
| L-6 | Rust proto(`timpani_rust/timpani-o/proto/schedinfo.proto`)에 필드만 동기화할지, Rust 포트 작업 시 일괄 반영할지 | Rust CI |
| L-7 | **철회**. 마커 wait point의 커널 식별(REQ-008 OI-17, DDR-008 F-8)은 명시적 Job 경계(`ttsched_evt_wait`와 `ttsched_evt_done`)로 대체되었다. `sched_yield` 후보와 마커 플래그는 폐지한다. 대체 근거: §6.1, DDR-008 D-18 | — |
| L-8 | 대기 중인 워커를 Event 없이 깨우는 수단. P1에는 없다. 프로세스 종료 때는 워커를 깨우지 않는다(HLD-008 §8 종료 절차). 전용 취소 API(깨움 워드만 올리고 `wait`가 E_INACTIVE 또는 취소 값을 돌려줌)는 P2에서 결정한다. 앱이 `ttsched_evt_post`로 가짜 Event를 만드는 방법은 쓰지 않는다 | 워커만 끝내는 앱의 종료 경로 |
| L-9 | **승인 필요 (API)**: libttsched v2 공개 API(§6.1)와 SHM v1 대비 ABI 변경(v1로 빌드된 앱은 다시 빌드해야 한다). 이전 초안의 `ttsched_evt_notify`, 배치 count, `TTSCHED_EVT_RESET`은 구현된 적이 없으며 이 개정에서 설계에서 뺐다. 영향: FCW, evttask, 생산자 | 앱 빌드 |
| L-10 | **승인 필요 (SHM v2)**: 객체 레이아웃과 상수(§5.6: 슬롯 2752 B, 객체 176192 B, MAX_TASKS 64, QDEPTH 8, PAYLOAD 64 B, JOB_LOG 32). 계수 타입(u64)과 Event 레코드 예약 공간 24 B | 공유 메모리 호환 |
| L-11 | **승인 필요 (protobuf)**: `schedinfo` TaskInfo 11(`min_inter_arrival_us`), FaultType DMISS·BUDGET_EXCEED·WATCHDOG(P2)·MIT_VIOLATION, FaultInfo 6–8. `node_control` CbsConfig 9–10, FaultInfo 10–13(`job_seq` = 12, `job_gen` = 13, u64). 미승인이면 보고는 카운트만 싣는다(flags, job_seq, job_gen 없음). 승인 범위는 D-11과 같이 정한다 | 프로토콜 호환 |
| L-12 | **승인 필요 (timpani-n 내부 API)**: `TaskRegistry::Binder`가 `bind_gen`을 돌려주고 `track`, `notify_exit`에 `bind_gen`이 들어간다. `TimerMaster::set_schedule_table`에서 EVT 인자를 뺀다. 새 TimerMaster 메서드 추가. 기존 공개 메서드의 이름은 바꾸지 않는다(§9 원칙) | 내부 API. 서명 확장으로 취급 |
| L-13 | **승인 필요 (수치 후보)**: `DL_SCAN_PERIOD` 1 ms, 감지 상한 2 ms(TIM-009), 보고 상한 200 ms(TIM-010), $L_{post}$ 50 µs, $L_{wake}$ 50 µs, $L_{del,max}$ 100 µs(TIM-011), $\delta_{ovr}$ 1 ms, CBS_ACT_TOL 50 µs, `tol_mit` 0, 10 ms back-off, DeadlineMonitor 우선순위 97(Replenisher 98), `MISS_Q_N` 256, `DETAIL_KEEP` 16. 모두 합의와 Linux 타깃 측정 전의 후보다 | 보장이 아닌 목표 |
| L-14 | 활성화 틈(sched_ext 전환과 슬롯 활성화 사이의 E_INACTIVE). 수락되지 않으므로 계수 불일치는 없다. 이 틈을 받아들일지 프로젝트 리더 확인이 필요하다 | 바인딩 직후 게시 |
| L-15 | **닫힘 (A2)**: 게시 (a)는 `kind`만 본다. TT 재발행이 `magic`을 토글해도 EVT 게시는 거절되지 않는다. 시작 정리는 `kind = NONE`으로 게시를 막는다. 객체 무효를 나타내는 별도 플래그는 두지 않는다. | 테이블 적용 중 게시(해결됨) |
| L-16 | FCW 워커의 프레임별 콘솔 출력 축소(로그 링은 추가하지 않는다, A20). 데모 출력 형식은 HLD-008 §8에서 정한다 | 데모 관측 |
| L-17 | 판정 해석 1 (확인 필요): 완료 시각 $t_{end}$는 `done()`이 종료 전이 직전에 읽는 시계 값이며, 기록된 $t_{end}$와 MISSED 플래그는 어긋나지 않는다. 게시 시점 판정이며 늦은 수집은 마감 실패가 아니다. 이 해석은 `REQ-008 PRT-009`와 §2.5(완료 시각 t_end와 경합 규칙)의 해석이다(REQ-008 OI-20). 완료 시각은 처리와 결과 게시의 완료이며 네트워크 전달이나 장치 반영의 완료가 아니다(A21) | 판정 정의(AC-008-14) |
| L-18 | 판정 해석 2 (확인 필요): TIMPANI가 마감 전에 닫은(CANCELLED, ABANDONED) Job은 마감 실패가 아니다. 마감 뒤에 닫으면 마감 실패이며 DETECTED_LATE다. 이는 amendment의 "마감 전 명시적 폐기·취소" 문장을 TIMPANI가 닫는 경우로 넓힌 해석이다 | 판정 정의(AC-008-14, AC-008-15) |
| L-19 | 열린 질문: 마감 실패 보고 대상이 DDR-012 임계 규칙의 보고로 한정되는지, 모든 마감 실패인지. 임계 규칙을 따르면 TIM-010은 보내진 보고에 대해서만 정의된다 | 보고 범위와 AC-008-14 |
| L-20 | 열린 질문: TIMPANI-O가 `job_seq`, `job_gen`을 Pullpiri의 `schedinfo.FaultInfo`로 전달할지. 현재 `schedinfo`에는 Job 식별자가 없다 | Pullpiri 호환 |
| L-21 | 열린 질문: DeadlineMonitor와 Replenisher 스레드를 합칠지(F-12). 현재는 분리한다 | 스케줄링 설계, RT 비용 |
| L-22 | 열린 질문: TMG-011을 게시·대기·완료 비용 항목으로 쓸지(§8 현재 용법), REQ 쪽의 정의와 맞춘다 | 추적성 |
| L-23 | 열린 질문: FCW 생산자 측의 TT 등록(`timpani_tt_register`) 대체 방법은 HLD-008 §8과 함께 확정한다 | 샘플 앱 |
| L-24 | C 측정 절차와 여유율(HLD-008 H-19, REQ-008 OI-23): Job당 CPU 수요를 주는 레코드와 evttask에 필요한 옵션(§7.1) | 수용식 입력 C의 근거 |
| L-25 | 재바인딩 경계의 재게시에서 `t_arr` 기준과 중복 처리 책임(HLD-008 H-20, REQ-008 OI-24) | 어댑터 계약, 마감 기준 |
