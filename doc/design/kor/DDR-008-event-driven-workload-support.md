<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-008: Event-Driven Workload Support — 설계 결정 기록

**날짜:** 2026-11-11
**최종 수정:** 2026-11-13 (Weekly 2026-11-12 범위·의존성 반영)
**상태:** Draft (Proposal, Phase 1 결정 확정 대기)
**버전:** v3
**작성:** Human + AI
**관련 요구사항:** [REQ-008](REQ-008-event-driven-workload-support.md), [SRS-008](SRS-008-event-driven-workload-support.md), SRS-000 `REQ-FUN-004/009/013/014`, `REQ-NFR-003/004`, `REQ-ED-001`
**P1 계약 보강:** [P1-008](P1-008-event-job-contract-amendment.md), [기능 지원 Issue #53](https://github.com/eclipse-timpani/timpani26/issues/53)
**관련 DDR:** [DDR-001](DDR-001-workload-model.md), [DDR-002](DDR-002-scheduling-architecture.md), [DDR-003](DDR-003-interface-protocol.md), [DDR-004](DDR-004-resource-allocation.md), [DDR-005](DDR-005-sched-ext-bpf-scheduler.md), [DDR-006](DDR-006-communication-architecture.md), [DDR-007](DDR-007-tt-cbs-integrated-scheduling.md), [DDR-011](DDR-011-runtime-table-update.md), [DDR-012](DDR-012-fault-management.md)
**하위 문서:** HLD-008, LLD-008

> 본 문서는 SRS-000이 참조하던 `DDR-008`의 **정본**이다. 과거 검토 초안(저장소 미반영, SOME/IP 특정)은 폐기하며, PR #50~#52가 인용한 "DDR-008 §2.3 / LLD §1.3 / FR-041"은 본 문서로 대체된다.
> 각 결정은 **D-NN** 번호를 가지며, Phase(P1/P2)와 대안·결과를 기록한다. HLD/LLD는 결정을 재논증하지 않고 D-NN을 참조한다.
> 철회된 항목(F-3, F-8)은 번호를 유지한 채 대체 항목을 가리킨다. 번호는 재사용하지 않는다.
> 이 문서의 모든 설계는 검토용 제안이다. 구현되었거나 Linux 타깃에서 검증된 내용이 아니다. 공개 API, SHM 레이아웃, protobuf 필드, 수치 상한은 프로젝트 리더의 승인이 필요하다.

---

## 요약 (Korean Summary)

- TIMPANI는 REQ-008 §2의 **공통 Event/Job 모델**(Event source·Event·Job, 생명주기 상태와 결과, 규칙 R1~R4, 불변식 I1)에 대응하도록 설계하며, **모델 / 연결·관측 / 집행**의 3층으로 구성한다. 모델 용어는 OS·스케줄러·라이브러리 메커니즘과 독립적으로 정의한다(D-01).
- **한 Event – 한 Job**: 앱의 입력 어댑터가 명시적으로 게시한 Event 한 건마다 독립된 Job 한 건을 만든다. 워커는 `ttsched_evt_wait()`로 Event 한 건을 받아 Job을 시작하고 `ttsched_evt_done()`으로 반드시 끝낸다. 배치 소비(대기 건수 n 처리)와 다음 wait에서의 암묵 완료는 쓰지 않는다(D-18).
- **명시적 Event 연결**(libttsched v2 + SHM v2): 고정 크기 Event 레코드 링과 인라인 payload를 둔다(후보: 8건, 64 B). 수락 시 식별자 `(gen, seq)`와 절대 마감을 고정하며, 거절·넘침·취소·재등록·재시작의 의미를 정의한다. 게시 주체는 source당 하나로 제한한다(D-03, D-19).
- **마감은 도달 시각 + D**(절대 마감)이다. TIMPANI-N의 DeadlineMonitor(신규)가 완료를 기다리지 않고 큐 대기·실행·선점·스로틀·CPU 유휴 중에도 기한 경과를 감지하며, Job당 한 번만 계수한다. 완료 시각의 정의와 감시와의 경합 규칙을 둔다(D-20).
- BPF는 **실행 구간**(커널 활성화부터 자발적 수면까지)을 관측하고 CBS 예산 집행, CPU 고정, TT 보호만 맡는다. BPF는 Job을 정의하지 않으며 MIT 검사와 L2 DMISS 판정을 하지 않는다(D-02).
- CBS 보충은 **활성화 기준 보충 윈도우**다(D-04). 성질 P-a는 `T_s ≤ MIT`, `D ≤ MIT`와 블로킹 없음 등 전제를 명시한다. MIT 위반은 도달 기준으로 계수·보고하며 Event를 거절하지 않는다(D-05). 예산은 Job이 아니라 윈도우 단위로 준다.
- 승인은 이용률 + **FIFO 블로킹 합 충분조건**이며, 도달부터 첫 디스패치까지의 지연 `L_del_max`와 틱 overrun `δ_ovr`을 포함한다. 모든 L2 태스크에 한 식을 쓴다. Event를 게시하지 않는 L2 태스크는 예산만 집행받는 class이며 Event 단위 마감 지원과 동등하다고 설명하지 않는다(D-09, D-21).
- 폴트: DMISS는 DeadlineMonitor·워커 종료·정리 경로에서, BUDGET_EXCEED는 BPF에서, MIT_VIOLATION은 슬롯 계수에서 나온다. 보고는 손실 없는 계수 경로와 최선 노력 상세 경로로 나뉜다(D-10). proto 변경은 필드 추가만이며 모든 변경은 명시 승인 대상이다(D-11).
- P1 한계: 블로킹하는 Job은 마감이 감시되지만 admission이 보장하지 않는다. 게시자가 죽는 경우 `accepted` 계수 1건이 빠질 수 있다. 데몬이 다운된 동안에는 마감 감지가 없다. 수치는 후보값이며 합의·측정이 필요하다.
- 상태: Draft. 수치 후보값, 공개 API·SHM·proto 변경, DeadlineMonitor PoC는 승인·측정 전이며 구현되었거나 검증되지 않았다.

---

## 1. 컨텍스트 (Context)

### 1.1 문제
REQ-008 §1.3/§10이 정리한 바와 같이, `timpani26`에는 L2(CBS) 경로가 end-to-end로 존재하지만 Event-Driven 워크로드의 **동작 모델**(Event·Job 정의, 완료·마감 기준, 보충 결정론, CPU 고정, 승인 충분조건, 폴트 분류)이 미정의이거나 결정론 요구를 만족하지 못한다. 또한 선행 PoC(`examples/someip/libtimpani_rt`)가 SOME/IP를 코어 계약에 끌어들였다.

2026-11-10 초안(커밋 `9209bc5`)은 커널 수면 경계로 Job을 정의하고, wait가 대기 건수 n을 돌려주는 배치 소비와 활성화 시각 기준 마감을 썼다. 이 구조에서는 처리 중 도달한 Event와 완료되지 않은 기한 초과를 Job 단위로 구분해 감지할 수 없다. P1 계약 보강(P1-008, Issue #53)은 Event 한 건을 Job 한 건으로 만들고, 마감을 도달 시각 기준으로 고정하며, 완료를 기다리지 않는 감지를 요구한다. 본 문서는 이 합의를 반영한다.

### 1.2 설계 세력 (Forces)

| 세력 | 설명 | 출처 |
|:--|:--|:--|
| **결정론** | 모든 타이밍 주장은 측정 가능한 바운드여야 한다 | SRS-000 REQ-NFR-001, REQ-008 TIM |
| **FFI** | L2의 폭주·초과가 L1·타 L2로 전파되면 안 된다 | SRS-000 REQ-NFR-002 |
| **투명성** | 명시적 연결을 쓰지 않는 앱은 표준 블로킹을 그대로 쓸 수 있어야 한다(예산 집행만 적용) | SRS-000 REQ-NFR-003, REQ-008 DET-003 |
| **전송 계층 중립** | 코어에 미들웨어 의미론 금지 | REQ-008 MOD-006 |
| **Event–Job 1:1 식별** | 수락한 Event마다 독립된 Job과 식별자가 있어야 한다 | P1-008 §1 |
| **완료 무관 마감 감지** | 큐 대기·실행·선점·스로틀·CPU 유휴 중에도 기한 경과를 감지해야 한다 | P1-008 §4 |
| **데이터–식별 일관성** | 게시·통지·소비·재시작에서 데이터와 식별·시각이 어긋나면 안 된다 | P1-008 §1 |
| **단순성(P1)** | 현실적·단순한 기본 기능으로 FCW 샘플 완성 | REQ-008 §0.1 |
| **기존 결정 존중** | DDR-001~007·011·012의 모델·메커니즘 유지, 필요한 최소 수정만 | 본 DDR §6 |
| **RT 경로 순수성** | BPF/RT 스레드에 gRPC·할당·블로킹 금지 | DDR-006 §4 |
| **커널 제약** | BPF에서 futex wake 불가, `bpf_timer`는 PREEMPT_RT에서 부적합, 슬라이스 만료는 틱 의존 | DDR-007 §4.5.1, REQ-008 C-01/02 |
| **proto 거버넌스** | 필드 번호 유지·추가만, 명시 승인 | `.github/copilot-instructions.md` |

---

## 2. 결정 (Decisions)

### D-01. 공통 Event/Job 모델과 3층 구조(전송 계층 중립) — P1

**결정**
1. **모델을 먼저 고정한다.** REQ-008 §2의 공통 모델을 TIMPANI의 유일한 기준으로 삼는다. 모델 용어는 OS·스케줄러·라이브러리와 독립이다.
   - **Event source S**: 선언된 발생원. P1에서는 L2 태스크 하나당 하나이며 `task_id`로 식별한다. 최소 도달 간격 MIT는 `TaskInfo.min_inter_arrival_us`이고, 없으면 주기 $T$를 쓴다(D-11).
   - **Event e**: 앱의 입력 어댑터 경계에서 수락된 도달 한 건. 식별자 `(gen, seq)`, 도달 시각 $t_{arr}$, 절대 마감 $t_{dl} = t_{arr} + D$(수락 시 한 번 계산하고 이후 바꾸지 않는다), 불투명 payload.
   - **Job j(e)**: Event 한 건을 처리하는 작업. 배치와 병합이 없다. Job과 수락된 Event는 1:1이다(I1).
   - **생명주기 상태**: `QUEUED → RUNNING → COMPLETED | DISCARDED`, `QUEUED → CANCELLED`, `RUNNING → ABANDONED`. 종료 상태는 최종이다. COMPLETED만 성공 종료다. DISCARDED는 워커의 정책적 포기이며 성공이 아니다. CANCELLED와 ABANDONED는 TIMPANI가 닫은 결과다.
   - **마감 결과**(`MISSED`)는 생명주기와 별도의 플래그다. Job당 최대 한 번 설정되며 지워지지 않는다(D-20).
   - **시각**: $t_{arr}$(도달), $t_{dl}$(절대 마감), $t_{begin}$(워커가 Event를 받아 Job을 시작한 시각), $t_{end}$(워커가 종료 전이를 게시하기 직전에 읽은 시각). TIMPANI가 닫은 Job은 $t_{end}$ 대신 닫힌 시각을 가진다. 응답 시간은 $R = t_{end} - t_{arr}$이며, 기한 충족은 COMPLETED이고 $t_{end} \le t_{dl}$인 경우다.
   - **규칙**: R1 대기 중 도달은 그 Job을 즉시 실행 가능하게 만든다. R2 처리 중 도달은 QUEUED로 남으며 실행 중인 Job에 합치지 않는다. R3 Job이 끝났고 QUEUED가 있으면 워커는 잠들지 않고 가장 오래된 Event로 다음 Job을 시작한다(연속 Job은 별개 Job이다). R4 Job은 Event 한 건만 소비하며 source 안에서 FIFO다.
   - **불변식 I1**: 수락된 Event는 정확히 한 개의 종료 상태에 도달한다. Job과 수락된 Event는 1:1이다.
   - **거절된 게시는 Event가 아니다.** E_INACTIVE, E_FULL, E_INVAL은 아무것도 만들지 않는다. E_FULL은 넘침으로 계수한다.
2. **연결·관측 계층은 두 경로로 나눈다.** (①) **명시적 Event 연결**: libttsched 공유 큐와 SHM 슬롯. Event, Job, 마감을 제공하는 유일한 경로다. (②) **커널 실행 구간 관측**: BPF. 모든 L2 태스크의 실행 구간과 예산을 관측하지만 Event를 제공하지 않는다. 커널 관측은 Event를 얻는 수단이 아니므로 두 어댑터를 같은 자격의 대안으로 보지 않는다(P1-008 §1).
3. **집행·판정 계층.** CBS 예산, CPU 고정, TT 보호는 BPF가 맡는다. 마감 감시와 폴트 보고는 TIMPANI-N(DeadlineMonitor, FaultMonitor)이 맡는다. 승인은 TIMPANI-O가 맡는다.
4. **전송 계층 중립.** 코어(O/N/BPF/libttsched)의 proto·API·자료구조는 미들웨어 식별자와 미들웨어 헤더를 포함하지 않는다. 미들웨어 수신을 Event 게시로 바꾸는 입력 어댑터는 앱 쪽에 둔다. TIMPANI는 임의의 `recv()`, `read()`, 커널 깨움에서 메시지 의미를 추론하지 않는다.
5. **P1 제한.** Event source는 L2 태스크당 1개이며 암묵적으로 선언된다. proto에 Event source 메시지를 추가하지 않는다. `task_id`는 15바이트 이하이고 같은 노드 안에서 유일해야 하며 TIMPANI-O가 검증한다(D-09 (4)). 태스크당 다중 source는 P2다(F-7).

**근거**: 9209bc5 초안은 Event를 커널 wakeup으로, Job 경계를 수면으로 정의했다. 그래서 처리 중 도달, 배치 소비, Job 단위 마감 판정이 표현되지 않았다. 모델을 구현 수단과 분리하면 (a) 도달과 처리를 따로 계수하고 식별할 수 있고, (b) 커널 관측의 한계를 모델 위반이 아니라 관측 범위의 한계로 기술할 수 있으며, (c) 집행 코드를 바꾸지 않고 연결 계층을 교체할 수 있다. 명시적 연결 없이 커널 관측만 있는 태스크는 예산 집행만 받으며, Event 단위 마감 지원과 동등하다고 말하지 않는다(D-21).

**대안**
| 대안 | 장점 | 단점 | 판정 |
|:--|:--|:--|:--|
| A. 코어가 특정 미들웨어 계약을 보유(선행 PoC) | FCW에 즉시 맞춤 | 미들웨어마다 코어 수정; TIMPANI가 메시지 의미를 알게 됨 | 기각 |
| B. TIMPANI-N에 미들웨어 플러그인 계층 | 전송별 관측 정밀 | N에 미들웨어 의존성과 RT 경로 오염 위험, P1 범위 초과 | P2 검토(관측 전용 플러그인) |
| C. 공통 모델 + 명시적 연결 + 커널 실행 구간 관측 + 앱 측 입력 어댑터 | 코어 단순, 식별과 마감이 명시적, 인증 범위 최소 | 앱이 입력 어댑터를 구현해야 함 | **채택** |
| D. Event ≡ 커널 wakeup, Job ≡ 수면 사이 구간 (9209bc5) | 구현이 가장 작음 | Event와 Job을 구분할 수 없음. 처리 중 도달과 배치 소비를 표현할 수 없음. 마감 판정이 완료에 묶임 | 기각 |
| E. 커널 관측만으로 얻은 원천을 Event와 동등한 어댑터로 취급 (9209bc5 어댑터 K) | 표준 블로킹 앱을 그대로 지원 | 식별자와 도달 시각이 없고 연속 Job이 병합됨 | 기각. 예산만 집행하는 class로 격하(D-21) |

**결과**: `libtimpani_rt`, `timpani_evt_register`, 자체 SHM `/timpani_rt`와 라이브러리 측 MIT·E2E 판정은 폐기 대상이다(REQ-008 §12). 샘플 앱의 입력 어댑터는 미들웨어 콜백에서 `ttsched_evt_post()`를 호출하는 얇은 계층으로 줄어든다. 워크로드 선언(proto)은 MIT 필드 외에 바뀌지 않는다. 큐 깊이와 payload 상한은 P1 빌드 상수다(D-19, F-11).

---

### D-02. 커널 관측: BPF 실행 구간 관측기 — P1

**결정**: BPF는 태스크별 **실행 구간**(execution interval) 상태 머신을 유지한다. 실행 구간은 커널 활성화(수면에서 깨어나 실행 가능 상태가 됨)에서 시작해 첫 실행을 거쳐 자발적 수면에서 끝난다. **실행 구간은 Job이 아니다.** Job 경계는 워커의 명시적 호출(D-03)로만 정해진다. 코드 식별자에는 `job_` 접두를 유지할 수 있으나 그 의미는 실행 구간이다.

상태는 {SLEEP, ACTIVE}다. TaskMeta에 `run_seq`, `t_act`, `t_start`, `bind_gen`과 플래그를 둔다. 전이는 다음과 같다.

```text
SLEEP  --(ops.runnable: 수면에서 깨어남)--> ACTIVE
          · t_act = now, run_seq++, 보충 윈도우 검사(D-04 규칙 1), 활성화 기록 1회
ACTIVE --(ops.enqueue: 선점·슬라이스 만료 후 재삽입)--> ACTIVE (t_act 불변)
ACTIVE --(ops.stopping: 실행 중단)--> ACTIVE
          · 예산 차감. 예산이 양수에서 0으로 바뀌는 시점에만 BUDGET_EXCEED 1회(D-10)
ACTIVE --(ops.quiescent: SCX_DEQ_SLEEP, 자발적 수면)--> SLEEP
          · 실행 구간 레코드 발행 (run_seq, t_act, t_start, t_sleep, budget_used_us, flags)
```

- 활성화의 유일한 계기는 SLEEP 상태에서의 `ops.runnable`이다. `ops.enqueue`는 재삽입 경로에서도 호출되므로 활성화 판정에 쓰지 않는다. 수면이 아닌 dequeue(속성 변경 등)는 실행 구간을 닫지 않는다.
- **블로킹과 연속 Job.** Job 중 블로킹하면 깨어날 때 커널 활성화가 새 실행 구간으로 기록된다. 그 활성화는 열린 보충 윈도우 안에서 처리되므로 새 예산을 받지 않는다(D-04). Job 경계는 바뀌지 않는다. 블로킹은 허용되고 관측되며 응답 시간에 포함된다. 다만 admission의 보장은 블로킹하지 않는 Job에 한정된다(D-09 가정 (4)). 수면 없이 이어지는 연속 Job은 실행 구간이 이어지고, Job 경계는 워커의 `done`과 `wait`가 정한다.
- **P1 BPF가 하지 않는 일**: Job 정의, activation 기준 MIT 검사와 `MIT_VIOLATION` 발행, L2 DMISS 판정과 조기 DMISS, 수면 경계의 시스템 콜 분류. 마감 감시는 TIMPANI-N(D-20)이 맡는다. TT 슬롯의 DMISS 경로(DDR-012)는 변경하지 않는다.

**근거**: 커널은 Job 경계를 알 수 없다. 연속 Job, 처리 중 도달, 블로킹은 모두 커널에서 같은 수면·깨움 패턴으로 보인다. 그러나 예산 집행에 필요한 것은 실행 구간이다. 그러므로 BPF는 실행 구간만 관측하고, Job은 워커가 명시적으로 정의한다.

**대안**
| 대안 | 장점 | 단점 | 판정 |
|:--|:--|:--|:--|
| (i) 매 wakeup = 활성화 = Job (9209bc5) | 구현 단순 | 연속 Job과 블로킹이 잘못 나뉘거나 병합됨 | 기각 |
| (ii) 사용자 공간 모니터가 공유 메모리만으로 판정 | 커널 변경 없음 | BPF 집행과 괴리, 마커 없는 앱 판정 불가 | 기각 |
| (iii) uprobe/USDT로 라이브러리 함수에 훅 | 커널 변경 없음 | 컨테이너별 attach 운영 부담, 호출당 비용 | P2 후보로 보류 |
| (iv) 수면 경계를 시스템 콜 분류 또는 `sched_yield()` 표식으로 식별 (9209bc5 P2 계획, 이전 F-8) | 커널에서 경계를 식별 | 처리 중 도달과 연속 Job을 구분하지 못함. 앱 의미를 커널이 추론하게 됨 | 철회. 명시적 Job 경계(D-18)로 대체 |

**결과**: `select_cpu()`에서 매 wakeup마다 `activation_ns`를 덮어쓰는 코드(`timpani.bpf.c:256`)를 제거하고 상태 머신으로 대체한다. CBS 예산은 실행 구간 단위로 집행하며 Job마다 새로 주지 않는다(D-04). 실행 구간 레코드는 D-16에서 다룬다.

---

### D-03. 명시적 Event 연결: libttsched v2 API와 SHM v2 슬롯 — P1 (공개 API·SHM 변경 제안, 프로젝트 리더 승인 필요)

**결정**: 기존 `libttsched`(SHM 객체, 태스크 슬롯, futex)를 v2로 확장한다. 앱은 `ttsched_evt_open`, `ttsched_evt_post`, `ttsched_evt_wait`, `ttsched_evt_done`으로 Event 단위 연결을 한다. 아래 시그니처는 **제안이며 승인 대상**이다. 세부 구조와 오류 코드는 LLD-008이 정의한다.

```c
ttsched_evt_t ttsched_evt_open(const char *task_id);          /* 초기화 경로, 힙 없음 */
int ttsched_evt_post(ttsched_evt_t src, uint64_t t_arr_ns,     /* 입력 어댑터가 호출 */
                     const void *payload, uint32_t len,
                     struct ttsched_evt_id *id_out);
int ttsched_evt_wait(struct ttsched_evt_event *ev);             /* 워커: Event 한 건, Job 시작 */
int ttsched_evt_done(int outcome);                              /* 워커: Job 종료, 필수 */
```

- **open**: 프로세스 안의 고정 테이블에 핸들을 둔다. 핸들은 `task_id`를 가리키며 `ttsched_init()` 뒤에도 유효하다. 매핑은 초기화 경로에서 한다.
- **post**: 0은 수락이다. Event가 존재하며 식별자 `(gen, seq)`와 마감 $t_{arr} + D$를 가지고 감시 대상이 된다. 음수는 아무것도 만들지 않았다는 뜻이다. `t_arr_ns == 0`이면 호출 시점의 CLOCK_MONOTONIC을 쓴다. 값을 주면 CLOCK_MONOTONIC이어야 하며, 미래 시각이거나 source의 직전 도달보다 빠르면 E_INVAL이다. 현재보다 이른 값은 그대로 수락되며, 그 지연은 응답 시간에 포함되고 admission에는 포함되지 않는다.
- **wait**: 열린 Job이 있으면 E_STATE다. Event 한 건을 받아 Job을 시작한다. E_INACTIVE이면 짧은 back-off(후보 10 ms) 뒤 `ttsched_init()`을 호출하고 재시도한다.
- **done**: Job마다 필수다. outcome은 COMPLETED 또는 DISCARDED다. **암묵 완료는 없다.** `done`은 먼저 `word`를 읽어 열린 Job의 `seq`와 상태를 확인한다. `seq`가 다르거나 TIMPANI가 이미 Job을 닫았으면(ABANDONED) E_INACTIVE를 돌려주며 열린 플래그는 지운다(A6).
- **워커 루프**: `for (;;) { if (ttsched_evt_wait(&ev) < 0) { ttsched_init(); continue; } process(&ev); ttsched_evt_done(TTSCHED_EVT_COMPLETED); }`
- **스레드 규칙**: source마다 wait와 done을 호출하는 스레드는 하나다. 그 스레드의 이름이 `task_id`(1~15바이트)다. 두 번째 스레드가 호출하면 계약 위반이다. 워커의 열린 Job 상태와 Event 사본은 스레드 지역 저장소에 둔다.
- **게시 주체**: source마다 게시 주체는 한 번에 하나다(D-19). 게시는 워커 프로세스의 스레드이거나 SHM 쓰기 권한이 있는 같은 호스트의 다른 프로세스일 수 있다(OI-15 결정 유지).
- **비용 상한**: post, 비블로킹 wait, done은 원자 연산 수가 상한을 가지며(개수는 LLD-008 §6이 준다. 측정 전까지 후보), memcpy는 payload 상한 이하이고, futex 시스템 콜은 최대 한 번이다. 힙, 락, 블로킹은 없다. 매핑과 재매핑은 `ttsched_evt_open()`과 `ttsched_init()`이 비RT 경로에서 한다. 매핑을 쓸 수 없으면 post와 wait는 E_INACTIVE를 돌려주며 스스로 재매핑하지 않는다.
- **신뢰 범위(P1)**: SHM 쓰기가 가능한 모든 프로세스는 신뢰 대상이다. 잘못된 쓰기는 어떤 슬롯도 망가뜨릴 수 있다. 권한은 9209bc5의 결정대로 `fchmod` 0666을 유지한다. 객체 단위 격리는 P2다(F-14).

**SHM v2 슬롯 (제안, 승인 필요)**
- 헤더는 `{magic, version, n_tasks, generation, instance_id, reserved}`를 유지한다. `version`과 `instance_id`를 추가하고 `magic` 값을 갱신한다(ABI 단절 명시). `TIMPANI_MAX_TASKS`를 32에서 64로 늘린다(후보; LLD가 크기와 정렬, 객체 상한을 확정한다).
- 슬롯 고정부는 `name`, `counter`(대기·깨움용 futex 워드), `kind`(NONE, TT, EVT), 설정 `mit_us`, `deadline_us`, `gen`, 링 인덱스 `head`와 `tail`, 도달 기록 `last_arr_ns`, `last_viol_ns`를 가진다. `gen`은 u64이며, 상위 32비트는 `instance_id`에서, 하위 32비트는 활성화마다 증가하는 바인딩 횟수에서 얻는다.
- **누적 카운터(필수 통계, 트레이스와 무관)**: `arrivals`, `accepted`, `overflow`, `revoked`, `started`, `completed`, `discarded`, `cancelled`, `abandoned`, `missed`, `mit_viol`, 그리고 워커만 쓰는 `resp_max_ns`. 원자 가산을 쓰며 객체 수명 동안 누적된다. TIMPANI-N은 활성화 시점에 기준값을 잡는다.
- `counter`는 대기자를 깨우기 위한 워드다. Event 계수와 분리한다. 비활성화나 데몬 종료에서 깨움을 위해 `counter`를 바꾸어도 Event 계수가 바뀌지 않게 하기 위해서다.
- 슬롯 규약: 한 번 배정된 `task_id`는 객체 수명 동안 같은 슬롯을 쓴다. 비활성화는 `kind = NONE`이며 이름과 누적 값은 유지한다. 재활성화하면 같은 슬롯을 다시 쓴다. `kind`, `gen`, 레코드 `word`, `head`, `tail`, `counter`는 순차 일관(sequentially consistent) 원자 연산으로 읽고 쓴다. TT 슬롯은 기존 `magic`-last와 `generation` 핸드셰이크를 그대로 쓴다.
- 쓰기 주체 분담: 게시자는 레코드 필드, 레코드 `word` 게시, `tail`, 도달 계수, `counter` 증가와 깨움을 쓴다. 워커는 QUEUED에서 RUNNING으로의 전이, `t_begin`, 종료 전이와 `t_end`, 종료 계수, Job log를 쓴다. DeadlineMonitor는 MISSED 플래그(CAS)와 `missed`를 쓴다. TimerMaster는 `kind`, `name`, 설정, `gen`, 활성화 시 `last_arr_ns` 초기화, TIMPANI가 닫는 전이와 해당 계수를 쓴다. 상세 인터리빙 논증은 LLD-008이 맡는다.

**대안**
| 대안 | 장점 | 단점 | 판정 |
|:--|:--|:--|:--|
| (i) 앱 소유 큐 + 배치 개수 n + invalidate/clear + `TTSCHED_EVT_RESET` + SpscRing (9209bc5) | 자료구조가 익숙함 | 통지 SHM과 앱 큐를 함께 맞춰야 함. n건 처리 계약. 재시작마다 RESET 프로토콜 필요 | 기각 |
| (ii) `ttsched_evt_notify(src, t_arr)`만 호출하고 payload는 앱이 관리 | 호출이 짧음 | 통지와 payload가 어긋날 수 있음. 식별자가 없음 | 기각 |
| (iii) 암묵 완료: 다음 wait에서 이전 Job을 완료로 처리 | 앱 코드가 단순 | 실패한 Job을 완료로 기록할 수 있음 | 기각. `done` 필수 |
| (iv) 앱이 pinned BPF 맵을 직접 갱신 (DDR-008 초안 대안) | 커널 경로 재사용 | 권한·마운트 의존 | 기각(기존 결정 유지) |
| (v) 별도 라이브러리 `libttevent` | 역할 분리 | ABI와 SHM 이중화 | 기각 |
| (vi) eventfd 기반 통지 | 표준 fd | fd 전달 필요, futex보다 지연 큼 | 기각 |
| (vii) 이름 기반 signal과 반환값 없는 wait (이전 초안) | 호출 단순 | 처리 중 도달을 표현하지 못함. 호출마다 이름 탐색 | 기각 |

**결과**: `libttsched.h`의 소비자(timpani-n, sample-apps, 외부 TT 앱)는 재컴파일이 필요하다(ABI 단절, DDR-011 §1.1과 같은 유형). `timpani_rt_shm.h`와 `/timpani_rt`는 폐기한다. OI-15의 결정(게시 주체 범위)은 유지하며, 쓰기 권한 축소와 프로세스 간 정책은 P2다.

---

### D-04. CBS 보충 의미론: 활성화 기준 보충 윈도우 + 백업 보충 필수 — P1

**결정**: 서버 $s$는 잔여 예산 `remaining`과 현재 보충 윈도우의 끝 `replenish_at`을 가진다. 현행 격자 정렬 지연 보충(`replenish_at_ns += k·T_s`)을 버리고 다음 규칙을 쓴다. $T_s$는 서버의 선언된 CBS 주기 `period`다. 도달 간격의 MIT(`min_inter_arrival_us`, 없으면 주기)와 같은 값이 아니다. 성질 (P-a)는 $T_s \le MIT$를 전제로 한다(A15).

1. **활성화 시**(SLEEP→ACTIVE, D-02): $t_{act} + tol \ge replenish\_at$이면 `remaining = C_s`, `replenish_at = t_act + T_s`로 바꾼다. 그렇지 않으면(직전 윈도우가 아직 끝나지 않음) 예산과 윈도우를 바꾸지 않고, 실행 구간에 **예산 이월 표식**(`CARRYOVER`)을 남긴다.
2. **소진 시**: `remaining`이 0이 되면 Throttled가 되고 `BUDGET_EXCEED`를 1회 낸다(D-10).
3. **승격 시**: Throttled 서버는 $now \ge replenish\_at$이 되면 배정 CPU의 dispatch 경로에서 `remaining = C_s`, `replenish_at = now + T_s`로 갱신되고 L2 큐의 **뒤**로 들어간다(D-08).
4. 활성 상태이고 Throttled가 아닌 서버는 윈도우가 끝나도 예산을 다시 채우지 않는다. 다음 보충은 규칙 1 또는 3에서만 일어난다.
5. `tol`(`CBS_ACT_TOL_US`)은 **규칙 1의 허용 오차**만을 뜻한다. 깨움 지연의 변동 때문에 연속된 두 활성화의 간격이 조금 짧게 관측되는 경우를 흡수해야 하므로 활성화 지터의 상한 이상이어야 한다(후보 50 µs = `L_post`, 측정 필요). `tol`은 도달 기준 MIT 검사와 공유하지 않으며, 승인식의 $L_{wake}^{max}$와도 묶지 않는다. 도달 기준 MIT 검사는 자체 허용 오차 `tol_mit`(후보 0; 도달 시각은 깨움 지터를 받지 않는다)을 쓴다(D-05).
6. 서버가 동작하는 동안 `remaining`, `replenish_at`, Throttled 상태는 **BPF만**, 배정 CPU의 런큐 잠금 아래에서 쓴다. 사용자 공간은 서버를 만들 때(태스크가 `SCHED_EXT`에 들어오기 전)만 초기값을 쓰고 그 뒤로는 읽기만 한다(D-07, D-17).

**예산은 Job이 아니라 윈도우에 붙는다.** 윈도우는 규칙 1(윈도우가 끝난 뒤의 활성화)과 규칙 3(승격)에서만 열린다. 새 Job은 그 자체로 예산을 주지 않는다. 열린 윈도우 안에서 시작한 Job(연속 Job, 블로킹 뒤 재개)은 남은 예산을 나눠 쓴다.

DDR-007 §4.5.3의 **Replenisher 스레드(Backup)를 필수 구성요소로 격상**한다(D-07). 배정 CPU가 유휴일 때 규칙 3이 제때 실행되게 하는 것이 그 역할이다.

**성질**
- **(P-a) 선언을 지키는 서버는 스로틀되지 않는다.** 다음 전제가 모두 성립하면 규칙 1이 매 Job에 온전한 $C_s$를 준다.
  (i) 모든 도달이 MIT를 지키고 $T_s \le MIT$이다. (ii) $D \le MIT$이다. (iii) Job당 차감 실행 시간(활성화부터 워커가 `ttsched_evt_wait()`에서 잠들 때까지)이 $C_s$ 미만이다. (iv) Job이 블로킹하지 않는다. (v) 각 Job이 다음 도달 전에 끝나 워커가 잠들어 있다.
  이때 직전 윈도우는 $t_{prev} + T_s$에 끝나고, 다음 활성화는 $t_{act} \ge t_{prev} + T_s - tol$이다. 귀납적으로 서버는 스로틀되지 않는다.
  **경계 조건**: Job이 너무 늦게 끝나 다음 도달 전에 워커가 아직 잠들지 못하면(응답 시간이 MIT에 가까울 때만 가능), 다음 Job은 커널 활성화 없이 시작되어 남은 윈도우 예산으로 실행된다. 이 윈도우는 직전 활성화 지연 이내에 끝나고, 그 뒤 승격이 예산을 다시 채운다. 추가 지연은 작고 상한이 있으나 `BUDGET_EXCEED`가 날 수 있다. 그래서 $D$와 MIT 사이에 여유를 둘 것을 권고한다.
- **(P-b) 윈도우당 소비 상한.** 한 보충 윈도우 안에서 서버의 소비는 $C_s$(+ 틱 해상도에 의한 overrun, SRS-008 TMG-004) 이하다. 윈도우의 시작 간격은 $T_s - tol$ 이상이므로 길이 $L$인 임의 구간의 소비는 $(\lceil L/(T_s - tol) \rceil + 1) \cdot C_s$ 이하다. 이 상한은 서버가 위반하든 아니든 성립한다.
- **(P-c) 다른 서버가 앞서 실행하는 양은 서버당 $C_j$ 이하.** Job $i$의 활성화 시점에 서버 $j$가 큐에서 앞서 있거나 실행 중이면 $j$의 잔여 예산은 $C_j$ 이하다. $j$의 예산이 다시 채워지는 경우는 규칙 1의 활성화와 규칙 3의 승격 둘뿐이며, 둘 다 $j$를 $i$ 뒤로 보낸다(D-08). 연속 Job들은 스로틀되지 않는 한 예산을 새로 받지 않는다(규칙 4). 따라서 $j$가 $i$보다 앞서 쓰는 양은 $i$의 활성화 시점에 남아 있던 예산을 넘지 않는다. D-09의 $\sum_{j \ne i} (C_j + \delta_{ovr})$ 항은 이 성질에 근거한다. 이 성질은 $j$가 MIT를 위반하거나 예산을 초과해도 성립한다.

**위반 후 동작(위반의 비용은 위반한 태스크가 진다)**: MIT 위반 활성화가 잔여 예산을 다 쓰면 윈도우 끝까지 스로틀되었다가 **다음 윈도우의 예산**으로 실행된다(규칙 3). 그 뒤의 활성화는 간격이 MIT 이상이어도 열린 윈도우 안에 떨어지면 새 예산을 받지 못하고(규칙 1, `CARRYOVER`) 남은 예산으로 실행된다. 예산이 모자라면 윈도우 끝까지 기다린다. 다음 예시는 윈도우 기준으로 쓴 것이다. 각 Job은 예산 2 ms를 모두 쓴다고 가정한다.

```text
T_s = 18 ms, C_s = 2 ms, MIT = 18 ms (T_s <= MIT).
t= 0.0  J1 도달, 활성화 → 윈도우 W1 = [0, 18), 예산 2 ms. J1 실행 0~2
t=15.0  J2 도달 (J1 도달과의 간격 15 ms, MIT 위반). 활성화 시 W1이 끝나지 않음
        → 규칙 1 불충족, 예산 0 유지, 스로틀
t=18.0  승격 (규칙 3): 예산 2 ms, 윈도우 W2 = [18, 36). J2 실행 18~20 (J2 응답 시간 5 ms)
t=33.0  J3 도달 (J2 도달과의 간격 18 ms, MIT 준수). 33.0은 W2 안
        → 규칙 1 불충족, CARRYOVER, 잔여 예산 0, 36.0까지 스로틀
t=36.0  승격: 윈도우 W3 = [36, 54). J3 실행 36~38 (J3 응답 시간 5 ms, 도달 기준 3 ms 지연)
```

윈도우 안에서는 예산이 다시 채워지지 않는다. Job이 쓴 만큼만 잔여 예산이 줄어든다. 서버는 윈도우가 끝난 뒤 첫 활성화(규칙 1) 또는 승격(규칙 3)에서 온전한 $C_s$를 받아 도달 위상에 다시 맞는다(P-a 상태로 복귀). 복귀 시점은 도달 간격과 윈도우 위상에 따라 달라지며 그 지연은 측정 항목이다. $C_s$는 실제 실행 시간보다 여유 있게 선언한다(HLD-008 §8). 이 구간에서 마감을 놓친 Job은 DeadlineMonitor가 `THROTTLED`와 함께 보고하며, 실행 구간 레코드에는 `CARRYOVER`가 남아 실행 결함과 구분된다(D-10). 어느 경우에도 다른 서버와 L1에 대한 보장((P-b), (P-c))은 유지된다.

**격자 정렬 보충을 기각한 이유(반례)**: $T_s$ = 18 ms, $C_s$ = 2 ms인 서버가 있고, 같은 CPU에 [16, 18) ms와 [36, 38) ms를 점유하는 TT 블록이 있다고 하자. 격자 보충 시각은 0, 18, 36 ms다. Event A는 도달 16 ms, 마감 21 ms, Event B는 도달 34 ms, 마감 39 ms다(D = 5 ms).

```text
t=16.0  Event A 도달 → TT 블록 때문에 18.0까지 대기
t=18.0  격자 보충 (remaining = 2 ms). A 실행 18.0~20.0 → [18, 36) 주기의 예산 사용
t=34.0  Event B 도달 (A로부터 18 ms: MIT 준수). remaining = 0 → Throttled
t=36.0  격자 보충. 그러나 TT 블록 [36, 38)
t=38.0  B 실행 38.0~40.0 → 완료 40.0 > 마감 39.0 (DMISS)
```

이 구성은 승인식(D-09)을 통과한다. 즉 격자 정렬에서는 선언을 지키는 태스크가 스로틀되어 승인식이 보장한 마감을 놓칠 수 있다. 활성화 기준 윈도우에서는 A의 윈도우가 [16, 34)이므로 B는 34.0에 온전한 예산을 받아 36.0 전에 끝난다.

**대안**
| 대안 | 판정 |
|:--|:--|
| 주기 격자 정렬 지연 보충(현행 `cbs_lazy_replenish`) | 위 반례로 기각. BPF 변경은 가장 작지만 건전하지 않다 |
| 소비 조각마다 보충 시각을 기록하는 완전한 sporadic server | 서버당 보충 목록이 필요해 BPF가 복잡해진다. 단일 스레드 서버에는 과하다. 기각 |
| 마감 기반 CBS(예산 소진 시 서버 마감 연기, EDF 순서) | D-08 EDF와 함께 P2에서 검토 |
| 승격한 윈도우를 승격 시각이 아니라 Job의 활성화 시각에 맞추기(`replenish_at = t_act + T_s`) | 위반 직후 Job부터 도달 위상에 다시 맞아 복귀가 빠르다. 그러나 위반을 반복하는 서버가 $T_s$마다 $2C_s$를 얻어 윈도우당 상한 (P-b)가 깨진다. 다른 서버의 보장 (P-c)은 유지되므로 P2에서 측정 후 재검토(F-9) |
| 위반 Job을 거절하거나 폐기 | 수락된 Event는 Job을 가진다(I1). 거절은 실제 도달을 잃고, 폐기는 워커의 정책 결과로만 가능하다. 기각(D-05) |

**결과**: `replenish_at`은 격자가 아니라 서버별 활성화에 따라 움직인다. Replenisher는 서버별 시각을 BPF에서 읽어 따라간다(D-07). DDR-007 §4.5의 "주기 격자" 서술은 정합 갱신 대상이다(F-2). REQ-008 OI-4는 본 결정으로 종결한다. 설계 가정에 $T_s \le MIT$와 블로킹 없음이 추가되었으며, 위반 시 복귀 시간은 측정 항목이다.

---

### D-05. MIT: 도달 기준 계수·보고, 거절하지 않음 — P1

**결정**: MIT는 Event source의 **도달** 제약이다(REQ-008 §2.6). 계수는 도달 경계, 즉 게시 경로에서 한다. 활성화 기준 MIT 검사는 두지 않는다. BPF에서 MIT 검사와 `MIT_VIOLATION` 발행을 제거한다.

- **검사**: 게시 프로토콜의 도달 단계에서 $t_{arr} - last\_arr\_ns < MIT - tol_{mit}$이면 슬롯의 `mit_viol`을 증가시키고 `last_viol_ns = t_arr`로 기록한다. 이 값은 게시 주체만 쓴다(D-19).
- **계수 대상**: MIT 상태에 도달한 모든 게시를 도달로 센다. 수락된 것, 넘침으로 거절된 것, 되돌려진 것이 모두 여기에 든다. 첫 단계에서 source가 비활성이어서 거절된 게시는 도달이 아니다.
- **위반도 수락한다**: MIT를 위반한 도달도 **수락하여 큐에 넣고 Job을 만든다.** 위반을 거절하지 않는다. 위반은 계수·보고된다.
- **보고**: TIMPANI-N의 EventSourceMonitor가 `mit_viol`의 증가분을 활성화 시점 기준값과 비교하여 `MIT_VIOLATION`을 만든다. source당 $T_s$ 윈도우마다 한 건이며 위반 횟수를 함께 싣는다(D-10).
- **집행**: 집행은 MIT 위반 여부와 무관하게 윈도우 상태만 본다(D-04). 위반 뒤 Job은 윈도우 규칙에 따라 스로틀되거나 `CARRYOVER`를 받을 수 있으며, 이는 보고된다.
- **MIT 연관 표식**: 마지막 위반 시각 `last_viol_ns`가 마감을 놓친 Job의 $t_{arr} - T_s$ 이후이면 `MIT_RELATED` 표식을 붙인다. 표식은 FaultMonitor가 `last_viol_ns`를 읽어 계산한다(D-10).
- **허용 오차**: `tol_mit`(후보 0)은 도달 기준 검사에만 쓴다. 도달 시각은 깨움 지터를 받지 않으므로 `tol`(D-04 규칙 5)과 분리한다.

**대안**
| 대안 | 판정 |
|:--|:--|
| (i) 활성화 기준 MIT 검사를 BPF에서 수행 (9209bc5 어댑터 K) | 커널만으로 가능하지만, 처리 중 도달과 블로킹 조각을 거짓 위반으로 계수한다. 깨움 지터가 위반으로 관측된다. 기각 |
| (ii) 도달 기준과 활성화 기준을 병행하고 원천을 전환(마커 활성 전환, 이전 D-05) | 원천 전환과 중복 계수 방지 로직이 복잡하다. 기각 |
| (iii) MIT 위반 도달을 거절 | 앱이 실제 발생시킨 도달을 잃는다. 기각 |
| (iv) 위반 도달을 폐기(drop) | 수락된 Event는 Job을 가진다(I1). 폐기는 워커의 정책 결과로만 가능하다. 기각 |
| (v) 보충 시각까지 지연(enqueue를 THROTTLED로 강제) | 정상 잔여 예산을 쓰지 못하게 하여 지연만 늘린다. 기각 |

**결과**: BPF의 `mit_viol_count`, `TaskMeta.last_act_ns`의 MIT 용도, `FAULT_MIT_VIOLATION` 발행은 제거된다. MIT 계수는 SHM 누적 카운터 하나로 일원화된다. 도달 기준 검사의 비용은 게시 경로의 원자 연산 몇 개다.

---

### D-06. 배정 CPU 고정: CPU별 L2 DSQ — P1

**결정**: 전역 `DSQ_CBS(101)`/`DSQ_THROTTLED(102)`를 **CPU별 DSQ**로 바꾼다: `DSQ_CBS_BASE + cpu`, `DSQ_THROTTLED_BASE + cpu`(예: `0x100 + cpu`, `0x200 + cpu`; `init()`에서 isolated 마스크의 CPU마다 생성). `ops.enqueue`는 `meta->assigned_cpu`의 L2 DSQ에만 삽입하고, `ops.dispatch(cpu)`는 자기 CPU의 L2 DSQ만 소비한다. `ops.select_cpu`는 항상 `assigned_cpu`를 반환한다. `sched_setaffinity`는 사용하지 않는다(현행 원칙 유지; cpuset으로 허용된 CPU 내에 있어야 함).

**대안**: (i) `select_cpu`만으로 유도(현행) — 전역 DSQ consume으로 타 CPU 실행 가능, 기각. (ii) `sched_setaffinity`로 하드 고정 — 앱 투명성 저해·cgroup cpuset과 충돌 가능, 기각.

**결과**: `promote_throttled_if_replenished`도 CPU별로 순회한다. 스캔 상한 `PROMOTE_BUDGET`(16)은 CPU당 L2 태스크 수 상한 `MAX_L2_PER_CPU`(16, D-09)와 같아 윈도우가 끝난 서버가 스캔에서 빠지지 않는다. `CbsConfig`에 배정 CPU 필드가 필요하다(D-11).

---

### D-07. 백업 보충(Replenisher) 스레드와 CPU 재스케줄 유발 수단 — P1

**결정**: timpani-n에 `Replenisher` 스레드(SCHED_FIFO, Non-Isolated CPU)를 추가한다. 우선순위는 TimerMaster − 1(후보 98)로 두고 PoC에서 확정한다. Replenisher는 **서버 상태를 쓰지 않는다.** 하는 일은 Throttled 서버의 윈도우가 끝난 시점에 배정 CPU가 dispatch 경로를 다시 타게 하는 것뿐이고, 예산 갱신과 승격은 BPF가 그 CPU의 런큐 잠금 아래에서 한다(D-04 규칙 3, 6). 이렇게 하면 사용자 공간의 쓰기가 다른 CPU에서 진행 중인 예산 차감을 덮어쓰는 갱신 유실이 생기지 않는다.

**동작**: 서버별 "다음 확인 시각"을 min-heap으로 관리하고 `CLOCK_MONOTONIC` 절대 시각까지 대기한다. 깨어나면
1. BPF `BPF_PROG_TYPE_SYSCALL` 프로그램 `timpani_replenish_kick`을 `bpf_prog_test_run_opts()`로 실행한다(인자: 서버의 `cbs_map` 키와 배정 CPU). 프로그램은 `cbs_map`을 **읽어** 서버가 Throttled이고 `now ≥ replenish_at`이며 그 CPU의 TT 슬롯이 활성이 아닐 때만 `scx_bpf_kick_cpu(cpu, 0)`을 호출하고, 서버의 `(throttled, remaining, replenish_at)`을 돌려준다. TT 슬롯이 활성이면 kick하지 않는다. 여기서 "활성"은 현행 `tt_active_on_cpu`의 판정, 곧 그 CPU에 실행 가능한(runnable) TT 태스크가 있다는 뜻이다(시간표상의 슬롯 구간이 아니다). 그 동안에는 dispatch가 L2를 내보내지 않는다. TT 태스크가 잠들면 CPU가 다음 태스크를 고르면서 dispatch가 호출되고, 그때 승격이 일어난다.
2. 다음 확인 시각: kick 프로그램을 실행하지 못했거나(일시적 오류), 서버가 아직 Throttled이고 윈도우가 이미 끝났으면(승격이 아직 일어나지 않음) `now + 1 ms`(`REPLENISH_RETRY`), 돌려받은 `replenish_at`이 미래이면 그 시각, 그 밖에는 `now + T_s`.

`replenish_at`은 항상 "갱신 시점 + $T_s$"로만 바뀌므로(D-04 규칙 1, 3), 열려 있는 윈도우의 끝보다 늦게 확인하는 일은 없다. 윈도우가 끝났는데 승격되지 않은 서버는 승격될 때까지 1 ms마다 다시 확인한다. 불필요한 실행은 무해하다(프로그램은 조건이 맞을 때만 kick하고 상태를 바꾸지 않는다).

**재스케줄 유발 수단(우선순위)**
1. 위 `timpani_replenish_kick`. `SCX_KICK_IDLE`이 아니라 플래그 0을 쓴다. 대상 CPU가 유휴로 들어가는 도중이면 `SCX_KICK_IDLE`은 kick을 생략할 수 있다. 플래그 0은 실행 중인 태스크를 선점하지 않고 재스케줄만 요청한다. sched_ext의 kfunc 집합이 SYSCALL 프로그램 타입에서 호출 가능하다는 전제이며 **P1 PoC로 확인**한다(OI-9). PoC는 (a) 호출 가능 여부, (b) 유휴 CPU에서 kick → 승격 지연(TMG-003), (c) L2 실행 중인 CPU에 대한 kick의 비용과, kick 직후 TT 슬롯이 시작되는 경계 조건에서의 TT 지터 영향, (d) 예산 차감과 동시에 실행해도 `remaining`이 유실되지 않음을 확인한다.
2. 대체: CPU별 보조 "kicker" 스레드(SCHED_FIFO, 해당 Isolated CPU 고정)를 futex로 깨워 즉시 재대기시킴 → 해당 CPU에서 `schedule()` → `ops.dispatch` 재진입 → 승격. Isolated CPU에 비-SCX RT 스레드가 짧게 실행되는 부작용(수 µs)이 있다.

**대안**: `bpf_timer` — DDR-007 §4.5.1에서 PREEMPT_RT 사유로 기각된 결정 유지. Replenisher와 DeadlineMonitor를 한 스레드로 합치는 안은 P1에서 기각한다(D-20 대안, F-12).

**결과**: DeadlineMonitor(D-20)는 Replenisher와 별개의 스레드다. 두 스레드의 우선순위 후보가 같지 않도록 PoC에서 정한다.

---

### D-08. 동일 CPU 다중 L2 서버 순서: P1 FIFO, P2 EDF

**결정**: P1은 CPU별 L2 DSQ를 **실행 구간의 활성화 시각 순서(FIFO)**로 소비한다. 구현은 DSQ를 우선순위 큐로 쓰고 키를 다음과 같이 준다(`scx_bpf_dsq_insert_vtime`). 커널 이름은 대상 vmlinux.h에서 확인한다.
- 새 실행 구간, 그리고 선점·슬라이스 만료 후 재삽입되는 실행 구간: `vtime = t_act`(자신의 활성화 순위를 유지한다).
- 스로틀 후 승격되는 서버: `vtime = 승격 시각`(큐의 뒤로 간다. 예산을 초과한 서버가 먼저 활성화되었다는 이유로 다른 서버를 다시 앞지르지 못한다).

P2에서 키를 절대 마감으로 바꿔 **EDF**로 전환하고, 승인식을 윈도우 수요·공급 함수식으로 교체한다. P2 EDF가 쓰는 키는 Job의 절대 마감 $t_{dl}$이다(D-20). 그 전환은 BPF에 커널 공간 키로 Job 마감을 전달하는 일을 포함하므로 별도 검토한다.

**근거**: DSQ의 단순 삽입 순서는 활성화 순서와 다르다(TT 선점 후 재삽입된 실행 구간이 나중에 활성화된 것 뒤로 밀린다). 활성화 시각을 키로 써야 D-09의 "먼저 활성화된 다른 서버만 앞선다"는 전제가 성립한다. 이 순서와 D-04의 성질 (P-c)가 합쳐져 다른 서버의 간섭 상한이 서버당 $C_j$가 된다. EDF는 수용률을 높이지만 승인식과 BPF 변경이 크다.

---

### D-09. 승인 충분조건(P1): 이용률 + FIFO 블로킹 합 — 한 식, 모든 L2 태스크

**결정**: TIMPANI-O는 CPU $k$에 대해 다음을 모두 검증한다.

```text
(1) 이용률:     U_tt(k) ≤ U_bound  AND  U_tt(k) + Σ_j C_j/T_j + U_overhead ≤ U_bound
(2) 최소 Gap:   min_gap(k) ≥ CBS_MIN_EXEC_US            (경고 → 거부로 승격)
(3) 마감(FIFO): ∀ L2 task i on k:
                C_i + B_tt(k, D_i) + L_del_max + Σ_{j≠i} (C_j + δ_ovr) ≤ D_i
(4) 파라미터:   0 < C_i ≤ D_i ≤ MIT_i,  T_i ≤ MIT_i,
                task_id는 15바이트 이하이고 노드 안에서 유일
(5) 개수:       CPU k의 L2 태스크 수 ≤ MAX_L2_PER_CPU (16)
```

조건 (3)은 새로 추가되는 태스크뿐 아니라 CPU $k$에 이미 배치된 **모든** L2 태스크에 대해 다시 검사한다. 새 태스크의 $C$가 기존 태스크의 간섭 항에 더해지기 때문이다. 조건 (3)과 (4)는 **Event 게시 여부와 무관하게 모든 L2 태스크에 같다**(D-21).

- $B_{tt}(k, L)$: CPU $k$에서 길이 $L$인 임의 윈도우 안의 TT 점유 시간의 최댓값이다(인접·연속 슬롯 병합 후, hyperperiod 경계 wrap 포함). 윈도우가 여러 TT 블록을 덮으면 그 합이 된다. 구현(`TtBlocking::occupancy_within_us`)도 같은 값을 계산한다.
- $L_{del\_max}$(도달에서 유휴이고 TT가 없는 CPU에서 첫 디스패치까지의 지연 상한) $= L_{post} + L_{wake}^{max}$. $L_{post}$는 도달(게시)부터 실행 가능 상태가 되기까지의 지연이며 TIM-011(새 타이밍 항목, 후보 50 µs, 측정 필요)이다. $L_{wake}^{max}$는 실행 가능 상태에서 첫 실행까지의 지연이며 TIM-001(후보 50 µs)이다. TIM-002(TT가 CPU를 점유한 중의 깨움)는 이 항에 넣지 않는다. TT 점유는 $B_{tt}$ 항이 덮는다(A24). 후보 $L_{del\_max} = 100$ µs. 이 값들은 측정 목표이며 증명된 최악값이 아니다. TIM-001/002는 커널 측 정의를 유지한다.
- $\delta_{ovr}$: 한 서버가 예산을 넘겨 실행할 수 있는 최대 양(틱 해상도에 의한 overrun)이다(TIM-004, TMG-004). P1에서는 HZ=1000 기준 1 ms로 잡는다. nohz_full CPU에서는 틱 재시작 지연을 더해 측정해야 한다. 이 항은 이전 공식이 무시한 틱 overrun을 반영한다.
- $MIT_i$: `min_inter_arrival_us`이며, 없으면 주기 $T_i$를 쓴다(D-11 규칙).

**(4)의 사이드 조건 $D_i \le MIT_i$**는 적합한 도달에서 source당 살아 있는 Event가 한 개 이하임을 보장한다(Job이 마감을 지키는 경우). 그래서 자기 큐잉 항이 식에 필요하지 않다. MIT 위반 시에는 큐가 최대 QDEPTH까지 쌓이고 뒤 Job이 놓칠 수 있으며, 이는 보고되지만 막지 않는다.

**가정(결과의 문서화 필수 항목)**: 이 식은 다음 가정 아래의 충분조건이다.
1. TT 태스크는 선언된 runtime 안에서 실행한다. TT overrun은 TT DMISS이며 L2 보장 밖이다.
2. $\delta_{ovr}$과 $L_{del\_max}$가 측정값대로 성립한다.
3. source의 도달이 MIT를 지킨다.
4. Job이 블로킹하지 않는다. 블로킹하는 source는 마감이 감시되지만(D-20) admission은 그 마감을 보장하지 않는다.
5. 어댑터는 도달 시점에 게시한다. 주어진 과거 $t_{arr}$은 D의 일부를 소비한다.
6. 각 Job의 CPU 수요가 선언 예산 $C_i$를 넘지 않는다. 헤드룸을 두고 선언한다.
7. 각 Job은 다음 도달 전에 워커가 잠들 만큼 빨리 끝난다(D-04 (P-a) 경계 조건).
8. $tol$이 측정된 활성화 지터를 덮는다.

**근거**: Job $i$의 응답 시간(도달에서 완료까지)은 (a) 다른 서버가 $i$보다 앞서 실행하는 양, 서버당 $C_j + \delta_{ovr}$ 이하(D-04 (P-c), D-08, 틱 overrun), (b) 응답 윈도우 안의 TT 점유 $B_{tt}(k, D_i)$ 이하, (c) 도달에서 첫 디스패치까지의 지연 $L_{del\_max}$ 이하, (d) 자신의 실행 $C_i$ 이하(D-04 (P-a)의 조건에서 스로틀되지 않음)의 합을 넘지 않는다. 구체적인 파라미터 예시와 산술은 HLD-008 §8과 AC-008-16을 참조한다.

**대안**
| 대안 | 판정 |
|:--|:--|
| 클래스(Event 연결 여부)별 별도 공식 | TIMPANI-O는 클래스를 알 수 없다. 기각(D-21) |
| 기존 `L_wake_max`만 사용하고 도달 기준 지연과 틱 overrun을 무시 | 도달 기준 응답 시간에서 게시 경로가 빠지고, 틱 overrun이 보장을 깬다. 기각 |
| 공식 수준의 자기 큐잉 항 추가 | 적합한 도달에서 불필요하다($D_i \le MIT_i$). MIT 위반 상황의 보고는 D-05가 맡는다. 기각 |

**선행 수정**: CPU별 vs 전역 hyperperiod 불일치, CBS 전용 노드 `hyperperiod_us = 0`, TT $U_{bound}$ 미검사(REQ-008 §10-16)를 함께 수정한다.

**P2 전환**: 조건 (3)의 FIFO 블로킹 합은 P2에서 $sbf_k(L)$과 $dbf$ 정밀식(EDF)으로 바꾼다. 이 항목은 본 절과 §3.3의 P2 항목으로 추적한다.

**CPU 선택**: 조건 (1)~(3)을 만족하는 후보 중 **worst-fit**(잔여 이용률 최대)을 유지한다(DDR-007 §3.3).

---

### D-10. 폴트 원천·분류·보고 경로 — P1

**결정**

- **원천과 경로**

| 폴트 | 원천 | 보고 경로 |
|:--|:--|:--|
| DMISS (L2) | DeadlineMonitor(마감 경과 감지, D-20), 워커 종료 전이(`t_end > t_dl`, `LATE_COMPLETION`), 정리 sweep과 워커의 대기 중 stale 세대 정리(`DETECTED_LATE`) | **계수 경로**(슬롯 `missed` 증가분, 손실 없음)가 기존 임계 규칙을 구동한다. **상세 경로**(미스 레코드와 Job log의 `MISSED` 항목, 최선 노력)는 `(task, gen, seq)`로 중복을 제거해 보고에 붙인다 |
| BUDGET_EXCEED | BPF(예산이 양수에서 0으로 바뀌는 전이 시 에피소드당 1회) | BPF `fault_ringbuf` → FaultMonitor |
| MIT_VIOLATION | 슬롯 `mit_viol` 증가분(활성화 시점 기준값 대비), 도달 기준(D-05) | 계수 경로. source당 $T_s$ 윈도우당 1건, 위반 수 포함 |
| DMISS (TT) | DDR-012 경로 | 변경 없음 |

- **FaultInfo.flags**(하나의 표, 모든 문서 공통. 값의 정본은 LLD-008)

| 비트 | 이름 | 뜻 | 세팅 주체 |
|:--|:--|:--|:--|
| 0x1 | MIT_RELATED | 마지막 MIT 위반 시각 `last_viol_ns`가 $t_{arr} - T_s$ 이후 | FaultMonitor (`last_viol_ns` 판독으로 계산) |
| 0x2 | THROTTLED | 감지 시점의 `CbsState.throttled` 값. DeadlineMonitor 감지에서만 채운다. 워커 종료나 정리 sweep이 설정한 미스에는 쓰지 않는다 | DeadlineMonitor |
| 0x4 | NOT_STARTED | 감지 시점에 레코드가 QUEUED였다(9209bc5의 `EARLY` 대체) | DeadlineMonitor |
| 0x8 | LATE_COMPLETION | 워커의 종료 전이가 마감 이후에 기록되었다(9209bc5의 `MARKER` 대체) | 워커 |
| 0x10 | DETECTED_LATE | 닫기 절차에서 마감 경과를 발견했다. 데몬 재시작 시 정리 sweep이나 워커 대기 중 stale 세대 정리이며, 다운 중에는 실시간 감지가 없다 | 정리 sweep, 워커 stale 세대 정리 |

- Job 기록은 같은 비트를 같은 뜻으로 쓰고, 자체 outcome 필드와 `missed` 표시를 더한다.
- **BPF 실행 구간 플래그**(LLD-008 소유): `CARRYOVER`, `THROTTLED`, `TASK_EXIT`. `CARRYOVER`는 실행 구간 표식이며 P1에서 DMISS 보고 플래그가 아니다.
- **폐기된 BPF 비트(재사용 금지)**: `FAULT_MIT_VIOLATION` 발행과 실행 구간 레코드의 `mit_violation` 플래그, `JOB_FLAG_EARLY_DMISS`(조기 DMISS, `early` 플래그), L2용 DMISS 판정. 이 비트들은 두 표를 혼동하지 않도록 명시적으로 폐기한다.
- **BPF `FaultEvent.fault_type` 번호**: `DMISS=0`(TT 경로에만 쓴다), `BUDGET_EXCEED=1`, `MIT_VIOLATION=2`(BPF가 발행하지 않으며 번호를 재사용하지 않는다), `WAKEUP_LATENCY_EXCEED=3`과 `WATCHDOG=4`는 예약. `BUDGET_EXCEED`는 `run_seq`와 `t_act`를 싣는다.
- **중복 흡수**: BUDGET_EXCEED 에피소드 키는 `replenish_at`이다. 에피소드는 Job 경계와 무관하므로 Job 식별자를 키에 넣지 않는다.
- **DMISS 임계**: DDR-012 규칙(count가 `current_limit`을 넘으면 보고)을 유지한다. 계수 경로의 입력은 등록 활성화 시점 기준값 대비 `missed` 증가분이다. 모든 미스는 필수 통계에 계수되며, 보고는 이 규칙이 보내라고 할 때만 나간다. 보고 지연 상한은 보내진 보고에 적용된다(A4).
- **세대 규칙**: FaultInfo는 현재 등록의 태스크에만 보낸다. 이전 세대의 미스(닫는 과정에서 발견)는 레코드와 통계에만 남는다.
- **MIT_VIOLATION 주기**: source당 $T_s$ 윈도우당 FaultInfo 1건, 위반 수를 싣는다.
- **타입 매핑**: TIMPANI-O `handle_fault_info`는 같은 의미의 유형으로 Pullpiri `FaultType`에 매핑한다(UNKNOWN 축약 제거). P1에 쓰는 유형(DMISS, BUDGET_EXCEED, MIT_VIOLATION)은 두 proto에서 번호가 같다.
- BPF `FaultEvent.fault_type` 번호와 proto `FaultType` 번호는 **다르다.** TIMPANI-N `NodeClient`가 명시적 변환표로 매핑하며 정수 캐스팅을 금지한다(LLD-008 §5.5).
- FaultInfo에 `task_id`, `node_id`, `timestamp_ns`를 채운다. node_control FaultInfo의 `timestamp_ns`는 이미 있으나 현재 설정되지 않는다. schedinfo FaultInfo의 `timestamp_ns`는 새로 추가한다(D-11, A19).

**대안**
| 대안 | 판정 |
|:--|:--|
| 완료 시점에 BPF가 DMISS 판정 (9209bc5) | 완료 없는 Job의 미스를 못 잡는다. 기각 |
| 상세 경로만으로 DMISS 보고 | 넘침 시 계수가 누락된다. 기각. 계수 경로는 필수 |
| 미보고 비트로 링 재사용을 보고 전까지 막기 | 데몬이 다운되면 데이터 경로가 멈춘다. 기각(P1-008 §10 검토 결과) |
| 조기 DMISS를 BPF가 마감 경과 직후 발행 (9209bc5 `EARLY`) | 감시가 DeadlineMonitor로 옮겨 중복된다. 기각 |

**결과**: TIMPANI-O의 `handle_fault_info`는 유형과 플래그를 보존한다. `count`와 `flags` 필드는 protobuf 변경을 요구한다(D-11). 승인이 없으면 보고는 `count`만 싣는다(`flags`, `job_seq`, `job_gen` 없음, A18). 승인 전에는 플래그를 검사하지 않는다.

---

### D-11. proto 변경 범위(P1, 필드 추가만, 모두 명시 승인 대상)

**결정**: protobuf 변경은 필드 추가만 하며 기존 번호는 바꾸지 않는다. **모든 protobuf 변경은 명시적 승인이 필요하다.** 승인되지 않은 항목이 있으면 해당 기능은 계수만 보고하거나 기능을 빼고 구현한다. 번호는 LLD-008에서 최종 확정한다.

| 파일 | 변경 | Phase | 상태 |
|:--|:--|:-:|:--|
| `schedinfo.proto` `TaskInfo` | `uint32 min_inter_arrival_us = 11` 추가. Sporadic에서 0이면 전환 기간 동안 `period`를 MIT로 해석(경고) | P1 | 승인 필요 |
| `schedinfo.proto` `FaultType` | `BUDGET_EXCEED = 2`, `MIT_VIOLATION = 4` 추가. `WATCHDOG = 3` 예약(사용은 P2). 같은 유형은 `node_control`과 번호가 같다. `WAKEUP_LATENCY_EXCEED`는 P2에 추가 | P1 | 승인 필요 |
| `schedinfo.proto` `FaultInfo` | `uint64 timestamp_ns`, `uint32 count`, `uint32 flags` 추가. `flags`는 D-10의 비트 표를 쓴다. `count`는 이 보고의 발생 수다. 누적 필드 `cumulative_dmiss`와 뜻이 다르므로 재사용하지 않는다(A19) | P1 | 승인 필요 |
| `node_control.proto` `CbsConfig` | `uint32 min_inter_arrival_us = 9`, `optional uint32 cpu = 10` 추가. `optional`로 CPU 0과 미지정을 구분한다. TIMPANI-N은 필드가 없거나 Isolated CPU가 아닌 엔트리를 적용 오류로 처리하며, 파티션 cpuset으로 대체하지 않는다 | P1 | 승인 필요 |
| `node_control.proto` `LayerConfig` | L2 엔트리를 `model = EVENT_DRIVEN`, `layer_index = 2` 레이어로 분리 발행(메시지 변경 없음, 발행 규칙 변경) | P1 | 메시지 변경 없음 |
| `node_control.proto` `FaultType` | `MIT_VIOLATION = 4` 추가, `WAKEUP_LATENCY_EXCEED = 5` 예약. 기존 `WATCHDOG = 3`은 P2까지 사용하지 않는다 | P1 | 승인 필요 |
| `node_control.proto` `FaultInfo` | `uint32 count`, `uint32 flags`(MIT_RELATED 0x1, THROTTLED 0x2, NOT_STARTED 0x4, LATE_COMPLETION 0x8, DETECTED_LATE 0x10), `uint64 job_gen`, `uint32 job_seq` 추가. `(job_gen, job_seq)`는 Event 식별자 `(gen, seq)`와 1:1이며, `(task, gen, seq)`를 한 객체 안에서 유일하게 만든다. 재시작 사이의 유일성은 `instance_id`에 기대므로 보장이 아니다(A14). `count`는 이 보고의 발생 수다. 누적 필드 `dmiss_count`와 뜻이 다르므로 재사용하지 않는다(A19). `timestamp_ns`(field 5)는 이미 있다 | P1 | 승인 필요 |
| `node_control.proto` `HierarchicalScheduleTable.table_id` | 발행마다 단조 증가 문자열(`"table_<gen>"`). 메시지 변경 없음 | P1 | 메시지 변경 없음 |
| `schedinfo.proto` `TaskInfo.temporal_class`(태스크 단위), `Response.reason/message` | — | P2 | — |
| `node_control.proto` `NodeStatus` L2 통계 | — | P2 | — |

- **큐 깊이와 payload 크기는 proto 변경이 없다.** P1에서는 빌드 상수다(D-19). 워크로드 선언에 담는 일은 F-11이며 별도 승인이 필요하다.
- `CbsConfig` 명칭은 유지한다(OI-13 종결: 신규 메시지는 승인 범위를 넓힌다).

**근거**: `FaultInfo`에는 현재 `count`, `flags`, 작업 식별자가 없다. `job_gen`이 없으면 데몬 재시작 뒤 같은 `seq`가 다시 나타날 수 있다.

---

### D-12. 지연 식별(Late binding): TaskRegistry 부활 + 주기 재스캔 — P1

**결정**: 사장된 `TaskRegistry`를 식별 모듈로 복원한다. 테이블 적용 시 미해결 L2/TT 태스크 이름을 보관하고, 500 ms 주기(설정 가능)로 `/proc/*/comm` 및 `/proc/*/task/*/comm`을 재스캔하여 발견 즉시 BPF 등록·`SCHED_EXT` 적용·SHM 슬롯 바인딩을 수행한다(`T_bind` ≤ 1 s). TID 소멸(`/proc/<pid>/task/<tid>` 부재 또는 BPF `exit_task` 콜백)을 감지하면 상태를 정리하고 미해결 목록으로 되돌린다. 종료 레코드의 `bind_gen`으로 이전 바인딩의 늦은 종료가 새 바인딩을 해제하지 않게 한다(D-17).

**대안**: (i) `tp_btf/task_newtask` + `sched_process_comm`(comm 변경) 트레이스포인트로 즉시 바인딩 — 지연 ≪ 1 s이나 BPF/ringbuf 경로 추가, P2. (ii) cgroup 이벤트(`inotify` on `cgroup.procs`) + Pullpiri 메타데이터 — REQ-NFR-004의 최종 형태, P2.

---

### D-13. TT floor(이중 모드)·Watchdog: P1 미채택

**결정**: DDR-007 §0의 결정(이중 모드 미채택)을 유지한다. 이벤트 침묵 감시(`WATCHDOG`)는 P2에서 **사용자 공간 EventMonitor**(FaultMonitor 확장, `FaultPolicy.watchdog_period_us` 또는 `max_inter_arrival_us` 신규 필드)로 구현한다. DDR-002 §7은 "DDR-007/008에 의해 대체됨"으로 정합 갱신한다. P1의 마감 감시(D-20)는 침묵 감시와 다른 기능이다.

---

### D-14. DDR-005 사용자 앱 인터페이스 정합

**결정**: DDR-005 "ttsched_wait_next_period()는 L1/L2 모두 필수"를 다음과 같이 개정한다. **L1은 `ttsched_wait_next_period()`를 필수로 쓴다. L2에서 명시적 Event 연결을 쓰는 워커는 `ttsched_evt_wait()`와 `ttsched_evt_done()`의 쌍을 쓴다(Event 연결을 쓰는 경우 필수, D-18).** 표준 블로킹만 쓰는 L2 태스크는 호출 없이 등록되며 예산 집행만 받는다(D-21, FR-L-003). L2 식별 제약(`pthread_setname_np` = `task_id`)은 유지한다.

---

### D-15. 구현 대상: C++ 경로(timpani-o/timpani-n C++ + BPF) — P1

**결정**: P1은 `timpani-o/`(C++), `timpani-n/`(C++ + BPF), `sample-apps/src/libttsched`를 대상으로 구현한다. `timpani_rust/`는 CBS·TemporalClass·NodeStream이 부재하고 timpani-n Rust는 런타임이 없으므로(REQ-008 §10 기반 분석) P1 대상이 아니다(단, Rust `schedinfo.proto` 정의 파일에는 신규 필드만 동기화하며 Rust 코드 변경은 없다). Rust 포트에는 본 DDR의 모델(상태 머신, 승인식, proto)을 그대로 이식하는 후속 작업을 남긴다.

**결과**: `doc/docs/structure.md`의 "Rust만 활발히 개발" 서술과 상충하므로 해당 문서 갱신이 필요하다. C++ 경로는 CI에 포함되지 않으므로 로컬 GTest·통합 스크립트로 검증한다(SRS-008 NFR-007).

---

### D-16. 관측 출력: Job 기록, 실행 구간 레코드, 필수 통계 — P1

**결정**: 관측 출력을 세 가지로 나눈다. 필수 통계는 트레이스 설정과 무관하다.

1. **Job 기록 (워커 Job log)**: 워커가 `done`에서 쓰는 SPSC 로그다(SHM 안, 손실 가능, 드롭 계수). 생산자는 워커뿐이다. TIMPANI-N의 `JobTraceCollector`가 수집한다. 레코드는 `{gen, seq, t_arr, t_dl, t_begin, t_end, outcome, flags}`이며, 워커의 비공유 사본으로 만든다(공유 레코드를 종료 뒤에 읽지 않는다). TIMPANI가 닫은 Job과 미스 레코드도 같은 형식으로 TIMPANI-N의 트레이스 출력에 쓴다. 해당 시각이 없는 필드는 비운다. 트레이스 출력에는 64비트 `instance_id`를 함께 쓰며, 오프라인 분석이 데몬 실행을 나눌 수 있게 한다(A14).
2. **실행 구간 레코드 (BPF `job_trace_rb`)**: `BPF_MAP_TYPE_RINGBUF`(256 KiB). 레코드는 `{…, run_seq, t_act, t_start, t_sleep, budget_used_us, flags}`이며 flags는 `CARRYOVER`, `THROTTLED`를 쓴다. 전역 `trace_enabled` 변수(rodata 아님, 런타임 토글)로 게이트한다. 예외: 태스크 종료 레코드(`ops.exit_task`)는 TaskRegistry 정리와 `bind_gen` 확인을 위해 게이트 없이 항상 발행한다. 채널 이름 `job_trace_rb`는 유지하며, 그 내용이 실행 구간이라는 점을 명시한다.
3. **필수 통계(트레이스와 무관)**: SHM 슬롯의 누적 카운터(D-03)와 BPF 카운터. `EventSourceMonitor`가 주기 스냅샷으로 읽어 출력한다. 활성화 시점의 기준값과 차이로 등록 안의 값을 얻는다. Tracepoint가 필요한 통계(예: 할당되지 않은 CPU에서의 실행)는 필수 집합 밖이며, 어디서 계수하는지 명시한다.

- 응답 시간 통계는 `t_end - t_arr`로 계산한다. 9209bc5의 `t_rel` 기준 오프라인 대조 규칙과 Job–Event 짝짓기 규칙은 폐기한다. 모든 Job 기록이 자기 `t_arr`를 가지므로 짝짓기가 필요 없다. 실행 구간의 `t_act`는 예산 집행을 관측하기 위한 값이다.
- 트레이스 출력 형식(gpdata 확장)은 검증 프레임워크와 협의한다. Timer Master 지터 샘플도 같은 채널 형식으로 출력한다.

**대안**
| 대안 | 판정 |
|:--|:--|
| `fault_ringbuf` 재사용 | 폴트와 트레이스의 대역폭과 우선순위가 섞인다. 기각 |
| 커널 tracepoint(`bpf_trace_printk`, `trace_pipe`) | 비결정적이며 파싱 비용이 든다. 기각 |
| 9209bc5의 `n_events`를 Job 레코드에 기록하고 Job과 Event를 실시간으로 대조 | 비동기 수집에서 경합이 생긴다. 기각 |
| 9209bc5의 `t_rel` 기준 오프라인 대조 | 조건이 깨진 구간을 따로 다뤄야 한다. 기각. 각 기록이 자기 `t_arr`를 가진다 |

---

### D-17. 런타임 갱신(P1): 전체 테이블 재발행 유지, 등록·바인딩 세대

**결정**: P1은 현행 전체 테이블 발행과 전체 재등록을 유지한다. 다음 규칙을 함께 둔다.

- (a) L2 엔트리가 동일하고 태스크가 이미 바인딩되어 있으면 `cbs_map`과 `task_meta_map`에 **쓰지 않는다**. 사용자 공간이 값을 통째로 다시 쓰면 그 사이의 BPF 갱신(예산, 실행 구간 상태, 카운터)을 덮어쓴다.
- (b) 파라미터가 바뀐 엔트리는 **제거 후 재등록**한다. 태스크를 `SCHED_OTHER`로 되돌려 큐에서 빼고, 맵 엔트리를 지우고, 새 엔트리를 쓴 뒤 `SCHED_EXT`로 넣는다. 새로 등록할 때도 맵 엔트리를 먼저 쓰고 `SCHED_EXT` 전환을 나중에 한다(전환 직후의 깨움이 서버 없이 처리되지 않게 한다).
- (c) **활성화(바인딩 시)**: 활성화는 워커 스레드가 `SCHED_EXT`에 바인딩된 뒤에 한다. 테이블 적용 시점이 아니다. 따라서 수락이 성공하면 관리되는 워커가 있다. 순서는 다음과 같다. ① 이전 등록의 미종료 레코드를 닫는다(QUEUED는 CANCELLED, RUNNING은 ABANDONED, 마감이 지났으면 MISSED를 함께 설정). ② 설정(`mit_us`, `deadline_us`)을 쓴다. ③ `last_arr_ns = 0`. ④ 새 `gen`을 만든다(상위 32비트는 `instance_id`, 하위 32비트는 바인딩 횟수 증가). ⑤ `kind = EVT`. TIMPANI-N은 이 시점에 카운터 기준값(`missed`, `mit_viol` 등)을 잡는다.
- (d) **비활성화**(워커 제거, STOP, 워커 종료 감지, 데몬 종료): `kind = NONE`을 먼저 쓰고, 슬롯의 모든 항목을 훑는 닫기 sweep을 한다. QUEUED는 CANCELLED로 닫는다. RUNNING은 워커가 종료되었을 때만 ABANDONED로 닫고, 아니면 워커가 끝낼 때까지 RUNNING으로 두며 마감 감시를 계속한다. 닫는 주체가 마감 경과를 확인하면 MISSED를 함께 설정한다. 대기 중인 워커는 E_INACTIVE를 받는다.
- (e) 같은 `task_id`의 SHM 슬롯과 그 누적 카운터는 유지한다(D-03). 바인딩마다 BPF 실행 구간 상태(`run_seq`, 마지막 활성화 시각, 윈도우 상태)는 0부터 다시 시작한다. `run_seq`는 등록 구간 안에서만 유일하며 레코드의 순서는 `t_act`로 정한다.
- (f) `TaskMeta`에 `bind_gen`을 두어, 태스크 종료 레코드가 이전 바인딩의 것이면 새 바인딩을 해제하지 않게 한다(FR-N-005 갱신).
- (g) 재등록 중 진행 중이던 Job은 닫히거나(ABANDONED) 그 워커가 끝낸 뒤 기록된다. 관측이 끊기는 것은 P1에서 허용된다. 이 경계에서 게시된 Event는 같은 `gen`이 아니므로 새 등록의 상태를 바꾸지 않는다(D-19).
- (h) TT 슬롯 집합이 같으면 Timer Master 재시작을 생략한다(SHOULD).

**대안**
| 대안 | 판정 |
|:--|:--|
| 테이블 적용 시점에 활성화 | 워커가 없는 상태에서 수락이 일어날 수 있다. 기각 |
| 활성화 시 링 인덱스를 0으로 초기화 | 게시자가 쓰는 인덱스를 건드린다(인덱스는 자유 실행). 기각 |
| 재등록마다 누적 카운터 초기화 | 기준값으로 충분하다. 누적 값은 객체 수명 동안 유지한다. 기각 |

---

### D-18. 한 Event – 한 Job, 명시적 Job 경계 — P1

**결정**: 수락된 Event 한 건은 Job 한 건이다. 워커는 `ttsched_evt_wait()`로 Event 한 건을 받아 Job을 시작하고, 처리 후 `ttsched_evt_done(outcome)`으로 반드시 끝낸다. 배치 소비와 암묵 완료는 없다. 연속 Job(R3)은 별개의 Job이며 각각 `t_begin`, `t_end`, MISSED 판정을 가진다. 커널 실행 구간은 Job 경계가 아니다.

- Job 기록은 `{node, task, gen, seq, t_arr, t_dl, t_begin, t_end, outcome, flags}`이며, 워커가 종료한 Job, 미스 레코드, TIMPANI가 닫은 기록이 모두 같은 형식이다(D-16).
- 워커가 `done`을 빠뜨리면 다음 `wait`는 E_STATE를 돌려준다. 워커 스레드가 종료하면 TIMPANI가 RUNNING 레코드를 ABANDONED로 닫는다(D-17).
- 블로킹은 Job 중에도 허용된다. 블로킹 시간은 응답 시간에 포함되며, 블로킹하는 Job은 admission 보장 밖이다(D-09 가정 4).

**근거**: I1은 Job과 수락된 Event의 1:1을 요구한다. 배치 소비는 한 Job이 여러 Event를 대표하게 만들어 Event별 마감과 응답 시간을 정의할 수 없게 한다. 커널 수면 경계는 연속 Job과 블로킹을 구분하지 못한다. 명시적 경계는 워커가 소유하므로 두 경우를 모두 정확히 가른다.

**대안**
| 대안 | 장점 | 단점 | 판정 |
|:--|:--|:--|:--|
| (i) 배치 소비: wait가 대기 건수 n을 돌려주고 한 Job이 n건 처리 (9209bc5) | 호출 횟수 감소 | 한 Job이 여러 Event를 대표한다. 개별 마감과 응답을 정의할 수 없다. 처리 중 도달을 표현할 수 없다 | 기각 |
| (ii) 대기 중 Event를 모두 하나의 Job으로 합치기 | 구현 단순 | (i)과 같다 | 기각 |
| (iii) 커널 수면 경계를 Job으로 사용 (9209bc5 wait point) | 앱 변경 없음 | 연속 Job이 병합되고, 블로킹이 Job을 나눈다 | 기각 |
| (iv) 수면 경계를 시스템 콜 분류나 `sched_yield()` 표식으로 식별 (9209bc5 P2 계획, 이전 F-8) | 커널에서 경계를 식별 | 처리 중 도달과 연속 Job을 구분하지 못한다. 앱 의미를 커널이 추론한다 | 철회 |
| (v) 다음 wait에서 이전 Job을 암묵 완료로 처리 (9209bc5) | 앱 코드 단순 | 실패한 Job이 완료로 기록된다 | 기각 |

**결과**: 워커 코드는 `done` 호출을 가져야 한다. 샘플 앱과 FCW 적용 절차는 HLD-008 §8과 LLD-008에서 다룬다.

---

### D-19. 공통 Event 큐: 고정 레코드 링, 인라인 payload, 수락·넘침·닫기·세대 — P1 (상수와 구조는 승인 필요)

**결정**: Event 큐는 source마다 **고정 크기 레코드 링**(QDEPTH 항목)이며 payload는 레코드 안에 인라인으로 든다.

- **빌드 상수(후보)**: `TTSCHED_EVT_QDEPTH` = 8(2의 거듭제곱, 링 점유 상한: QUEUED, RUNNING, head가 아직 넘기지 못한 종료 항목의 합), `TTSCHED_EVT_PAYLOAD_MAX` = 64 B. 두 값 모두 합의·측정이 필요하며, 공개 API의 일부이므로 승인 대상이다. E_FULL은 살아 있는 Event가 QDEPTH보다 적을 때도 날 수 있다. RUNNING인 Job이 head에 있으면 그 뒤의 닫힌 항목이 head를 막기 때문이다(A13).
- **레코드 (128 B)**: `word`(u64 원자 값, 상위 32비트는 `seq`, 하위 비트는 상태와 MISSED 플래그), `gen`(u64), `t_arr_ns`, `t_dl_ns`, `len`, `payload`.
- **상태**: FREE(초기 상태, 0으로 채운 객체), QUEUED, RUNNING, COMPLETED, DISCARDED, CANCELLED, ABANDONED, REVOKED. FREE는 게시된 상태도 종료 상태도 아니다. 게시 후의 상태 변경은 `word` 전체에 대한 CAS다. `word`가 `seq`를 포함하므로 같은 링 항목의 이전 점유자를 본 CAS는 실패한다.
- **수락(post 프로토콜 요약)**: ① 슬롯의 `kind`를 확인한다(헤더 `magic`은 보지 않는다. 재시작 정리가 `kind = NONE`으로 게시를 막는다, A2). ② `gen`을 읽는다. ③ 도달 검사(D-05)로 MIT 계수를 한다. ④ 가득 찼는지 확인한다. 가득 찼으면 끝난 항목 너머로 `head`를 진행시킨 뒤에도 가득 차면 `overflow`를 올리고 E_FULL이다. ⑤ 레코드를 채우고 `word`를 QUEUED로 게시한 뒤 `tail`을 올린다. ⑥ `kind`와 `gen`을 다시 확인한다. 이 확인이 실패하면 현재 `word`에 따라 되돌린다. QUEUED가 아니면(다른 주체가 이미 가져갔거나 닫았으면) 수락으로 계수한다. 현재 `word`가 QUEUED이고 MISSED가 이미 설정되어 있으면 CANCELLED로 닫고 MISSED를 유지하며 수락과 취소를 계수한다. QUEUED이고 MISSED가 없으면 마감 경과와 무관하게 REVOKED로 되돌리고 E_INACTIVE를 돌려준다. 그것은 Event가 아니다. 되돌림은 MISSED를 새로 설정하지 않는다(A11).
- **넘침**: 가득 찬 큐에서 이미 수락된 Event를 버리지 않는다(I1). 새 게시만 거절한다.
- **소비와 종료**: 워커가 Job을 받으면 QUEUED에서 RUNNING으로 CAS한다. 레코드는 RUNNING인 동안 워커가 복사한다. 종료는 `done`의 CAS다. `done`은 먼저 `word`(W0)를 읽어 열린 Job의 seq와 ABANDONED 여부를 확인한다(A6). `head`는 종료 상태의 항목을 본 주체(워커, TimerMaster, 게시자)가 CAS로 넘긴다.
- **닫기**: 활성화 시 이전 등록의 미종료 항목을 닫는다(D-17 (c)). 비활성화 시 QUEUED는 CANCELLED, RUNNING은 D-17 (d)의 규칙을 따른다. 닫기는 마감 경과를 확인하면 MISSED를 함께 설정한다(D-20).
- **세대**: 각 레코드는 `gen`을 가진다. 모니터는 현재 `gen`과 다른 레코드를 건너뛰어 그 상태와 계수를 바꾸지 않는다(D-20 규칙 3, A9). 닫기 sweep은 세대 비교 없이 닫으라고 지시받은 비종료 기록을 모두 닫는다. 활성화 때 닫는 것은 이전 등록의 기록이고, 비활성화 때 닫는 것은 현재 등록의 기록이다(A9).
- **재시작**: 데몬 시작 시 이전 객체는 `magic = 0`, 모든 슬롯 `kind = NONE`으로 바꾸고, 닫기 규칙을 모든 슬롯에 적용하고(마감이 지난 항목에 MISSED), 대기자를 깨우고, 최종 계수를 기록하고, unlink한 뒤 새 `instance_id`로 객체를 만든다. 옛 객체에 붙어 있던 클라이언트는 E_INACTIVE를 받고 `ttsched_init()`으로 새 객체에 붙는다. `seq`는 새 객체에서 다시 시작한다. `(gen, seq)`는 한 객체 안에서 `seq`가 한 바퀴 돌기 전(수락 2^32회 이내)까지 유일하다. 재시작 사이의 유일성은 `gen` 상위 32비트(`instance_id`에서 얻는다)에 기대므로 보장이 아니다. 트레이스 출력은 64비트 `instance_id`를 함께 기록한다(A14).
- **게시 주체**: source마다 게시 주체는 한 번에 하나다. 여러 주체의 동시 게시(다중 게시자 링)는 P1에서 지원하지 않는다(F-13). 두 게시가 겹쳐 생긴 손상은 그 source의 큐에만 미친다.
- **payload**: TIMPANI는 payload를 해석하지 않는다. 식별자와 시각과 함께 같은 레코드에 게시되어, 소비자는 자기 Event의 식별자와 데이터를 함께 받는다. payload에 앱 저장소의 포인터를 넣는 것은 계약 밖이다(F-13). 큰 payload는 명시적 해제 통지와 함께 P2다.
- **P1 한계**: 재등록 경계에서 게시가 진행 중이면 그 도달이 새 등록의 도달 계수에 들어갈 수 있다(재등록마다 최대 1건). 데몬이 다운된 동안 게시는 수락되고 워커는 계속 처리하지만, 마감 감지는 없다(D-20). 게시자가 공개(`tail` 전진) 뒤 (f) 이전에 죽으면 그 Event는 처리되고 감시되지만 `accepted` 계수 1건이 빠지며 다음 게시가 이를 복구하지 않는다. 게시 뒤 `tail` 전진 전에 죽으면 다음 게시가 그 레코드를 복구해 계수하고 워커를 깨운다. 두 경우 모두 다음 게시나 TimerMaster의 깨움이 있을 때까지 워커는 그 Event에 대해 깨어나지 않는다(A8). 따라서 '수락 수 + 거절 수 = 게시 시도 수'는 게시자가 죽지 않은 구간에서만 성립한다. 비활성화가 `kind = NONE`을 쓴 뒤 닫기 sweep이 도달하기 전에 워커가 QUEUED 기록을 가져가면 그 Job은 P1에서 허용되며 정상 기록되고 감시된다(A12).

**대안**
| 대안 | 장점 | 단점 | 판정 |
|:--|:--|:--|:--|
| (i) 앱 소유 payload 큐 + 배치 개수 n + invalidate/clear + `TTSCHED_EVT_RESET` + SpscRing (9209bc5) | 자료구조 익숙 | 두 큐를 맞춰야 한다. 무효화 경쟁. 재시작마다 RESET 필요 | 기각 |
| (ii) 다중 게시자 링과 예약 항목 (검토 1차 제안) | 여러 주체 게시 | 예약 후 멈춘 게시자가 항목을 막는다. 순서 밖 게시가 생긴다. P1은 게시 주체가 하나다 | 기각. P2 F-13 |
| (iii) payload 핸들(앱 저장소 포인터) | 큰 데이터 | 수명과 해제 통지가 계약 밖이다. 잘못된 포인터 위험 | 기각. P2 F-13 |
| (iv) 무제한 큐 | 넘침이 없다 | 실시간과 메모리 상한을 깬다 | 기각 |
| (v) 활성화 시 링 인덱스 초기화 (검토 1차 제안) | 단순 | 게시자가 쓰는 인덱스를 건드린다. 인덱스는 자유 실행이다 | 기각 |
| (vi) 보고 전까지 링 재사용을 막는 미보고 비트 (검토 2차 제안) | 보고 누락이 없다 | 데몬이 다운되면 데이터 경로가 멈춘다. 계수는 필수 경로, 상세는 최선 노력 | 기각 |
| (vii) 게시 상태를 PENDING으로 표시 (검토 2차 제안) | 미확정 게시를 구분 | 게시자가 죽으면 상태가 남는다. 판정 경로가 늘어난다 | 기각. 롤백 루프로 처리 |

**결과**: 큐 상수와 SHM 레이아웃은 승인 대상이다(D-03). 데몬 다운 중 마감은 감지되지 않는다(D-20, F-14).

---

### D-20. 완료를 기다리지 않는 마감 감지: 도달 기준 절대 마감, DeadlineMonitor — P1

**결정**
1. **마감**: 절대 마감은 $t_{dl} = t_{arr} + D$이며 수락 시 고정된다. 활성화, 보충, 재개, 재등록 때문에 뒤로 옮기지 않는다. **활성화 시각 + D를 대체 마감으로 쓰지 않는다.**
2. **감시 시작**: 감시는 첫 수락 시점부터 시작한다. 워커가 시작되기를 기다리지 않는다. 수락은 관리되는 워커가 있을 때만 일어난다(D-17 (c)).
3. **감지 주체**: TIMPANI-N의 신규 **DeadlineMonitor** 스레드다. SCHED_FIFO이며 후보 우선순위는 97로, TimerMaster(99) 아래다(Replenisher는 98이다, A1). 비격리 CPU에 둔다. 힙, 블로킹 I/O, gRPC를 쓰지 않는다. 클록은 CLOCK_MONOTONIC이다(TT 경로의 CLOCK_REALTIME 사용은 바꾸지 않는다). 주기 `DL_SCAN_PERIOD_US`의 후보값은 1000 µs다. 매 주기 시계를 한 번 읽고, 모든 슬롯의 모든 링 항목을 `kind`와 무관하게 상태로 골라 검사한다. 현재 `gen`과 다른 항목은 건너뛴다(A9). QUEUED 또는 RUNNING이고, MISSED가 없고, $now > t_{dl}$인 항목에는 MISSED를 CAS로 설정하고 `missed`를 올린 뒤, 미스 레코드 `{slot, gen, seq, t_arr, t_dl, t_detect, state_at_detect, throttled}`를 FaultMonitor에 고정 크기 SPSC 큐로 넘긴다. 넘치면 계수한다. 감지 때에만 서버의 `CbsState`를 한 번 읽어(비블로킹 BPF 맵 조회) `throttled`를 채운다.
4. **감지 지연**: $t_{dl}$에서 MISSED 설정까지의 지연은 주기와 깨움 지터를 합한 값 이하다. 전제는 그 CPU에 상위 우선순위 부하가 없다는 것이다. 후보 상한은 2 ms다. 보고 지연(MISSED 설정에서 TIMPANI-O의 FaultInfo까지, 보내진 보고에 한한다)은 별도 바운드이며 후보는 200 ms다. **두 값 모두 합의·측정이 필요하다.** 지연 0을 보장한다는 뜻이 아니다.
5. **Job당 한 번**: MISSED는 CAS 한 번으로 설정하며, 설정한 주체가 `missed`를 올린다. 설정 주체는 다음 중 하나다. DeadlineMonitor, 워커의 종료 전이, 닫기 sweep(TimerMaster의 닫기와 데몬 시작 정리), 워커가 대기 중 수행하는 stale 세대 정리. 게시 롤백은 이미 설정된 MISSED를 유지하며 새로 설정하지 않는다(A11). `missed` 증가는 CAS 뒤에 실행되므로 활성화 기준값 이후에 들어갈 수 있다(바인딩당 최대 1건, A9). 이후의 완료는 완료 기록으로 연결하고 실패를 다시 계수하지 않는다. MISSED는 어떤 나중 전이로도 지워지지 않는다.
6. **완료 시각과 경합 규칙(해석, 확인 필요)**
   - $t_{end}$는 워커가 `done()`에서 종료 전이를 게시하기 직전에 읽는 시계 값이다. 게시가 곧 완료다. 완료는 워커가 선언된 처리를 끝내고 결과를 게시한 시점이다. 네트워크 전달이나 액추에이션의 완료를 뜻하지 않는다. IRQ/ISR, 미들웨어 내부 스레드, 네트워크 경로의 시간은 TIMPANI가 예산으로 잡지 않는다(REQ-008 §7 범위 외, A21).
   - 워커의 전이 CAS가 감시의 MISSED 설정과 경합하면 전이는 실패한다. 워커는 시계를 다시 읽고(이 값은 $t_{dl}$ 이후다) 재시도한다. 그러므로 기록된 $t_{end}$와 MISSED 플래그가 어긋나지 않는다.
   - 정시(마감 이전)에 게시된 완료는 수집이 늦어도 미스가 아니다. 판정은 기록된 $t_{end}$로 한다.
   - 허용되는 오차: 정시로 기록된 Job이 시계 읽기와 전이 사이의 선점으로 $t_{dl}$ 직후에 게시될 수 있다. 그 게시는 $t_{dl}$ 뒤 첫 감시 주기 이전에 끝난다(감시 주기 뒤에 게시되는 일은 없다).
   - 가드 밴드와 잠정 판정은 채택하지 않는다. 완료를 게시로 정의하고 시계를 다시 읽는 방식이 같은 목적을 판정 번복 없이 달성한다.
7. **종료 규칙(해석, 확인 필요)**: 마감 이전의 종료(COMPLETED, DISCARDED)와 마감 이전 TIMPANI의 닫기(CANCELLED, ABANDONED)는 미스가 아니다. 이들은 별도 결과로 계수하며 성공으로 세지 않는다. 마감 이후의 어떤 종료(늦은 완료, 늦은 폐기, 취소, 중단)도 미스다. 이미 설정된 미스는 나중의 종료가 지우지 않는다.
8. **실패의 후속 처리**: 마감 실패는 처리 상태와 별개의 결과다. 실패 감지는 Job의 자동 종료를 뜻하지 않는다. P1에서 워커는 실패한 Job도 끝까지 처리하고 결과를 기록한다. 실패한 Job을 계속할지 취소할지는 별도 정책이며 개방 이슈다.
9. **데몬 다운 중(P1 한계)**: 데몬이 죽어 있는 동안 기존 객체는 `kind = EVT`로 남는다. 게시는 수락되고 워커는 계속 처리한다. 워커의 종료 전이는 늦은 완료를 판정한다. 그러나 큐에서 또는 실행 중에 만료된 Event는 실시간으로 감지되지 않는다. 다음 데몬 시작의 정리 sweep에서 DETECTED_LATE로 찾아 최종 통계에 기록한다. P1에는 데몬 하트비트가 없다(F-14).
10. **보고와 감지의 분리**: 감지(DeadlineMonitor)와 보고(FaultMonitor, 계수 경로와 상세 경로)는 D-10에 따라 분리된다.

**대안**
| 대안 | 판정 |
|:--|:--|
| (i) 활성화 기준 마감 $t_{act} + D$ (9209bc5) | 큐 대기만큼 마감이 뒤로 밀린다. 큐 지연이 보이지 않는다. 기각 |
| (ii) 완료 시점에만 판정 (BPF `stopping`/`quiescent`) | 큐 대기, 스로틀, CPU 유휴 중 감지가 안 된다. 완료가 없으면 판정도 없다. 기각 |
| (iii) 워커가 Job 시작 때 타이머 설정 | 워커가 스케줄되지 않으면 동작하지 않는다. 대기 중에는 감시가 없다. 기각 |
| (iv) 완료와 감시의 경합을 가드 밴드나 잠정 판정으로 처리 | 잠정 판정은 번복이 필요하다. 가드 폭은 측정에 묶인다. 기각 (규칙 6) |
| (v) 미확정 게시를 PENDING 상태로 표시 | 게시자가 죽으면 상태가 남는다. 판정 경로가 늘어난다. 기각 (롤백 루프로 처리) |
| (vi) DeadlineMonitor를 Replenisher 스레드와 합치기 | 역할과 주기가 달라 우선순위가 충돌하고, 보충 경로가 감시 지연에 묶인다. P1에서 기각. 측정 후 F-12 |
| (vii) Event마다 hrtimer 또는 timerfd 설정 | 레코드마다 타이머를 관리하고 할당과 취소 비용이 든다. TimerMaster 부하가 늘어난다. 기각. 이벤트 기반 깨움은 F-10 |
| (viii) BPF에서 마감 판정 | BPF는 큐 대기 중인 Job을 깨우지 못하고 유휴 CPU에서 실행되지 않는다. 기각 |
| (ix) 데몬 하트비트로 다운 중 감지 | P1 범위 밖. P2 F-14 |

**결과**: 감지 지연과 보고 지연은 측정 전까지 후보값이다. DL_SCAN 비용과 지터는 PoC 항목이다(F-6, F-10). BPF는 마감 판정 코드를 잃고 단순해진다. 두 해석(규칙 6과 7)은 개방 이슈다.

---

### D-21. 예산 전용 L2 태스크 class (Event 미게시) — P1

**결정**: Event를 게시하지 않는 L2 태스크(예산 전용 class)는 CBS 예산 집행, CPU 고정, `BUDGET_EXCEED`, 실행 구간 레코드를 받는다. Event, MIT 검사, DMISS 감시는 없다. 선언한 $D$는 **admission 파라미터**다. 커널 활성화를 기준으로 하며 감시 대상이 아니다.

- **구분 방식**: 설정 플래그가 없다. TIMPANI의 어떤 동작도 class에 의존하지 않는다. source가 Event 연결을 쓰는지는 Event가 게시되었는지로 관측된다. 게시가 없으면 DeadlineMonitor가 판정할 대상이 없다.
- **보장 문구**: 예산 전용 태스크를 "Event별 도달 기준 마감을 지원한다"고 설명하지 않는다. 표준 블로킹을 쓰는 앱이 이 class에 해당한다(FR-L-003).
- **admission**: D-09의 같은 식을 쓴다. $L_{post}$ 부분은 이 class에서 여유로 남는다.

**근거**: 명시적 연결 없이 커널이 관측하는 것은 실행 구간뿐이다. 식별자와 도달 시각이 없으므로 Event 단위 마감 감시를 줄 수 없다. 그러나 예산 집행과 CPU 고정은 실행 구간만으로 충분하다(FFI 요구는 유지된다).

**대안**
| 대안 | 판정 |
|:--|:--|
| (i) 클래스별 별도 admission 공식 | TIMPANI-O는 클래스를 알 수 없다. 기각 |
| (ii) proto에 클래스 필드 추가 | proto 변경을 늘린다. 관측으로 충분하다. 기각 |
| (iii) 커널 wakeup을 Event로 취급 (9209bc5 어댑터 K) | 식별자와 도달 시각이 없다. 연속 Job이 병합된다. 기각 |
| (iv) 예산 전용 태스크를 등록 거부 | 기존 L2 CBS 사용을 깬다. 기각 |

---

### D-22. CBS-only 실행: TT 발화 및 TT hyperperiod와의 독립 — P1

**근거:** [2026-11-12 Weekly 논의와 후속 답변](https://github.com/eclipse-timpani/timpani26/discussions/4). CBS는 Linux `SCHED_DEADLINE`이 아니라 `SCHED_EXT` 안에서 집행한다. CBS-only에서도 Event 수락, CPU별 dispatch, 예산 보충, DeadlineMonitor 감시는 동작해야 한다.

- TT 슬롯이 없는 CPU의 TT 점유는 0이며 가용 Gap은 전체 시간이다. D-09에서 `B_tt(k, D) = 0`으로 검사한다. CPU별 L2 수 제한과 다른 서버 간섭·이용률 검사는 유지한다.
- CBS 보충은 서버별 `T_s`와 D-04/D-07의 윈도우·재스케줄 유발로 동작한다. TT_START/TT_END가 발생해야 보충·dispatch되는 의존성을 두지 않는다.
- **CBS 실행 자체에 공통 hyperperiod는 필요하지 않다.** MIT는 도달 제약이고 `T_s`는 보충 주기다. MIT의 LCM이나 임의 기본값으로 TT hyperperiod를 만들어야 한다고 결정하지 않는다.
- 테이블 전송·검증·갱신 코드가 양수 `hyperperiod_us`를 요구하는지는 별도 호환성 확인 대상이다. TT가 없는 테이블의 표현(0 허용 여부 포함), 갱신 적용 시점, TT 최초 추가·최종 제거는 [DDR-011 §1.2](DDR-011-runtime-table-update.md#12-2026-11-12-weekly-연계-검토--제안미구현)에서 정의한다. 표현과 적용 경계는 구현 전 확정해야 하며 이 결정이 필드 변경 승인은 아니다.
- **P1 검증:** TT 없는 노드와 혼합 노드의 TT 없는 CPU에서 첫 Event 처리, 유휴 후 재도달, 예산 소진 후 백업 보충을 확인한다. TT 추가·제거가 기존 CBS 예산과 미완료 Job을 초기화하거나 마감을 바꾸지 않는지 검사한다.

### D-23. 마감 실패 사실과 원인·복구 정책의 분리 — P2

**결정 방향:** TT 초과 실행으로 영향을 받은 L2의 마감 실패 사실은 지우지 않되, 그 사실만으로 L2 자체의 결함이라고 단정하거나 자동으로 불이익을 주지 않는다. 회의에서 말한 원인 분석은 별도 설계·검증 대상이지 이미 구현된 지능형 판정이 아니다.

1. **P1에서 유지하는 기본 관측:** Job별 `MISSED` 감지·계수·기본 보고와 기존 D-10/D-16/D-20의 관측 정보를 제공한다. 이는 마감 실패의 사실이며 원인 확정이 아니다. 새로운 TT 상관 분석이나 원인별 복구 정책은 P1 완료 조건이 아니다.
2. **P2 정책 설계:** 현재 D-10의 임계 초과 보고가 [DDR-012 §4.4](DDR-012-fault-management.md#44-2026-11-12-weekly-후속--피해-태스크와-원인-태스크-분리-검토용)의 복구 정책과 결합할 때, TT 간섭의 피해 CBS를 자체 결함으로 자동 귀책하지 않도록 원인·피해 태스크와 대응을 정의한다. P1의 기존 보고·복구 경로는 이번 단계 분리로 변경하지 않으며, 원인 인지형 복구를 제공한다고 설명하지 않는다. 모든 miss의 계수와 외부 보고 범위·지연은 P1 기본 계약의 별개 개방 이슈로 유지한다.
3. **P2 분석 확장:** TT 실행 구간·슬롯 종료 지연·Timer Master 지터·CBS 실행·Job 기록을 연계한 원인 분류, 다중 원인 분석 및 자동 복구 선택을 검토한다. 같은 시각의 TT overrun·`THROTTLED`·MIT 위반만으로 인과를 확정하지 않는다. 구체적인 알고리즘과 일정은 미확정이며 신규 AI 의존성이나 proto 필드를 승인하지 않는다.
4. **보장 범위:** TT 경계 보호는 P2의 DDR-007 §4.3.1 담당이다. 그 검증 전 D-09는 TT가 선언된 실행 범위 안에 있다는 조건부 보장이다. TT 종료 강제와 원인 분석은 P1 기본 환경의 완료를 막지 않지만, TT overrun까지 포함한 시간 격리가 P1에서 보장된다고 주장하지 않는다.

### D-24. 애플리케이션 추상화와 실행 환경 전제 — P1

- **P1:** 앱의 입력 어댑터·라이브러리 래퍼 뒤에 TIMPANI 의존성을 감출 수 있다. 코어는 SOME/IP 의미를 모르며, 워커는 공통 Event/Job 계약을 사용한다. 단, 어떤 계층에서는 도달·Event 식별·Job 시작/종료를 명시해야 한다. 임의 앱의 커널 wakeup만으로 같은 정보를 자동 추론하지 않는다(D-01~D-03, D-18).
- **후속 조사:** 앱 수정이 전혀 없는 완전 투명 연결은 P1 보장이 아니다. 래퍼나 미들웨어 훅을 선택하더라도 관측 정확도·비용·실패 처리를 별도 검증한다.
- **P1 환경 확인:** L1/L2와 수신·vsomeip 스레드의 PID/TID, 이름, 실제 CPU, 정책, 허용 마스크와 cgroup을 구분한다. `sched_setaffinity`는 `cpuset.cpus.effective` 밖으로 나가게 하지 못하며 root도 이 제약을 우회하지 못한다. `EINVAL` 하나만으로 원인을 확정하지 않고 online CPU·마스크·대상 cgroup을 함께 확인한다.
- cgroup/cpuset 배치는 [DDR-003 §2](DDR-003-interface-protocol.md#2-컨텍스트)의 시스템·Pullpiri 책임이다. 같은 프로세스의 워커와 미관리 스레드를 나누려면 cgroup 구성과 스레드별 이동 가능성부터 확인한다. BPF CPU 배정이나 우선순위 변경이 이 구성을 대신하지 않는다.
- 미관리 스레드를 Non-Isolated CPU로 옮기거나 `SCHED_FIFO`로 올리는 것은 일괄 처방이 아니다. affinity 변경과 cgroup 이동을 구분하고, 수신 지연뿐 아니라 Timer Master·Replenisher·DeadlineMonitor에 대한 간섭과 starvation 위험을 검증한다. 기존 지연 후보값을 이 환경에서도 자동 보장하지 않는다.

---

## 3. 결과 (Consequences)

### 3.1 긍정
- Event, Job, 마감, 결과의 정의가 명확하고 1:1로 식별된다. 검증 프레임워크가 레코드의 합부를 판정할 수 있다.
- 연속 Job, 처리 중 도달, 블로킹이 서로 다른 기록으로 구분된다.
- 완료를 기다리지 않는 마감 감지가 큐 대기, 스로틀, CPU 유휴 중에도 동작하도록 설계되었다(측정 대기).
- 선언을 지키는 태스크는 스로틀되지 않는다(P-a의 전제가 성립할 때). 보충 지연이 바운드되어 "이벤트 없는 CPU의 기아"가 사라진다.
- L2가 배정 CPU에 고정되어 DDR-007의 CPU별 Gap 분석이 유효해진다.
- 코어가 미들웨어 중립이다. 앱은 입력 어댑터를 통해 같은 계약을 쓴다.
- proto는 필드 추가만으로 호환을 유지한다.
- 필수 통계가 트레이스 설정과 무관하다.

### 3.2 부정 / 비용
- `libttsched` v2와 SHM의 ABI 단절이 있다(재컴파일). 공개 API, SHM 레이아웃, 큐 상수는 승인 대상이다(D-03, D-19).
- 워커는 `done`을 호출해야 한다. 입력 어댑터를 앱이 구현해야 한다.
- BPF 변경(실행 구간 상태 머신, CPU별 DSQ, 폴트 전이)과 신규 스레드(DeadlineMonitor, Replenisher)가 추가되어 timpani-n 검증 범위가 넓어진다. PoC(OI-9, 모니터 PoC)가 필요하다.
- 승인식의 수용률이 낮아진다. $\delta_{ovr}$과 $L_{del\_max}$를 넣었기 때문이며, 다중 L2 구성에서 특히 그렇다(P2 EDF, HRTICK 검토).
- 블로킹하는 Job은 마감이 감시되지만 admission이 보장하지 않는다.
- 데몬이 다운된 동안에는 Event 마감이 실시간으로 감지되지 않는다. 하트비트가 없다(F-14).
- 큐 상한 QDEPTH를 넘는 도달은 넘침으로 거절된다. 상한 값은 측정이 필요하다.
- MIT를 위반하거나 예산을 초과한 서버는 윈도우가 도달 위상보다 뒤로 밀리고, 윈도우가 끝난 뒤 첫 활성화나 승격까지 그 태스크 자신의 응답 시간이 늘어난다(D-04 "위반 후 동작"). 도달이 계속 윈도우 안에 떨어지면 복귀가 늦어지며, 그 지연은 측정 항목이다.
- L2 `task_id`마다 SHM 슬롯을 데몬 수명 동안 고정 배정하므로, 이름이 자주 바뀌는 워크로드는 슬롯을 소진할 수 있다.
- 설계 단계이며 구현되지 않았다. 설계 가정과 수치는 측정 전까지 후보다.

### 3.3 후속 결정 필요 (P2 또는 OI)

| # | 항목 | 문서 |
|:--|:--|:--|
| F-1 | OI-9: 백업 보충의 재스케줄 유발 수단 PoC(BPF SYSCALL + `scx_bpf_kick_cpu` 가용성) | 본 DDR Accepted 조건 |
| F-2 | DDR-007 §4.5(주기 격자 보충 서술, Replenisher의 맵 갱신)를 D-04와 D-07에 맞춰 정합 갱신 | DDR-007 |
| F-3 | **철회(2026-11-11 개정).** 수면 경계의 시스템 콜 분류는 명시적 Job 경계(D-18)로 대체되어 항목을 뺀다. 남은 P2 항목은 EDF 전환(D-08)과 정밀 승인식(D-09)에서 추적한다 | D-18, D-08, D-09 |
| F-4 | DDR-002 §7, DDR-005 User App Interface(D-14), DDR-011 증분 갱신, DDR-012 비동기 폴트 전달의 정합 갱신 | 각 DDR |
| F-5 | `doc/docs/structure.md`의 Rust 전용 개발 서술 갱신 | docs |
| F-6 | TIM 후보값 확정(RPi5 측정): TIM-001/002(L_wake), $L_{post}$, $\delta_{ovr}$, $DL\_SCAN\_PERIOD\_US$, 감지·보고 지연 | 테스트 전략 |
| F-7 | 태스크당 다중 Event source의 명시적 선언(REQ-008 OI-16), 앱 측 입력 어댑터 공통 지침 | DDR-008 개정 |
| F-8 | **철회(2026-11-11 개정).** 마커 wait point의 커널 식별(`sched_yield()` 표식과 BPF `ops.yield`)은 명시적 Job 경계(D-18)로 대체되었다. 식별 자체가 P1에서 필요 없다 | D-18, D-02 |
| F-9 | 위반 후 윈도우 복귀 정책: 승격 윈도우를 Job의 활성화 시각에 맞추는 대안(D-04 대안 표)의 측정과 재검토 | DDR-008 개정 |
| F-10 | DeadlineMonitor의 이벤트 기반 깨움: 주기 스캔으로 TIM-009 바운드를 달성하지 못할 때 도입 | DDR-008 개정(D-20) |
| F-11 | source별 큐 깊이와 payload 크기를 워크로드 선언에 담기(proto 변경, 승인 필요) | DDR-008, proto |
| F-12 | DeadlineMonitor와 Replenisher 스레드 병합 가능성(우선순위와 주기를 측정한 뒤) | D-07, D-20 |
| F-13 | 다중 게시 주체, payload 핸들과 해제 통지, 큰 payload | DDR-008 개정(D-19) |
| F-14 | SHM의 source별 격리, 데몬 하트비트(다운 중 감지) | DDR-008 개정(D-20) |

### 3.4 개방 이슈 (승인·측정·확인 필요)

- **수치 후보값(합의·측정 필요)**: `TTSCHED_EVT_QDEPTH` 8, `TTSCHED_EVT_PAYLOAD_MAX` 64 B, `DL_SCAN_PERIOD_US` 1000, 감지 지연 상한 2 ms, 보고 지연 상한 200 ms, $L_{post}$ 50 µs, $L_{del\_max}$ 100 µs, $\delta_{ovr}$ 1 ms(HZ=1000), `tol` 50 µs, `tol_mit` 0, SHM 슬롯 수 64, 객체 크기 상한 256 KiB, DeadlineMonitor 우선순위 97(Replenisher 98), back-off 10 ms.
- **승인**: 공개 API(`ttsched_evt_*`), SHM v2, 큐 상수, 모든 protobuf 변경(D-03, D-11, D-19).
- **PoC**: DeadlineMonitor의 1 kHz 스캔 비용과 지터(TT 부하와 선택한 CPU 조건), 이벤트 기반 깨움의 필요성(F-10, F-6).
- **해석 확인 1**: 완료 시각 $t_{end}$의 정의와 감시와의 경합 규칙(D-20 규칙 6). 독자가 "실제 완료 시각 기준 판정"을 이렇게 읽는지 확인한다.
- **해석 확인 2**: 마감 이전에 TIMPANI가 닫은 항목(CANCELLED, ABANDONED)은 미스가 아니다(D-20 규칙 7). P1-008 §2의 "마감 전 명시적 폐기·취소" 문장을 TIMPANI 닫기에 확장한 해석이다.
- **실패한 Job의 계속·취소 정책**(D-20 규칙 8).
- **OI-9 PoC**(F-1)와 **DDR-007 정합**(F-2).

---

### 3.5 2026-11-12 Weekly 반영 및 Phase 판정

**2026-11-13 범위 합의:** P1은 공통 Event/Job을 정의하고 TIMPANI가 수락·전달·처리·종료·마감 감시할 수 있는 Event-Driven Workload의 기본 환경을 구현하는 단계다. 공통 큐·식별·복구 계약, CBS 예산 집행 및 TT 없는 실행도 이 환경에 포함한다. TT 슬롯 경계 계약의 구체화·종료 집행과 CBS 마감 실패 원인 분석·원인별 복구는 P2로 분리한다. 기존 P1의 도달 기준 마감 감지·계수·기본 보고는 축소하지 않는다.

**출처:** [Discussion #4의 Weekly 2026-11-12 및 seokheehan 후속 답변](https://github.com/eclipse-timpani/timpani26/discussions/4). 안건의 질문 자체를 승인된 해법으로 취급하지 않는다. 아래 P1/P2는 **Event-Job 지원의 단계**이며 다른 DDR의 과거 개발 Phase 번호와 다르다. 이 표는 범위 제안이며 Project Leader의 일정·구현 승인과 타깃 검증을 대신하지 않는다.

| 회의 항목 | P1 기본 환경 / 보장 전제 | P2 또는 범위 외 | 담당 |
|:--|:--|:--|:--|
| 1-a TT 슬롯 종료 강제 | TT 정상 실행 가정하의 기본 Event/CBS 환경을 검증. TT 초과까지의 격리는 보장하지 않음 | **P2:** 슬롯 범위·종료 집행·허용 지연·미완료 TT 정책과 혼합 시간 격리 검증 | DDR-007 §4.3.1(주관), DDR-005 |
| 1-b TT 초과 때문에 CBS가 마감을 놓침 | 마감 실패 감지·계수·기본 보고를 유지하며 원인 확정을 주장하지 않음 | **P2:** TT 간섭과 CBS 자체 결함 구분, 피해 태스크 보호 및 원인별 복구 정책 | D-23, DDR-012 §4.4 |
| 1-c CBS 정책과 Timer Master 선점 | CBS는 SCHED_EXT, Timer Master는 SCHED_FIFO. DEADLINE 계층·fair server 간섭을 P1 측정 전제로 확인 | Timer Master를 SCHED_DEADLINE으로 바꾸는 결론 없음. 근본 대책은 커널·플랫폼 조사 | DDR-005의 실행 환경 보충, DDR-006 §4 |
| 1-d DMISS 원인 정보 | 기존 기본 관측·통계 유지 | **P2:** TT/CBS 기록 상관 분석, 원인 분류·지능형 분석·원인별 자동 복구 | D-23, D-10/D-16, DDR-012 |
| 1-e 앱의 TIMPANI 의존성 | 어댑터·래퍼로 의존성 국소화, 명시적 Event/Job 연결 유지 | 앱 완전 무수정·자동 추론은 조사 대상 | D-24 |
| 1-f TT 없는 CBS 실행 | P1 필수. 보충·dispatch·감시는 TT와 독립. CBS 공통 hyperperiod 불필요 | 테이블 표현·갱신 경계는 구현 전 확정 | D-22, DDR-011 §1.2 |
| 1-g L1→L2 데이터 파이프라인 | Radar L1→Event→FCW L2를 P1 연결 예제로 검증 가능. 기존 L1/L2 분류는 유지 | 전체 파이프라인의 E2E 마감·관측·노드 간 보장은 DDR-009 주제이며 008 P1 보장 밖. 현재 체크아웃에 DDR-009 정본 없음 | HLD-008 §8, 향후 DDR-009 |
| 2 vsomeip 정책·CPU 이동 실패 | PID/TID·cgroup·effective cpuset·정책·online CPU·컨테이너 설정 증거 수집. 환경 검증은 P1 | 무조건 FIFO 승격·root로 제약 우회 금지. 시스템 배포 구성의 소관 | D-24, DDR-003 §2 |

**추가 검증 시나리오(미실행):** P1은 CBS-only 유휴 후 보충, 기본 Event/Job 생명주기·도달 기준 마감 감시, 잘못된 cpuset 배치의 명시적 실패와 L1→L2 연결 예제를 검증한다. 제어 스레드 지터는 실행 환경 전제로 측정한다. P2는 TT 경계 초과 시 다음 TT·CBS 보호와 피해 기록·원인 분류를 검증한다. L1→L2 예제를 수행해도 전체 E2E 마감 보장이 검증된 것은 아니다.

**문서 정합 대기:** D-22~D-24는 회의 반영 범위와 선행 조건을 추가한다. 신규 REQ/SRS ID, HLD/LLD 알고리즘, API/proto 및 기존 D-20 완료 경합 해석을 이번 변경으로 확정하지 않는다. 하위 체인 갱신은 별도 검토 대상이다.

---

## 4. 요구사항 대응표 (Decision → Requirement)

| 결정 | 충족 REQ-008 | 충족 SRS-008 |
|:--|:--|:--|
| D-01 | MOD-002/006/007/008/009/010/011, IF-005 | FR-G-001/002, FR-G-003, FR-L-002/006 |
| D-02 | MOD-003, DET-001/002/005 | FR-B-001~005, FR-B-024 |
| D-03 | DET-003/004, DET-007, IF-005 | FR-L-001~005, FR-L-009~012, IF-L-001~004 |
| D-04 | PRT-001/007/008, CBS-004/006, ORD-003 | FR-B-013/014/015/017 |
| D-05 | PRT-004/007, MOD-010 | FR-B-017, FR-B-024, FR-L-007, FR-N-014 |
| D-06 | CBS-005 | FR-B-010, TMG-010 |
| D-07 | PRT-003, TIM-003 | FR-N-010/011, TMG-003 |
| D-08 | ORD-002 | FR-B-016 |
| D-09 | CBS-001/004, ORD-001/003, MOD-004, A-03, PRT-008, TIM-001/002 (AC-008-16) | FR-O-003/010~015 |
| D-10 | PRT-005/006/007/009, IF-004 (AC-008-14) | FR-B-022/023, FR-N-012~015, FR-N-031, FR-O-020 |
| D-11 | IF-001/003/004, NFR-005 | IF-PO-001/004/005/006, IF-ON-001~004/006 |
| D-12 | DET-006, TIM-006 | FR-N-003/004/007 |
| D-13 | (범위 외 명시) | — |
| D-14 | DET-003 | FR-L-003 |
| D-15 | NFR-003 | NFR-006/007 |
| D-16 | OBS-001/002, OBS-004, TIM-008 | FR-N-020~024/026, FR-N-030, IF-V-001~004 |
| D-17 | RTU-001/004 (AC-008-15) | FR-O-005, FR-N-005/006, FR-N-029 |
| D-18 | MOD-009, DET-007 (AC-008-13) | FR-L-010, FR-L-011, FR-B-024 |
| D-19 | MOD-010, IF-007 (AC-008-15) | FR-L-009, FR-L-012, FR-N-029 |
| D-20 | PRT-008, PRT-009, TIM-009, TIM-010, TIM-011 (AC-008-14) | FR-N-027, FR-N-028, FR-N-031, TMG-014, TMG-015, TMG-016 |
| D-21 | MOD-011 | FR-G-003, FR-L-003 |
| D-22 | CBS-only 동작 및 테이블 경계 정합: 요구사항 추적 보완 필요 | FR-O/FR-N 및 갱신 수용 기준 보완 필요 |
| D-23 | P2 귀책·원인별 복구: 요구사항 추적 보완 필요. 기존 P1 관측 유지 | DDR-012와 P2 원인 분석·복구 수용 기준 합의 필요 |
| D-24 | MOD-006/011, IF-005의 추상화·환경 전제 명확화 | 연결·배포 환경 수용 기준 보완 필요 |

---

## 5. 영향 받는 컴포넌트

| 컴포넌트 | 변경 요약 (상세: LLD-008) |
|:--|:--|
| `timpani-o/proto/schedinfo.proto`, `node_control.proto` | D-11 필드 추가(모두 승인 필요) |
| `timpani-o/src/schedinfo_service.cpp` | MIT 필드 읽기와 전환 해석, 파라미터 검증(`D ≤ MIT`, `T ≤ MIT`), 거부 시 복원 |
| `timpani-o/src/global_scheduler.{h,cpp}` | `mit_us` 분리, $B_{tt}$ 계산, D-09 승인식(`L_del_max`, `δ_ovr`), min gap 거부, hyperperiod 불일치 수정, EVENT_DRIVEN 레이어 발행, CPU 명시 |
| `timpani-o/src/orchestrator_service.cpp`, `fault_client.cpp` | 폴트 유형과 플래그의 보존 매핑, 신규 필드 전달 |
| `timpani-n/src/bpf/maps.h`, `timpani.bpf.c` | TaskMeta 확장(실행 구간 상태, `bind_gen`), CPU별 DSQ, 실행 구간 상태 머신, BUDGET_EXCEED 전이 발행, `job_trace_rb`, `trace_enabled`. activation 기준 MIT 검사, L2 DMISS, `MIT_VIOLATION` 발행은 제거 |
| `timpani-n/src/main.cpp`, `bpf_loader.{h,cpp}` | EVENT_DRIVEN 레이어 처리, CBS 등록에 MIT와 CPU, 상태 보존 재등록, 활성화와 비활성화 순서 |
| `timpani-n/src/task_registry.{h,cpp}` | D-12 부활(미해결 목록, 재스캔, 소멸 감지), `bind_gen` 확인 |
| `timpani-n/src/replenisher.{h,cpp}` (신규) | D-07. 서버 상태를 쓰지 않는 재스케줄 유발 |
| `timpani-n/src/deadline_monitor.{h,cpp}` (신규) | D-20. SCHED_FIFO 스레드, 주기 스캔, MISSED CAS, 미스 레코드 SPSC 전달, 감지 시 `CbsState` 조회 |
| `timpani-n/src/job_trace.{h,cpp}` (신규) | D-16. Job log 수집, 실행 구간 레코드 수집, Job 기록 출력 |
| `timpani-n/src/event_source_monitor.{h,cpp}` (신규) | D-03, D-05, D-16. SHM 누적 카운터 폴링, 활성화 기준값, 필수 통계 출력, MIT 증분에서 `MIT_VIOLATION` 생성 |
| `timpani-n/src/fault_monitor.{h,cpp}` | 계수 경로(`missed`, `mit_viol` 증분)의 임계 규칙, 상세 경로의 `(task, gen, seq)` 중복 제거, 플래그 채움 |
| `timpani-n/src/timer_master.{h,cpp}` | SHM v2, 슬롯 할당, 활성화와 비활성화와 시작 정리(닫기 sweep), `gen` 생성, 지터 샘플 트레이스 출력 |
| `sample-apps/src/libttsched.{h,c}` | SHM v2, `ttsched_evt_open/post/wait/done`, `ttsched_init()` 재부착, back-off |
| `examples/someip/*` | `libtimpani_rt` 축소와 폐기. 입력 어댑터를 `ttsched_evt_post()` 호출로 축소(REQ-008 §12) |
| `scripts/mock_pullpiri.yaml`, `timpani-o/scripts/grpcurl/*.json` | MIT 필드 사용(기존 필드 유지) |
| 문서 | DDR-002 §7, DDR-005, DDR-006(RT 스레드 표에 Replenisher와 DeadlineMonitor), DDR-007 §4.5와 §7, DDR-011, DDR-012, `structure.md` 정합 |

---

## 6. 기존 DDR과의 정합

| DDR | 영향 |
|:--|:--|
| DDR-001 | 없음(L2 = Sporadic+Safety → CBS 그대로). `TaskSpec.min_inter_arrival_us` 설계를 proto에 실현 |
| DDR-002 | §7 이중 모드 → "미채택(DDR-007/008)"로 갱신 |
| DDR-003 | `TaskSpec` MIT 필드와 FaultType, FaultInfo 확장 반영 |
| DDR-004 | §8.C CBS 예산 계산에 D-09 승인식(`L_del_max`, `δ_ovr`) 추가 |
| DDR-005 | User App Interface: L2의 `ttsched_wait_next_period` 필수를 D-14대로 개정. 워커의 `ttsched_evt_wait/done` 쌍(D-18). TaskMeta 확장과 `bind_gen`. DSQ 레이아웃(CPU별) 갱신 |
| DDR-006 | RT 스레드 표에 Replenisher와 DeadlineMonitor 추가(DDR-007 §10 예고 사항 실현). 두 스레드 모두 gRPC와 할당 금지 |
| DDR-007 | §4.5 보충 의미론을 활성화 기준 윈도우로(D-04), §4.5.3 Backup 필수화(상태는 BPF만 갱신, D-07), §4.2 DSQ 레이아웃 CPU별, §7 Fault 표에 MIT_VIOLATION(도달 기준 원천, D-10) |
| DDR-011 | P1 영향 없음. P2 증분 갱신 시 개정 |
| DDR-012 | 폴트 계수 경로와 상세 경로 분리(D-10). DMISS 임계 규칙 유지. TT DMISS 경로는 변경 없음 |

---

## 7. 개정 이력 (Revision History)

| 버전 | 날짜 | 변경 내용 |
|:--|:--|:--|
| v1 | 2026-11-10 | 최초 Proposal. 커밋 `9209bc5` 기준 설계 결정 D-01~D-17. 버전 표기 없이 작성됨 |
| v2 | 2026-11-11 | P1 Event-Job 계약 보강(Issue #53, [P1-008](P1-008-event-job-contract-amendment.md))에 맞춰 정렬. D-01, D-02, D-03, D-04, D-05, D-09, D-10, D-11, D-14, D-16, D-17 개정. D-07과 D-08은 용어와 스레드 정합만 보완. D-18~D-21 신규. F-3, F-8 철회, F-10~F-14 신규. 요약, 결과, 대응표, 영향 컴포넌트, 정합 표를 갱신. 설계 단계이며 구현과 검증은 없다. 검토 반영(2026-11-11): 큐 점유 상한의 의미, 게시자 사망 시 계수 한계, MISSED 설정 주체, 세대 비교, done의 seq 확인, 윈도우 예산 복귀 서술, L_del_max 항(TIM-011, TIM-001), proto count 필드와 기존 누적 필드의 구분을 정정했다 |
| v3 | 2026-11-13 | Weekly 2026-11-12(Discussion #4) 반영. D-22~D-24와 §3.5에 CBS-only 독립 실행, 실패 사실과 원인·복구 분리, 앱 추상화·cpuset 환경 전제, P1/후속/타 DDR 책임을 기록. TT 경계 집행은 DDR-007/005, 테이블 경계는 DDR-011, 귀책·복구는 DDR-012에서 검토. 메커니즘·수치·공개 인터페이스 승인은 아님 |
