<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# Event-Driven Workload Support — 앱 개발자 가이드

| 항목 | 내용 |
|:--|:--|
| 상태 | Draft (설계 단계, P1 구현 전) |
| 날짜 | 2026-11-24 |
| 작성자 | Seokhee Han |
| 대상 독자 | L2(Event 구동) 샘플 앱을 TIMPANI 위에 작성하는 애플리케이션 개발자. 008 설계 체인을 읽지 않았다고 가정한다 |
| 기준 문서 | REQ-008 v5.2, SRS-008 v4.2, DDR-008 v3.3, HLD-008 v5.3, LLD-008 v5.2 (설계 기준 커밋 `937ca3c`) |

> **이 문서의 위상**
> - 이 가이드는 **설계 단계** 문서다. 구현이나 실행 검증 결과가 아니다.
> - `ttsched_evt_*` API, SHM v2 레이아웃, 새 워크로드 필드(`min_inter_arrival_us`), FaultInfo 플래그 확장은 **제안**이다. 아직 구현되지 않았고 Project Leader 승인 전이다.
> - 본문의 수치(C, D, MIT, T, 큐 깊이, 지연 목표)는 **후보**다. 측정으로 확정하지 않았다.
> - 이 가이드는 REQ v5.2, SRS v4.2, DDR-008 v3.3, HLD v5.3, LLD v5.2 (커밋 `937ca3c`)를 기준으로 쓴다. 정식 문서와 다르면 정식 문서가 우선한다.
> - 본문은 두 가지를 구분한다. **[현행]**은 지금 코드에서 동작하는 것이다. **[P1 제안]**은 P1에서 제공하려는 것이다. 제안을 현재 기능처럼 쓰지 않는다.
> - 대괄호 번호(예: [A-08], [H-8])는 설계 추적 ID다. 개발에 필요한 내용은 본문에 모두 있다.

## 요약

- L2 태스크는 외부 입력이 도달해야 실행되는 워크로드다. 입력 한 건이 **Event**, 그 처리 한 건이 **Job**이다.
- 앱은 두 부분을 쓴다. **입력 어댑터**는 도달 지점에서 `t_arr`를 기록하고 게시(post)한다. **워커**는 Event 대기(wait)와 종료(done)를 반복한다.
- 마감은 `t_dl = t_arr + D`다. 도달 시각부터 센다. 도달 뒤 게시까지의 지연은 모두 D를 쓴다.
- 예산 소진과 스로틀은 다르다. 예산이 0인데 계속 실행 가능하면 스로틀된다. 정확히 소진하고 잠들면 스로틀되지 않는다.
  다만 보충 윈도우가 끝나기 전에 다시 깨어나면 잔여 예산이 0이므로 스로틀된다. 윈도우 끝까지 남은 시간이 허용 오차 `tol`(후보 50 µs) 이하이면 끝난 것으로 보고 예산을 C로 채운다.
- 현재 코드에는 `ttsched_evt_*` API가 없다. 현행 FCW 예제는 SOME/IP ID 기반의 `libtimpani_rt`를 쓴다. P1에서 공통 게시 경로로 바꾼다.
- 어디서 무엇을 부르는지는 1.1절, 개발 절차는 4장, 반환 코드와 폴트 해석은 7장, radar -> FCW 예제는 8장에 있다.
- 공개 API 승인, 수치 측정, 재바인딩 때의 `t_arr` 기준 같은 미정 사항은 12장에 있다.

**지금 할 수 있는 것 (현행 코드 기준).** 현재 libttsched로는 이 가이드의 앱을 빌드할 수 없다. P1 승인과 구현 뒤에 가능하다.

| 항목 | 지금 가능 여부 | 비고 |
|:--|:--|:--|
| `ttsched_evt_*` 게시와 대기 | 불가 | 현행 libttsched에 없다. P1 승인과 구현 뒤 |
| 워크로드 YAML과 `scripts/mock_pullpiri.py` | 조건부 | 현행 키만 쓴다. MIT 키는 승인 뒤. protobuf 스텁을 다시 생성해야 L2로 분류된다(8.2 알려진 문제) |
| 현행 FCW 예제 실행과 이상 주입 | 조건부 | 8장. 현행 `libtimpani_rt`를 쓰며 수정 전이다. Linux, 격리 CPU, 스텁 재생성이 필요하다 |
| C, MIT 측정 | 부분 가능 | `budget_used_us`는 제안 필드라 쓸 수 없다. 예비 측정은 4장 3단계 |
| 수용식 계산 | 손으로 가능 | 4장 수용식을 직접 계산한다. TIMPANI-O 자동 판정은 [P1 제안] |
| 폴트와 Job 기록 보기 | 부분 가능 | 현행은 timpani-n 로그의 Dmiss와 폴트 줄. 표식과 Job 기록은 P1 |

## 1. 한눈에 보기

### 1.1 어디서 무엇을 부르나 [P1 제안]

FCW 소비자(`fcw_consumer`)를 예로 든다. 표의 번호는 아래 코드와 선언의 주석 번호와 같다. 번호는 코드 위치를 가리키며 실행 순서가 아니다. 실행 순서는 1.2절에 있다.

| | 어디서 | 무엇을 | 언제 | 자세히 |
|:--|:--|:--|:--|:--|
| ① | `main()` | `ttsched_evt_open("cbs_task")`로 source 핸들을 얻는다 | 시작 시 한 번, 미들웨어 시작 전 | 5장 |
| ② | L2 워커 스레드 | 스레드 이름을 `cbs_task`로 바꾼 뒤 `ttsched_init()` | 워커 시작 시 한 번, 미들웨어 시작 전 | 4장 5단계 |
| ③ | 입력 콜백 | 첫 문장에서 `t_arr`를 읽고 `ttsched_evt_post()` | 입력마다 | 4장 2·6단계 |
| ④ | L2 워커 스레드 | `ttsched_evt_wait()` → 처리 → `ttsched_evt_done()` | Event마다 반복 | 4장 5단계 |
| ⑤ | 워크로드 선언(YAML) | `runtime`, `deadline`, `period`, `min_inter_arrival_us` | 배포 시 | 4장 3·4단계 |

`cbs_task`는 이 워커의 `task_id`다. 스레드 이름, `ttsched_evt_open()` 인자, YAML의 `name`이 모두 같아야 한다(1~15바이트, 노드 안에서 유일).

```cpp
// FCW 소비자에 적용한 모습(스케치, 컴파일 검증 전). 오류 처리 전체는 4장 5·6단계와 7장에 있다.
// handle_post_error, run_collision_check는 앱 함수다.
#include <libttsched.h>

static ttsched_evt_t g_src = -1;
static std::mutex g_post_mtx;   // 입력 스레드가 여럿일 수 있으면(H-8) 게시 호출만 잠근다

// ③ 입력 콜백 (vsomeip 수신 스레드)
void on_obstacle_alert_received(const std::shared_ptr<vsomeip::message>& msg) {
    const uint64_t t_arr = clock_ns(CLOCK_MONOTONIC);   // 첫 문장에서 도달 시각을 읽는다
    /* payload 검사와 RadarTarget(16바이트) 파싱: 기존과 같다 */
    std::lock_guard<std::mutex> lk(g_post_mtx);
    const int rc = ttsched_evt_post(g_src, t_arr, target, sizeof(*target), nullptr);
    if (rc < 0) handle_post_error(rc);                   // Event가 만들어지지 않았다(7.1절)
}

// ②④ L2 워커: Event 한 건이 Job 하나다
void cbs_worker_loop() {
    pthread_setname_np(pthread_self(), "cbs_task");      // ② 이름 = task_id
    ttsched_init();
    for (;;) {
        struct ttsched_evt_event ev;
        const int rc = ttsched_evt_wait(&ev);            // ④ Event 한 건을 받아 Job을 시작한다
        if (rc == TTSCHED_EVT_E_INACTIVE) { ttsched_init(); continue; }  // 바인딩 전 등. back-off 뒤 반환
        if (rc < 0) abort();                             // E_STATE: done 누락 버그
        run_collision_check(*reinterpret_cast<const RadarTarget*>(ev.payload));  // 블로킹·로그 없이
        ttsched_evt_done(TTSCHED_EVT_COMPLETED);         // 반드시 부른다
    }
}

// ① main: 핸들 → 워커 → 미들웨어 순서로 시작한다
int main() {
    g_src = ttsched_evt_open("cbs_task");
    if (g_src < 0) return EXIT_FAILURE;
    std::thread(cbs_worker_loop).detach();
    /* vsomeip 초기화, register_message_handler, subscribe, app->start(): 기존과 같다 */
}
```

```yaml
# ⑤ FCW 소비자 선언(scripts/mock_pullpiri.yaml 발췌, P1 변경 후).
#   priority, policy, cpu_affinity, release_time은 현행 그대로 둔다.
workloads:
  - workload_id: verify_wl_cbs
    temporal_class: TEMPORAL_SPORADIC   # L2 (CBS)
    tasks:
      - name: cbs_task                  # 워커 스레드 이름과 같다
        runtime: 2000                   # C: 보충 윈도우당 CPU 예산 (µs, 후보)
        deadline: 5000                  # D: 도달부터 완료까지 (µs, 후보)
        period: 18000                   # T: CBS 보충 주기, MIT 이하 (현행 20000에서 변경)
        min_inter_arrival_us: 18000     # MIT [P1 제안]. 승인 전에는 period 값을 쓴다
        max_dmiss: 2                    # DMISS를 보고하기 전 허용 횟수
```

Event API 없이 ②의 스레드 이름과 ⑤의 선언만 맞춰도 [현행]에서 CBS 예산 집행을 받는다. protobuf 스텁 재생성과 기동 순서는 8.2절을 따른다. 다만 Event별 마감 감시와 MIT 계수는 받지 못한다(11장 Q2).

### 1.2 Event 한 건의 경로

아래 그림은 워커가 먼저 `ttsched_evt_wait`를 호출하고 Event가 없어 잠든 경우다. 입력 어댑터가 도달 시각을 찍고 게시하면 공통 큐에 Event와 마감을 기록하고, post 안에서 워커를 깨운다. 커널과 BPF가 워커를 실행하면 wait 안에서 Job을 시작하고 Event를 넘긴다([HLD-008 §4.2]). Event 공개(QUEUED)는 Job 시작(RUNNING)보다 먼저지만, post와 wait의 반환 순서는 보장되지 않는다. 이미 대기 중인 Event가 있으면 wait는 잠들지 않고 반환한다. DeadlineMonitor는 워커 완료와 독립적으로 마감을 감시한다.

```mermaid
sequenceDiagram
  participant SRC as 송신원
  participant IA as 입력 어댑터
  participant LIB as libttsched 게시와 대기
  participant Q as 공유 메모리 큐
  participant W as 워커
  participant DM as DeadlineMonitor
  W->>LIB: ttsched_evt_wait 호출
  Note over W: 대기 Event가 없으므로 잠든다(futex 대기)
  SRC->>IA: 입력 도달
  Note over IA: 선언된 경계에서 t_arr 기록
  IA->>LIB: ttsched_evt_post 호출
  LIB->>Q: 수락, gen과 seq 부여, t_dl = t_arr + D (QUEUED)
  par 게시 마무리
    LIB-->>W: futex 깨움 요청 (post 안에서)
    LIB-->>IA: 0 수락
  and 워커 처리
    Note over W: 대기 중이면 깨움 뒤 실행; 이미 큐를 보면 더 일찍 진행 가능
    Note over W: 커널과 BPF가 워커를 CPU에 올린다(예산·TT 조건)
    W->>Q: wait 안에서 QUEUED를 RUNNING으로 확정 (t_begin은 워커가 사적으로 기록)
    Note over W: wait 반환, Event 한 건 인계
    Note over W: 블로킹 없이 처리
    W->>LIB: ttsched_evt_done COMPLETED
    LIB->>Q: 종료 상태 기록, Job 종료 (t_end는 Job 로그에 기록)
    W->>LIB: 다음 ttsched_evt_wait 호출
    Note over W: 대기 Event가 있으면 잠들지 않고 다음 Job을 시작한다
  and 독립된 마감 감시
    DM->>Q: 주기 스캔, now가 t_dl을 넘었는지 확인
    Note over DM: 마감 실패는 완료를 기다리지 않고 표시
  end
```

**Job 상태.** `t_begin`은 wait가 RUNNING으로 바꾼 시각이다. `t_end`는 워커가 done을 게시하기 직전에 읽은 시각이다. `now`는 DeadlineMonitor가 읽은 현재 시각이다.

| 상태 | 뜻 |
|:--|:--|
| QUEUED | 게시가 수락했고 워커가 아직 가져가지 않았다 |
| RUNNING | wait가 인계했다. `t_begin`이 찍힌다 |
| COMPLETED, DISCARDED | done으로 끝났다. `t_end`가 찍힌다 |
| CANCELLED, ABANDONED | TIMPANI가 닫았다. 시작 전이면 CANCELLED, 실행 중이면 ABANDONED다 |

**TT와 event-driven의 차이.** TT(Time-Triggered) 태스크(L1)는 미리 정한 슬롯에 맞춰 주기적으로 실행된다.
현행에서 TIMPANI-N이 `ttsched_wait_next_period()`의 대기를 깨운다.
L2(CBS) 태스크는 외부 입력이 도달할 때 실행된다.
도달 시점을 미리 알 수 없으므로, TIMPANI는 CBS 서버로 CPU 예산을 집행한다. Event별 마감 감시(DeadlineMonitor)는 [P1 제안]이다.
두 종류는 같은 CPU에 함께 배치할 수 있다.
그때는 수용 판정(4장의 수용식)을 함께 통과해야 한다.

## 2. 꼭 알아야 할 용어

용어의 정본은 [REQ-008 §2](REQ-008-event-driven-workload-support.md)다. 이 표는 개발에 필요한 뜻만 요약한다.

| 용어 | 뜻 | 비고 |
|:--|:--|:--|
| Event | 수락된 도달 한 건. 식별자는 `(gen, seq)` | 거절된 게시는 Event가 아니다. `seq`는 같은 gen 안에서만 유일하고 2^32 뒤 되돌아간다. 재시작 사이의 유일성은 확률적이다 |
| Job | Event 하나의 처리. wait에서 시작하고 done에서 끝난다 | Event와 1:1이다. 여러 Event를 합치지 않는다 |
| 도달 (arrival, `t_arr`) | 입력 어댑터가 선언한 경계에서 입력을 처음 인지한 시각. `CLOCK_MONOTONIC` | 경계 앞에서 생긴 지연은 D에 들어가지 않는다. 경계를 늦게 잡으면 마감 판정이 그만큼 느슨해진다 |
| 게시 (post) | 어댑터가 Event를 큐에 넣는 호출 `ttsched_evt_post` | 수락 또는 거절 결과를 돌려준다 |
| Event 대기 (wait) | 워커가 Event 한 건을 받아 Job을 시작하는 호출 `ttsched_evt_wait` | 열린 Job이 없어야 한다. Event가 없으면 잠든다 |
| 종료 (done) | 워커가 열린 Job을 끝내는 호출 `ttsched_evt_done` | 생략할 수 없다 |
| 마감 (deadline, `t_dl`) | `t_dl = t_arr + D`. 수락 시 한 번 정하며 바뀌지 않는다 | 활성화 시각에 D를 더한 값은 마감이 아니다 |
| MIT (`min_inter_arrival_us`) | 같은 source의 연속 도달 사이 최소 간격 | 같은 source의 연속 도달 간격을 도달 시각으로 잰다 |
| 수용 판정 (admission) | TIMPANI-O가 배치 CPU의 모든 L2 태스크에 수용식을 적용하는 사전 판정 | Project Leader 승인과 다른 개념이다 |
| 예산 보충 (replenishment) | 보충 윈도우가 열릴 때 서버 예산을 C로 채우는 동작 | Job 시작만으로는 예산을 주지 않는다 |
| 스로틀 (throttling) | 예산이 0인 채 실행 가능한 서버의 추가 실행을 막는 동작 | 예산을 쓰고 잠든 것만으로는 아니다. 깨어날 때 잔여 예산이 0이고 보충 윈도우가 끝나지 않았으면 스로틀된다(남은 시간이 `tol` 이하이면 끝난 것으로 본다) |
| 기준 생산자 구성 | 도달 시각 기록부터 게시 반환까지 생산자 스레드가 선점되지 않는 구성 (REQ-008 A-10) | 벗어나면 L_post 상한과 수용식 보장이 성립하지 않는다 |
| D | 도달부터 완료까지의 상대 마감 | 도달 뒤 지연은 모두 D를 쓴다 |
| C | 보충 윈도우당 CPU 예산(`runtime`) | Job 하나의 측정 최대 CPU 시간보다 크게 선언한다. Job마다 새로 채워지지 않는다 |
| T | CBS 서버 주기(`period`) | T ≤ MIT다 |
| source | 입력 원천 하나. L2 태스크 하나에 하나다 | Event의 출처다 |
| 흔들림 | 주기 생산자가 주기에서 벗어나는 폭(송신 시점, 전달 지연) | MIT를 잡을 때 뺀다 |
| 활성화 (`t_act`) | 워커 스레드가 잠에서 깨어 실행 가능해진 시각 | 마감의 기준이 아니다. 예산 보충 윈도우의 기준이다 |
| 바인딩과 재바인딩 | 워커 스레드를 L2 서버에 붙이는 것과 다시 붙이는 것 | 재바인딩 때 `t_arr` 기준은 12장 H-20 |
| 보충 윈도우 | 길이 T_s의 구간. 열릴 때 예산이 C로 찬다 | 새 Job만으로는 보충하지 않는다 |
| TT(L1), CBS(L2) | 미리 정한 슬롯의 주기 태스크(L1), Event 구동 CBS 서버 태스크(L2) | 두 종류는 같은 CPU에 함께 둘 수 있다 |
| SHM | 앱과 TIMPANI-N이 공유하는 메모리 | 큐와 카운터가 여기에 있다 |
| sched_ext | 커널 스케줄러 확장(BPF) | TIMPANI-N이 CBS 서버 예산을 집행하는 곳이다 |

## 3. 앱 구조와 책임 분담

한 L2 태스크는 source 하나와 워커 스레드 하나로 구성한다. 입력 어댑터는 입력을 받은 스레드에서 게시만 한다. 워커는 wait, 처리, done의 루프만 돈다.
앱은 공통 큐를 따로 만들지 않는다. 입력 스레드가 여럿이면 게시 호출만 포스터 쪽 잠금으로 직렬화한다. 워커는 잠그지 않는다.

| 주체 | 하는 일 | 하지 않는 일 | 상태 |
|:--|:--|:--|:--|
| 앱 입력 어댑터 | 경계에서 `t_arr` 기록, `ttsched_evt_post` 호출, 반환 코드 처리 | 큐 관리, 마감 계산, MIT 판정 | [P1 제안] |
| 앱 워커 | `ttsched_evt_wait`, 처리, `ttsched_evt_done` (COMPLETED 또는 DISCARDED) | 자체 큐, 블로킹 I/O, SCHED_EXT나 우선순위 설정 | [P1 제안] |
| libttsched (앱에 링크) | post, wait, done 구현. SHM 쓰기와 수락·거절 계수 | 메시지 내용 해석 | [P1 제안]. 현행은 `ttsched_init()`과 `ttsched_wait_next_period()`만 있다 |
| 공유 메모리 (SHM v2) | source별 슬롯, 고정 크기 큐, 카운터, Job 기록 | payload 해석 | [P1 제안] |
| TIMPANI-N BPF (sched_ext) | L2 서버 예산 차감, 스로틀, 예산 보충, 실행 구간 관측 | Event 내용 해석 | 예산 차감과 스로틀은 [현행]. 보충 규칙(활성화 기준 윈도우)과 실행 구간 관측은 [P1 제안] |
| TIMPANI-N 사용자 공간 | 워커 바인딩, 폴트 계수와 보고, DeadlineMonitor(마감 감시) | 메시지 의미 판단 | 바인딩과 폴트 일부는 [현행]. DeadlineMonitor는 [P1 제안] |
| TIMPANI-O | CPU 배치, 워크로드 파라미터 검증, 수용 판정 | 런타임 Event 처리 | CPU 배치는 [현행]. 파라미터 검증과 수용식은 [P1 제안] |

## 4. 개발 절차

아래 순서를 따른다. 각 단계의 산출물이 다음 단계의 입력이다. 워커 루프와 입력 어댑터의 코드는 제안된 API만 쓴다.

1. **Event와 payload를 정한다.** payload는 64바이트 이하로 한다(`TTSCHED_EVT_PAYLOAD_MAX`, 후보). 값 자체를 넣는다.
   큰 데이터의 포인터나 참조를 넣지 않는다. P1에는 해제 통지가 없다.
   예: FCW의 `RadarTarget`은 16바이트다.

2. **도달 경계와 `t_arr` 지점을 정한다.** 수신 콜백의 첫 문장, 또는 datagram 읽기가 끝난 직후가 맞다. 경계를 파일과 함수 이름으로 기록한다. 경계에서 게시하지 못하면 그 차이만큼 D가 줄어든다([A-08], REQ-008).

3. **C, D, MIT, T를 정한다.** 규칙은 다음과 같다.
   - $0 < C \le D \le MIT$, $T \le MIT$를 지킨다.
   - **MIT**는 도달 지점에서 본 최소 간격이다. 주기 생산자는 주기에서 송신 흔들림과 전달 흔들림을 뺀다.
     예: 생산 주기 20 ms, 흔들림 2 ms면 MIT는 18 ms다. 깨움 흔들림과 전달 흔들림이 더 있으면 17 ms 이하다([HLD-008 §8.4]).
     규격이 없으면 측정한 최소 간격에서 여유를 뺀다. 허용 오차는 후보 0이다(tol_mit, 측정 전). 낙관적 MIT는 정상 구동에서도 MIT_VIOLATION을 낸다.
   - **C**는 측정한 Job당 최대 CPU 시간보다 크게 선언한다. 측정은 다음 순서다.
     (1) 실행 구간 하나에 Job 하나가 되도록 Event 간격을 넓히고 `budget_used_us`(P1 제안 필드)를 잰다.
     (2) 입력 종류별 최댓값을 본다.
     (3) 유휴 뒤 첫 Job, 버스트, 간섭 조건을 더한다.
     (4) 최댓값에 여유를 더해 선언한다. 여유율은 미정(H-19)이다.
     (5) 정상 구동과 이상 주입으로 검증한다.
     (6) 운용 중 관측한다.
     퍼센타일 기준은 쓰지 않는다. 정상 구동에서도 BUDGET_EXCEED를 낼 수 있기 때문이다([HLD-008 §8.2]).
     주의: `budget_used_us`는 Job이 아니라 실행 구간 단위다. 한 구간에 Job이 여럿이면 합산되고, Job 안에서 잠들면 한 Job이 여러 구간으로 나뉜다.
     Job 로그의 t_end − t_begin은 벽시계 시간이므로 C 측정에 쓰지 않는다.
     현행 예제에는 `budget_used_us`가 없다. 예비 측정으로 처리 구간의 `CLOCK_THREAD_CPUTIME_ID` 차를 로그에 남길 수 있다. 이 값은 P1 측정이 아니다.
   - **D**는 도달부터 완료까지의 상대 마감이다.
   - **T**는 CBS 서버 주기다. 생산 주기가 있으면 그보다 크지 않게 잡는다.
   - FCW 시작값은 C 2 ms, D 5 ms, T 18 ms, MIT 18 ms다. 모두 후보다. HLD-008 §8.4 산정 기준으로는 MIT가 17 ms 이하이므로 18 ms는 시작값일 뿐이다. 정상 구동에서 잰 최솟값보다 작게 확정한다.

4. **워크로드를 선언한다.**
   - [현행] `scripts/mock_pullpiri.yaml`은 `workload_id`, `temporal_class`, `tasks`를 쓴다.
     태스크 항목은 `name`, `priority`, `policy`, `cpu_affinity`, `period`, `runtime`, `deadline`, `release_time`, `max_dmiss`다.
     `max_dmiss`는 DMISS를 보고하기 전 허용 횟수다(PRT-002). `cpu_affinity`는 배치에 쓰이지 않는다(8.1).
     L2는 `temporal_class: TEMPORAL_SPORADIC`이다. `name`은 워커 스레드 comm과 같아야 한다. `runtime`은 C, `deadline`은 D, `period`는 T다.
   - [현행] 이 YAML에는 MIT 필드가 없다.
   - [P1 제안] `TaskInfo`에 `uint32 min_inter_arrival_us = 11;`을 추가한다(LLD-008 §2.1). YAML 키 `min_inter_arrival_us`가 이 필드에 대응한다.
     값이 0이면 `period`를 MIT로 쓴다(경고 로그, 승인 필요). 승인 전에는 MIT가 `period`로 정해진다.
   - [P1 제안] TIMPANI-O는 MIT = 0, T > MIT, D > MIT, 15바이트를 넘는 task_id, 노드 안에서 중복된 task_id를 ParamInvalid로 거부한다(LLD-008 §9).
   - FCW 예제는 `period`를 20000에서 18000으로 바꿔야 $T \le MIT$를 만족한다. 이 변경은 [HLD-008 §8.4]에 있다.

5. **워커 루프를 쓴다.** 스레드 comm을 `task_id`로 바꾸고, `ttsched_init()`을 한 번 부른 뒤 wait, 처리, done을 반복한다. 처리 중에는 블로킹 호출을 피한다.
   wait는 Event가 올 때까지 잠든다. E_INACTIVE는 back-off 뒤에 돌아온다.

   ```c
   /* 필요한 헤더: <pthread.h>, <stdint.h>, <stddef.h>, <stdlib.h>, <time.h>, "libttsched.h".
    * pthread_setname_np는 Linux 전용이며 _GNU_SOURCE를 헤더보다 먼저 정의해야 한다.
    * log_error, process_event는 앱 함수다. */
   #define WORKER_TASK_ID "cbs_task"        /* 노드 안에서 유일한 task_id이며 워커 comm과 같다 */

   static void *worker_main(void *arg) {
       (void)arg;
       pthread_setname_np(pthread_self(), WORKER_TASK_ID);
       ttsched_init();
       for (;;) {
           struct ttsched_evt_event ev;
           const int rc = ttsched_evt_wait(&ev);
           if (rc == TTSCHED_EVT_E_INACTIVE) {  /* back-off 뒤 반환. 다시 바인딩한다 */
               ttsched_init();
               continue;
           }
           if (rc < 0) {                        /* E_STATE는 done 누락 버그다. 숨기지 않는다 */
               log_error("ttsched_evt_wait", rc);
               abort();
           }
           process_event(&ev);                  /* ev.len(64 이하)만 쓴다. 블로킹 호출 없음 */
           const int done_rc = ttsched_evt_done(TTSCHED_EVT_COMPLETED);  /* 필수. 정책 폐기는 DISCARDED */
           if (done_rc < 0) {
               log_error("ttsched_evt_done", done_rc);   /* E_INACTIVE는 TIMPANI가 이미 닫은 늦은 종료다 */
           }
       }
   }
   ```

6. **입력 어댑터를 쓴다.** 경계의 첫 문장에서 `t_arr`를 읽고 바로 게시한다. 입력 스레드는 하나로 두는 것을 기본으로 한다(H-8 확인 전).
   여러 스레드가 게시하면 게시 호출만 앱이 직렬화한다. 워커는 잠그지 않는다.
   잠금 대기는 `t_arr` 뒤의 지연이므로 D를 쓴다. 잠금 대기 중 더 늦은 `t_arr`가 먼저 게시되면 E_INVAL이 날 수 있다. 여러 스레드면 A-10 구성과 충돌할 수 있으므로 측정으로 확인한다.

   ```c
   static ttsched_evt_t g_src = -1;   /* 초기화 때 ttsched_evt_open("cbs_task")로 한 번 얻는다. 음수면 게시하지 않는다 */
   static pthread_mutex_t g_post_mtx = PTHREAD_MUTEX_INITIALIZER;   /* 입력 스레드가 여럿일 때만 쓴다 */

   /* 선언된 경계: 입력이 앱 코드에 처음 보이는 지점 */
   void on_input_received(const uint8_t *msg, uint32_t len) {
       struct timespec ts;
       clock_gettime(CLOCK_MONOTONIC, &ts);
       const uint64_t t_arr = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

       pthread_mutex_lock(&g_post_mtx);   /* 같은 source의 게시 문맥은 한 번에 하나다 */
       struct ttsched_evt_id id;
       const int rc = ttsched_evt_post(g_src, t_arr, msg, len, &id);
       pthread_mutex_unlock(&g_post_mtx);

       if (rc == 0) {
           return;                        /* 수락: (gen, seq)가 생겼고 마감은 t_arr + D */
       }
       handle_post_error(rc);             /* 앱 함수. 7.1절 표대로 센다 */
   }
   ```

7. **반환 코드를 처리한다.** 0은 수락이다. E_FULL과 E_INACTIVE는 Event가 생기지 않았다. 표는 7.1절에 있다.
   E_INACTIVE 뒤에 `ttsched_init()`을 부르고 재시도할지 버릴지는 앱 정책이다. 기본값은 미정이다.
   재시도할 때는 같은 입력이 두 번 수락되지 않도록 앱이 막는다. E_INVAL은 앱 버그이므로 로그에 남긴다.

8. **생산자 스레드를 구성한다.** 게시를 호출하는 스레드는 기준 생산자 구성(REQ-008 A-10)을 만족해야 한다.
   - 도달 시각 기록부터 게시 반환까지 다른 스레드에 선점되지 않아야 한다. 후보는 유휴이거나 전용인 CPU에 고정한 실시간 정책 스레드다. 값은 측정으로 확정한다.
   - 실시간 정책은 배포 담당이 설정한다. P1에서 TIMPANI는 입력 스레드의 정책을 관리하지 않는다([DDR-008](DDR-008-event-driven-workload-support.md) D-24). 등록 워커의 SCHED_EXT 집행은 TIMPANI-N이 맡는다.
   - 이 구성을 벗어나면 TIM-011의 L_post 상한과 수용식 보장이 성립하지 않는다. 배포에서 구성을 검증한다.

9. **실행하고 점검한다.** 정상 구동에서 다음을 확인한다. 폴트의 해석은 7장을 따른다.
   폴트가 0건이어야 한다. MIT_VIOLATION이 나오면 MIT가 낙관적이다.
   Job 기록 수는 수락된 Event 수와 같아야 한다. 모든 Job에서 $t_{end} - t_{arr} \le D$이고 `missed`가 없어야 한다.
   `overflow`가 0이어야 한다. 정지 상태에서 `accepted = started + cancelled-before-start + queued`가 성립해야 한다.
   실행 방법은 8장에 있다.

**수용식 (9단계 전에 손으로 확인하는 판정, 제안).** 같은 CPU에 배치된 모든 L2 태스크 $i$에 대해 다음을 확인한다. CBS-004의 P1 조건이며 충분 조건이다.

$C_i + B_{tt}(k, D_i) + L_{del}^{max} + \sum_{j \ne i}(C_j + \delta_{ovr}) \le D_i$, $D_i \le MIT_i$, $T_i \le MIT_i$

- $C_i$는 태스크 $i$의 예산, $D_i$는 상대 마감, $MIT_i$는 최소 도달 간격이다.
- $B_{tt}(k, D_i)$는 길이 $D_i$ 구간에서 CPU $k$의 TT 점유 최댓값이다. $L_{del}^{max}$는 도달부터 워커 첫 실행까지의 상한이다. $\delta_{ovr}$는 스로틀 전 예산 초과 상한이다.
- 후보 값은 $L_{del}^{max}$ 100 µs, $\delta_{ovr}$ 1 ms다([HLD-008 §10 H-9]). 측정하기 전에는 합격 기준이 아니다.
- 이 식은 증명이 아니다. 전제(도달이 MIT를 지킴, 각 Job이 다음 도달 전에 끝나 워커가 잠듦, 도달 직후 게시)가 깨지면 보장이 없다.

## 5. API 요약 (P1 제안)

아래 API는 [LLD-008 §6.1](LLD-008-event-driven-workload-support.md)의 제안이다. Project Leader 승인 전이다. 시그니처는 제안 문서의 표기를 그대로 옮겼다.

```c
/* sample-apps/src/libttsched.h: v2 공개 부분 (제안) */
#include <stdint.h>
#define TTSCHED_EVT_PAYLOAD_MAX 64U              /* 후보. inline payload bytes per Event */

#define TTSCHED_EVT_E_INACTIVE (-1)              /* source 비활성, 워커 미바인딩, 제거, 데몬 중지, SHM 쓰기 불가, Job이 TIMPANI에 의해 닫힘 */
#define TTSCHED_EVT_E_FULL     (-2)              /* 큐 가득 참. 넘침으로 계수 */
#define TTSCHED_EVT_E_INVAL    (-3)              /* 잘못된 인자: len 초과, 미래 또는 직전보다 이른 t_arr, 잘못된 task_id, 잘못된 outcome */
#define TTSCHED_EVT_E_STATE    (-4)              /* 열린 Job 중 wait, 열린 Job 없이 done */

enum { TTSCHED_EVT_COMPLETED = 0, TTSCHED_EVT_DISCARDED = 1 };

typedef int ttsched_evt_t;                        /* source 핸들: 0 이상 성공, 음수 오류 */
struct ttsched_evt_id    { uint64_t gen; uint32_t seq; };
struct ttsched_evt_event { uint64_t gen; uint32_t seq; uint32_t len;
                           uint64_t t_arr_ns; uint64_t t_deadline_ns;
                           uint8_t payload[TTSCHED_EVT_PAYLOAD_MAX]; };

ttsched_evt_t ttsched_evt_open(const char *task_id);
int ttsched_evt_post(ttsched_evt_t src, uint64_t t_arr_ns, const void *payload, uint32_t len,
                     struct ttsched_evt_id *id_out);
int ttsched_evt_wait(struct ttsched_evt_event *ev);
int ttsched_evt_done(int outcome);
void ttsched_init(void);                          /* 현행과 같은 이름. P1에서 v2 SHM 매핑을 맡는다 */
int ttsched_wait_next_period(void);               /* L1(TT), 변경 없음 */
```

| 호출 | 누가 부르나 | 인자 | 반환 | 규칙 |
|:--|:--|:--|:--|:--|
| `ttsched_evt_open` | 입력 어댑터, 워커 초기화 | `task_id` (1~15바이트) | 핸들 (0 이상), 음수는 오류 | 비-RT 경로. 등록 전에 불러도 된다. 등록 전 게시는 E_INACTIVE다 |
| `ttsched_evt_post` | 입력 어댑터 | 핸들, `t_arr_ns`, payload, `len`, `id_out`(NULL 가능) | 0 수락, 음수는 아무것도 만들지 않음 | source당 한 번에 한 문맥. `t_arr_ns == 0`이면 호출 시각을 쓴다 |
| `ttsched_evt_wait` | 워커 | `ev` 버퍼 | 0 Job 시작, 음수는 실패 | 열린 Job이 없어야 한다. Event가 없으면 잠든다. E_INACTIVE이면 back-off 후 반환한다 |
| `ttsched_evt_done` | 워커 | `outcome` | 0 기록, 음수는 실패 | 모든 Job에 필요하다. 자동 완료는 없다 |
| `ttsched_init` | 워커, 입력 어댑터 | 없음 | 없음 | 비-RT 경로. 여러 번 불러도 된다. E_INACTIVE 뒤 호출한다 |

**호출 규칙.**
- `t_arr_ns`를 주면 CLOCK_MONOTONIC 값이어야 한다. 미래 값이거나 같은 source의 직전 도달보다 이르면 E_INVAL이다.
- 현재 시각보다 이른 값은 그대로 받는다. 그 지연은 D를 쓴다([A-08]).
- 라이브러리는 잠금을 쓰지 않는다. 같은 source의 게시는 한 번에 한 문맥이어야 한다. 입력 스레드가 여럿이면 앱이 게시 호출을 직렬화한다. 어기면 그 source의 큐만 깨질 수 있다. 라이브러리 차원의 다중 생산자 게시는 P2다([F-13]).
- `ttsched_evt_wait`와 `ttsched_evt_done`은 같은 워커 스레드에서만 부른다. 다른 스레드가 부르면 계약 위반이다.
- 게시와 대기는 힙과 잠금을 쓰지 않는다. 매핑은 비-RT 경로에서 한다. 매핑이 안 되면 게시와 대기는 E_INACTIVE를 돌려주고 스스로 다시 매핑하지 않는다.
- P1에는 대기 중인 워커를 Event 없이 깨우는 API가 없다. 워커는 프로세스 종료로 끝난다([L-8], 12장).

## 6. 지켜야 할 규칙 체크리스트

**지킨다**
- [ ] 입력 경계의 첫 문장에서 `t_arr`를 읽는다. 경계 뒤의 지연은 D를 소진한다.
- [ ] 워커 스레드 comm을 `task_id`로 설정한다. source당 워커는 하나다. wait와 done은 그 스레드에서만 부른다.
- [ ] 모든 Job을 `ttsched_evt_done()`으로 끝낸다. 처리하지 않기로 한 Event는 DISCARDED로 끝낸다. 성공이 아니다.
- [ ] 각 Job이 다음 도달 전에 끝나 워커가 잠들게 한다. 연속 Job이나 블로킹 뒤 재개는 새 예산을 받지 않고 같은 윈도우의 남은 예산을 나눠 쓴다.
- [ ] 게시의 모든 반환 코드를 처리한다. E_FULL과 E_INACTIVE를 앱 통계로 센다.
- [ ] payload는 64바이트 이하 inline 값이다. 포인터를 넣지 않는다.
- [ ] $0 < C \le D \le MIT$, $T \le MIT$를 확인한다. C는 측정 최댓값보다 크게 선언한다.
- [ ] 입력 어댑터가 쓰는 스레드를 기준 생산자 구성에서 측정한다.
- [ ] 로그는 Job 밖에서 쓴다. 로그 출력을 Job 안에 두지 않는다.
- [ ] 정상 구동 검증 뒤 MIT와 C를 확정한다.

**하지 않는다**
- 워커가 소켓이나 타이머를 직접 읽는 구조로 Event를 받지 않는다. 그 구조도 CBS 예산 집행은 받지만 Event 도달 시각, Job 기록, 마감 감시는 얻지 못한다([HLD-008 §8.3] 표, [REQ-008 §2.7]).
- Job 안에서 경합 있는 잠금, 블로킹 I/O, `sleep`, 동기 호출을 쓰지 않는다. 블로킹은 허용되지만 수용식의 보장 밖이며, 마감은 감시된다([A-06]).
- 워커에서 SCHED_EXT, 우선순위, affinity를 직접 설정하지 않는다.
- 앱에 자체 MIT 판정이나 마감 판정을 두지 않는다. 판정과 보고는 TIMPANI-N이 한다.
- 재바인딩 경계에서 같은 입력을 중복 게시하지 않는다. 재게시는 새 `gen`의 별개 Event가 된다. 중복 처리 방지는 앱이 맡는다.
- 한 source에 워커를 여러 개 두지 않는다. 워커 풀은 P1에서 지원하지 않는다.
- 새 코드에 `timpani_rt.h`의 `timpani_*` API를 쓰지 않는다. 이 라이브러리는 폐기 대상이다([REQ-008 §12]).

## 7. 오류와 진단

### 7.1 게시(post) 반환 코드

| 코드 | 값 | 뜻 | 앱이 할 일 |
|:--|:--|:--|:--|
| 0 | - | 수락. Event `(gen, seq)`가 생기고 마감은 `t_arr + D`다 | 추가 작업 없음 |
| `TTSCHED_EVT_E_INACTIVE` | -1 | source가 비활성이거나 워커가 바인딩되지 않았다. 제거, 데몬 중지, SHM 쓰기 불가가 여기에 든다. 바인딩 직후 source가 아직 켜지지 않은 짧은 구간도 같다(L-14) | Event가 없다. `ttsched_init()` 뒤 재시도할지 버릴지 정한다. 재시도 때 중복 수락을 막는다 |
| `TTSCHED_EVT_E_FULL` | -2 | 큐가 가득 찼다. 넘침이다 | Event가 아니다. 버린 수를 센다. 워커가 밀린 양은 `accepted - started - cancelled`로 본다(정지 상태에서만 정확) |
| `TTSCHED_EVT_E_INVAL` | -3 | `len` 초과, 미래 또는 직전보다 이른 `t_arr`, 잘못된 `task_id` | 앱 버그다. 로그에 남긴다 |

**[P1 제안]** E_FULL은 큐 깊이 후보 8에서 난다. 살아 있는 Event가 8개보다 적어도 날 수 있다. 소비자가 아직 치우지 않은 칸도 큐 점유로 센다([HLD-008 §4.9]).
넘침은 `overflow`로 센다. `arrivals - accepted`는 넘침과 REVOKED의 합이다. 거절된 입력도 도달로 세어 MIT 위반 계수에 들어간다. 큐가 가득 차면 뒤의 Job이 늦는다.

### 7.2 대기(wait)와 종료(done) 반환 코드

| 호출 | 반환 | 뜻 | 앱이 할 일 |
|:--|:--|:--|:--|
| wait | E_INACTIVE | 지금 기다릴 수 없다. back-off(후보 10 ms) 뒤에 반환한다 | `ttsched_init()` 뒤 다시 wait |
| wait | E_STATE | 열린 Job이 있는데 wait를 불렀다 | 프로그래밍 오류다. done 호출 누락을 찾는다 |
| done | E_STATE | 열린 Job이 없는데 done을 불렀다 | 프로그래밍 오류다. wait 뒤 done 쌍을 확인한다 |
| done | E_INACTIVE | TIMPANI가 이미 Job을 닫았다(ABANDONED). 열린 플래그만 지워진다 | 늦은 종료다. 다음 wait로 넘어간다 |
| done | E_INVAL | `outcome`이 잘못되었다. Job은 DISCARDED로 끝난다 | 앱 버그다. 값을 확인한다 |

**Job 결과.** 성공 종료는 COMPLETED뿐이다.

| 결과 | 누가 끝내나 | 뜻 |
|:--|:--|:--|
| COMPLETED | 워커 (`done(COMPLETED)`) | 처리를 마쳤다. `t_end <= t_dl`이면 마감 충족이다. `t_end`는 done 직전 시계 읽기로 정한 해석이다(H-10) |
| DISCARDED | 워커 (`done(DISCARDED)`) | 앱 정책으로 폐기했다. 성공이 아니다. 마감 전이면 실패가 아니고, 마감 뒤이면 실패(DMISS)다 |
| CANCELLED | TIMPANI | 시작 전에 닫혔다(비활성화, 재등록, 데몬 정리). 마감 전이면 실패가 아니다. 마감 뒤이면 실패다([미정 H-11, OI-21]) |
| ABANDONED | TIMPANI | 실행 중에 닫혔다(워커 종료, 재등록). 마감 규칙은 CANCELLED와 같다([미정 H-11, OI-21]) |

### 7.3 폴트 종류

분류는 [REQ-008 PRT-005]와 [LLD-008 §2.1]의 제안이다. 보고 경로와 값은 승인 전이다.

| 무엇 | 현행에서 보는 곳 | P1 (승인 전 제안) |
|:--|:--|:--|
| 마감 실패(DMISS) | timpani-n 로그의 `[FaultMonitor] Dmiss ...` 줄(태스크 해시) | 개수 보고(PRT-007). 표식은 승인 뒤. 노출 위치 미정(OI-19) |
| 예산 초과(BUDGET_EXCEED) | timpani-n 로그의 `Fault threshold exceeded!` 줄(`dmiss_count=0`) | 스로틀 에피소드마다 한 번 집계 |
| MIT 위반 | 소비자 로그의 `FAULT MIT_VIOLATION` 줄 | T_s 윈도우마다 개수와 함께 보고 |
| 넘침과 수락 계수 | 앱 통계만 있다 | SHM 카운터(`overflow`, `accepted` 등). 노출 위치 미정(OI-19) |

**폴트별 뜻과 보고 규칙.**

| 폴트 | 뜻 | 보고 규칙 | 먼저 볼 것 |
|:--|:--|:--|:--|
| DMISS | Event의 Job이 `t_dl`을 넘겼다. `t_dl`이 지나도 열려 있었거나, `t_dl` 뒤에 완료·폐기·취소·중단으로 끝났다. COMPLETED라도 `t_end > t_dl`이면 실패다. Job당 한 번 | `max_dmiss`(PRT-002)를 넘으면 보고한다. 보고 여부와 무관하게 통계에 계수된다. 표식은 승인 뒤에 싣는다 | 표식으로 원인을 좁힌다(7.5) |
| BUDGET_EXCEED | 실제 스로틀에 들어갔다. 정보성 값이다. 양의 초과 실행을 뜻하지 않는다 | 스로틀 에피소드마다 최대 한 번 집계한다(PRT-006) | C 선언, 도달 간격, 연속 Job, 예산 보충 상태 |
| MIT_VIOLATION | 도달 간격이 MIT보다 짧다. 위반 도달도 수락해 큐에 넣는다 | T_s 윈도우마다 개수와 함께 보고한다 | 송신원 주기와 버스트, `min_inter_arrival_us` 값 |
| WATCHDOG | 예약 (P2) | - | - |

**현행 표현.** 현행 README와 로그 문자열은 다르다. 예산 초과 줄은 `FAULT_BUDGET_EXCEED`로 찍히지 않는다(8.4 표). 출처는 [examples/someip/README.md](../../../examples/someip/README.md)다.
**이름 대응.** DMISS = 로그 `Dmiss` = 생산자 출력 `DEADLINE_MISS`. BUDGET_EXCEED = README `FAULT_BUDGET_EXCEED`. 넘침 = `overflow`(E_FULL).
`accepted`, `arrivals`, `started`, `missed`는 SHM 카운터 이름이다. 노출 위치는 미정(OI-19)이다.

### 7.4 DMISS 표식(플래그)

표식은 승인 전에는 보고에 싣지 않는다. 승인 전에는 개수만 보고한다(PRT-007).

| 비트 | 이름 | 뜻 | 설정 주체 |
|:--|:--|:--|:--|
| 0x1 | MIT_RELATED | 마감 실패 Job의 `t_arr - T_s` 뒤에 같은 source의 MIT 위반이 있었다. 시간상 근접 표식이며 원인 판정이 아니다. 원인 분류는 P2다 | FaultMonitor |
| 0x2 | THROTTLED | 감지 시점에 서버가 스로틀 상태였다. 감지 경로에서만 값이 있다 | DeadlineMonitor |
| 0x4 | NOT_STARTED | 감지 시점에 Event가 아직 QUEUED였다. 워커가 시작하지 못했다 | DeadlineMonitor |
| 0x8 | LATE_COMPLETION | 마감 실패가 워커의 종료 전이(`t_end > t_dl`)에서 설정되었다 | 워커 |
| 0x10 | DETECTED_LATE | 닫기 절차(시작 정리, 비활성화, 재등록)에서 설정되었다. 데몬 중단 뒤에 늦게 확인된 실패다 | TIMPANI-N 닫기 절차 |

### 7.5 먼저 볼 순서

1. **DMISS.** 표식부터 본다.
   - NOT_STARTED: 워커가 wait에서 기다리고 있었는지 확인한다. 바인딩과 CPU 간섭(TT, 다른 L2)을 본다.
   - THROTTLED: C가 작은지, 도달이 연속인지 본다. 예산이 모자랐는지 확인한다.
   - LATE_COMPLETION: Job 처리 시간을 본다. Job 안의 블로킹을 찾는다.
   - MIT_RELATED: 시간상 가까운 MIT 위반을 먼저 확인한다. 그 뒤에 나머지를 본다.
2. **BUDGET_EXCEED.** 정상 구동에서 나오면 C 선언, 도달 간격, 연속 Job을 다시 본다. BUDGET_EXCEED는 C 과소 추정만의 증거가 아니다([HLD-008 §8.2] ⑥).
3. **MIT_VIOLATION.** 0이 아니면 MIT가 낙관적이다. 위반 도달은 큐에 쌓여 뒤의 Job을 늦춘다. 막지 않고 보고만 한다.
4. **개수 불일치.** `accepted`와 완료 수의 차이를 본다. 아직 열린 Job, CANCELLED, `overflow`를 차례로 확인한다.

## 8. radar -> FCW 예제

### 8.1 현재 구조 [현행]

현행 FCW 예제는 두 프로세스로 돈다. L1 TT 태스크는 레이더 주기 스캔을 맡고, L2 CBS 워커는 경보 판정을 맡는다.

| 프로세스 / 스레드 | 역할 | 구분 | 현행에서 쓰는 API |
|:--|:--|:--|:--|
| `mock_radar_producer` / `tt_task` | 20 ms 주기 스캔, 경보 발행 | L1 TT | `timpani_rt_init`, `timpani_tt_register`, `ttsched_wait_next_period` |
| `fcw_consumer` / vsomeip 수신 스레드 (`on_obstacle_alert_received`) | SOME/IP 경보 수신, 큐에 넣기 | 앱 입력 경계 | `timpani_evt_mark_arrival` |
| `fcw_consumer` / `cbs_task` (`cbs_worker_loop`) | 충돌 판정 한 건 처리 | L2 CBS | `timpani_cbs_bind_thread`, `timpani_cbs_unbind_thread`, 조건 변수 큐 |
| timpani-o | CPU 배치와 워크로드 검증 | 관리 | 이 가이드의 범위 밖 |
| timpani-n | BPF 집행(TT 깨움, L2 예산), 폴트 보고 | 노드 실행 | 이 가이드의 범위 밖 |
| `scripts/mock_pullpiri.py` | YAML을 timpani-o에 전송 | 테스트 도구 | `--host`, `--port`, `--config` |

현행 파라미터는 다음과 같다.
- `tt_task`: period 20000, runtime 2000, deadline 20000 (YAML).
- `cbs_task`: period 20000, runtime 2000, deadline 5000 (YAML). 코드의 MIT는 `timpani_evt_register(..., 18000)`에 있다.
- YAML의 `cpu_affinity`는 배치에 쓰이지 않는다. 배치는 timpani-o가 `available_cpus`에서 고른다.

[examples/someip/include/timpani_rt.h](../../../examples/someip/include/timpani_rt.h)의 현행 API는 여덟 개다.
`timpani_rt_init`, `timpani_tt_register`, `timpani_tt_wait_period`, `timpani_evt_register`,
`timpani_evt_mark_arrival`, `timpani_cbs_bind_thread`, `timpani_cbs_unbind_thread`,
`timpani_cbs_register_socket`(legacy)다.
P1에서 이 경로는 삭제된다([HLD-008 §8.4] (2)).

현행 코드는 이벤트마다 `timpani_cbs_bind_thread`와 `timpani_cbs_unbind_thread`를 부른다(`fcw_consumer.cpp`). P1에서는 바인딩을 TIMPANI가 맡는다.

### 8.2 실행 환경과 기동 순서

[examples/someip/README.md](../../../examples/someip/README.md)가 현행 실행 절차를 정리한다. 여기서는 요약만 한다.

**준비물 [현행]**
- 커널은 sched_ext와 BTF(`/sys/kernel/btf/vmlinux`)를 지원해야 한다. 메인라인 6.12는 legacy API, 6.13부터 DSQ API다.
  빌드에는 clang, libelf, linux-tools가 필요하다([timpani-n/README.md](../../../timpani-n/README.md)).
- 예제를 CMake로 빌드한다(`examples/someip`). timpani-o와 timpani-n을 빌드한다. 예제와 라이브러리는 Linux 전용이다(futex, prctl).
  실제 vsomeip은 필요 없다. 저장소의 mock 헤더(`examples/someip/mock_include`)가 127.0.0.1 UDP로 대신한다.
- Python 환경: 필요 패키지는 grpcio, grpcio-tools, pyyaml이다.
  가상환경 경로는 스크립트마다 다르다. `mock_pullpiri.py`와 `verify_pipeline.sh`는 `$TIMPANI_ROOT/.venv`를, 예제 README는 `scripts/.venv`를 쓴다. 한쪽으로 맞추기 전까지 두 경로에 모두 설치한다.
- `node_configurations`의 `available_cpus`에 **Isolated CPU**(일반 작업에서 분리한 CPU)를 넣는다. README 예시는 `[2]`다. 노드 ID는 timpani-n의 노드 ID와 같아야 한다.
- timpani-n은 root 권한이 필요하다(`sudo`). 두 앱은 격리 CPU cgroup 안의 셸에서 실행한다. 이 README에는 cgroup 설정 절차가 없다. 배치는 시스템 책임이다([A-09]).

**알려진 문제 [현행]** (소스를 읽어 확인했고 실행으로 확인하지는 않았다)
- `scripts/schedinfo_pb2.py`가 현재 proto보다 오래됐다. `temporal_class` 필드가 없고 `mock_pullpiri.py`는 없는 필드를 조용히 건너뛴다.
  그 결과 YAML의 L2 워크로드가 TT로 분류된다. `timpani-o/proto/schedinfo.proto`에서 스텁을 다시 생성한 뒤 실행한다.
- 격리 CPU를 찾지 못하면 timpani-n은 FATAL을 찍고 끝난다. 격리 CPU와 cgroup을 만드는 절차는 저장소 문서에 없다.
- 앱 코드의 workload ID(`radar_workload`, `fcw_workload`)와 YAML의 `workload_id`가 다르다.
  바인딩은 스레드 이름(`tt_task`, `cbs_task`)으로 하므로 동작에는 영향이 없다.
- 예제는 `timpani_rt` 안에 `libttsched.c`를 따로 컴파일한다. `libttsched.so`만 바꿔서는 예제에 반영되지 않는다. 예제를 다시 빌드한다.

**현행 기동 순서 [현행]**
1. 소비자(`fcw_consumer`)를 먼저 띄운다.
2. 생산자(`mock_radar_producer`)를 띄운다. 웨이크업 신호를 기다린다.
3. timpani-o를 띄운다.
4. timpani-n을 띄운다. 필요하면 기록 옵션을 준다.
5. `mock_pullpiri.py --config mock_pullpiri.yaml`을 마지막에 실행한다.

**P1 목표 [P1 제안]** 기동 순서는 TIMPANI-O, TIMPANI-N, 워크로드 등록 순이다. 앱은 이 전후 어느 때 떠도 된다([HLD-008 §8.4] (3)). 워커가 바인딩되기 전의 게시는 E_INACTIVE다.

README 일부는 낡았다.
`LD_LIBRARY_PATH=.`는 현행 빌드가 `timpani_rt`를 공유 라이브러리로 만들므로(`examples/someip/CMakeLists.txt` 17행) 지금도 필요하다. 정적 링크로 바뀌기 전에는 안내를 지우지 않는다.
본문에 있는 "section 5.3" 참조는 존재하지 않는 절을 가리킨다([HLD-008 §8.4] (2)). 이상 주입 표의 MIT와 wakeup 행은 `libtimpani_rt`의 로그다.
P1에서 그 판정은 사라진다.

### 8.3 P1 변경 (전후 비교)

| 항목 | 현행 | P1 제안 | 근거 |
|:--|:--|:--|:--|
| 라이브러리 | `libtimpani_rt` (`timpani_rt.h`, `/timpani_rt` SHM) | `libttsched` v2 (`ttsched_evt_*`), `/timpani_ttsched` v2 | [HLD-008 §8.4] (2) |
| 워크로드 MIT | 코드의 `timpani_evt_register(..., 18000)` | YAML `min_inter_arrival_us: 18000` (승인 뒤). 코드에는 없음 | [LLD-008 §7.2] |
| 도달 지점 | `timpani_evt_mark_arrival(svc, inst, evt, seq)` | `ttsched_evt_post(g_src, t_arr, target, sizeof(*target), &id)`, `t_arr`는 핸들러 첫 문장 | [LLD-008 §7.2] |
| 큐와 잠금 | `std::queue`, `std::mutex`, `std::condition_variable` | 공통 Event 큐. 워커에 잠금 없음 | [HLD-008 §8.4] (2) |
| 워커 루프 | 조건 변수 대기, 한 건 처리, 바인딩과 수면 | `ttsched_evt_wait`, 처리, `ttsched_evt_done` | [LLD-008 §7.2] |
| 바인딩 | 이벤트마다 bind와 unbind | 삭제. TIMPANI가 바인딩 | [LLD-008 §7.2] |
| 워크로드 주기 | `period` 20000 | `period` 18000 (T ≤ MIT) | [HLD-008 §8.4] (2) |
| 로컬 판정 | `OVER_BUDGET`, `DEADLINE_MISS` 문자열 | 보조 로그만. 판정은 TIMPANI 폴트와 Job 기록 | [HLD-008 §8.4] (2) |
| 지연 주입 | `--wakeup-delay-ms`: 바인딩 전 수면 | post 전 지연으로 옮김. 도달 뒤 지연은 D를 소진 | [LLD-008 §7.2] |
| 종료 | 조건 변수 `notify_all`, 워커 `join` | 워커 detach, 프로세스 종료에 맡김 | [LLD-008 §7.2] |

FCW 소스 수정의 상세 목록은 [HLD-008 §8.4] (2)에 있다. 수정 뒤 검증 기준은 같은 절 (3)에 있다.

### 8.4 현행 이상 주입 방법 [현행]

아래 옵션은 현행 코드에 있다(`fcw_consumer.cpp`, `mock_radar_producer.cpp`). 확인 위치는 현행 README 기준이다.

| 실행 | 소비자(fcw_consumer) | 생산자(mock_radar_producer) | 확인 위치 (현행) |
|:--|:--|:--|:--|
| 정상 | 기본 | 기본 | 이상 없음 |
| MIT 위반 | 기본 | `--burst-ms 5` | 소비자 로그의 `FAULT MIT_VIOLATION` 줄 (`libtimpani_rt`) |
| 깨움 지연 | `--wakeup-delay-ms 5` | 기본 | 소비자 로그의 `FAULT E2E_LATENCY_EXCEEDED` 줄 (`libtimpani_rt`). 제한 3 ms |
| 예산 초과 | `--overrun-ms 10` 또는 `-be [N]` | 기본 | timpani-n 로그의 `Fault threshold exceeded!` 줄. `FAULT_BUDGET_EXCEED`라는 문자열은 찍히지 않는다 |
| TT 마감 실패 | 기본 | `--tt-overrun-ms 25` | 생산자 출력 줄의 `DEADLINE_MISS` 표시(`mock_radar_producer.cpp` 213행). timpani-n의 `[FaultMonitor] Dmiss ...` 줄은 해시로 찍힌다. timpani-o 전달은 `max_dmiss`를 넘은 뒤다 |
| CBS 마감 실패 | `--overrun-ms 10` | 기본 | 소비자 출력 줄의 `DEADLINE_MISS` 표시(`fcw_consumer.cpp` 126행, 응답 시간이 5 ms를 넘을 때). timpani-n의 `Dmiss` 줄은 해시로 찍힌다 |

**참고.** `--burst-ms`는 생산자의 wake-up을 로컬 타이머로 바꾼다. MIT 경로만 시험한다. `-be [N]`의 대괄호는 선택 인자다. 생략하면 N은 30이고, 그 Job은 3 ms를 쓴다. 예산은 2 ms다.

**P1에서 같은 실험의 기대 결과 (설계상, 측정 전)** [HLD-008 §8.4] (3). 아래 시각표는 `-be [N]`의 경우다. 3 ms를 쓰는 Job 하나가 예산 2 ms, 보충 윈도우 18 ms에서 지나는 순서이며, 시각은 Job 시작 뒤 ms다.

| 시각 (ms) | 사건 |
|:--|:--|
| 0~2 | 예산 2 ms를 다 쓴 뒤에도 할 일이 남아 실행 가능하므로 스로틀에 들어간다. BUDGET_EXCEED 1회 |
| 5 | DeadlineMonitor가 완료 전에 마감 경과를 감지한다. DMISS(THROTTLED) |
| 18 | 보충 윈도우가 끝나 예산 2 ms가 찬다. 스로틀된 서버가 재개된다(승격) |
| 19 | 남은 1 ms를 쓰고 완료한다. `t_end` = 19 ms |

이 기대는 측정으로 확인하지 않았다.
- `--overrun-ms 10`: Job마다 10 ms를 쓰므로 Job 하나가 여러 윈도우(윈도우마다 2 ms)에 걸친다. Job마다 DMISS가 나고, 도달(20 ms)이 처리보다 빨리 쌓여 큐가 차면 E_FULL이 난다. 수락된 Event는 처리되거나 마감 실패로 기록된다.
- `--burst-ms 6`: MIT_VIOLATION이 계수된다. 이 모드는 L1 보호를 보는 시험이 아니다.

## 9. SOME/IP가 아닌 입력 (evttask, 계획)

**상태: 계획이다. `sample-apps/src/evttask.c`는 아직 없다.** 원천은 Unix datagram(`AF_UNIX`, `SOCK_DGRAM`)이다. datagram 한 건은 유닉스 소켓 메시지 한 건이며, 그것이 Event 한 건이다.
수신 스레드가 datagram을 읽은 직후 게시하고, 워커 `sensor_task`가 임계값을 비교한다 ([LLD-008 §7.1], [HLD-008 §8.5]).

**옵션 (제안).** `--role recv|send`, `--sock PATH`, `--period-us N`, `--burst N`, `--wcet-us N`, `--overrun-us N`.

| 항목 | FCW (`cbs_task`) | evttask (`sensor_task`) |
|:--|:--|:--|
| 원천과 입력 어댑터 | ObstacleAlert, vsomeip 핸들러 | Unix datagram, `recvfrom` 뒤 |
| C | 2000 µs | 500 µs |
| D (단독) | 5000 µs | 5000 µs |
| D (조합 b) | 6000 µs | 6000 µs |
| T | 18000 µs | 10000 µs |
| MIT | 18000 µs | 10000 µs |
| 배정 CPU | CPU 0 | (a) CPU 1, (b)(c) CPU 0 |

배정 기호: (a) 단독 배치, (b) 같은 CPU 0에 조합 파라미터(FCW의 D 6 ms), (c) 같은 CPU 0에 기본 FCW 파라미터와 두 번째 L2 태스크.

수용 산술 (후보 값으로 계산한 결과이며 측정이 아니다).
- (a) 서로 다른 격리 CPU: FCW는 2000 + 2000 + 100 = 4100 ≤ 5000이다. 센서는 500 + 0 + 100 = 600 ≤ 5000이다. 두 검사는 CPU마다 독립이다.
- (b) 같은 CPU 0, 조합 파라미터: FCW는 D 6000으로 올린다. 2000 + 2000 + 100 + (500 + 1000) = 5600 ≤ 6000이다. 센서도 5600 ≤ 6000이다. 여유는 400 µs다.
- (c) 기본 FCW 파라미터에 같은 CPU의 두 번째 L2 태스크: 5600 > 5000이므로 거부된다. TIMPANI-O는 `status -1`을 내고 이전 집합을 복원한다.

시험 기준은 다음과 같다. (a)는 한 앱을 폭주시켜도 다른 앱의 결과가 바뀌지 않아야 한다. (b)는 폭주 앱에 BUDGET_EXCEED와 자기 마감 실패가 나고, 다른 앱은 마감 실패가 없어야 한다. (c)는 거부되어야 한다.

## 10. P1 한계

| 한계 | 내용 | 출처 |
|:--|:--|:--|
| 게시 문맥 | source당 게시 문맥은 하나다. 라이브러리는 잠금이 없어 위반하면 그 source 큐가 깨질 수 있다. 여러 입력 스레드는 앱이 직렬화한다 | [LLD-008 §6.4], [C-08] |
| 워커 | source당 워커 하나다. 워커 풀은 P1에서 지원하지 않는다 | [HLD-008 §8.3] |
| source 수 | L2 태스크당 source는 하나다. 여러 종류는 태스크를 나눈다 | [REQ-008 §2.2], [OI-16] |
| payload | 64바이트 inline만 지원한다. 큰 payload는 P2다 | [F-13] |
| 데몬 중단 | 중단 중에는 마감을 실시간으로 감시하지 않는다. 재시작 뒤 DETECTED_LATE로 확인한다 | [LLD-008 §6.4] |
| 블로킹 Job | 수용식의 보장 밖이다. 마감은 감시한다 | [A-06], [H-17] |
| 대기 취소 | 대기 중 워커를 Event 없이 깨우는 API가 없다 | [L-8] |
| source 활성화 틈 | 워커가 바인딩된 뒤 source가 켜지기 전의 게시는 E_INACTIVE다. 열린 문제다 | [L-14] |
| 재등록 경계 | 계수 한 건이 이전 또는 새 등록에 귀속될 수 있다. 판정에는 영향이 없다 | [LLD-008 §9] |
| 게시자 사망 | 게시 뒤 게시자가 죽으면 그 Event는 처리되지만 `accepted` 계수 1건이 빠질 수 있다 | [LLD-008 §6.4] (A8) |
| 신뢰 | SHM에 쓸 수 있는 모든 프로세스를 신뢰한다. 격리는 P2다 | [C-09], [F-14] |
| 수치 | 모든 수치는 후보다. 측정 전에는 합격 기준으로 쓰지 않는다 | [REQ-008 §4.6] |
| 수용식 | 충분 조건이다. 증명이 아니다. 항의 값은 측정으로 확인한다 | [HLD-008 §3.6] |
| v1 앱 | 구 `libttsched`로 빌드한 앱은 다시 빌드해야 한다 | [LLD-008 §9] |

## 11. 자주 묻는 질문

**Q1. Event 한 건이 Job 한 건인가? 여러 건을 묶어 처리할 수 있나?**
A. 한 건이 한 Job이다. 묶지 않는다. 처리 중에 도달한 Event는 QUEUED로 쌓여 다음 wait에서 하나씩 받는다([REQ-008 §2.4] R2, R3).

**Q2. 워커가 소켓을 직접 읽으면 안 되나?**
A. 실행할 수는 있다. CBS 예산 집행은 그대로 유지된다. 하지만 Event 도달 시각, Job 기록, MIT 계수, Event 기준 마감 감시는 얻지 못한다. 수신 스레드를 나누고 게시하는 구조를 권장한다([HLD-008 §8.2] 판단표).

**Q3. 예산을 다 쓰고 잠들면 스로틀되나?**
A. 정확히 소진하고 잠드는 것만으로는 스로틀되지 않는다. 다음 Event로 깨어났을 때 보충 윈도우가 끝나지 않았고 잔여 예산이 0이면,
새 Job을 시작하기 전에 스로틀된다. 윈도우가 끝난 뒤 깨어나면 C로 보충된다. 윈도우 끝까지 남은 시간이 허용 오차 `tol` 이하일 때 깨어나도 끝난 것으로 본다([REQ-008 PRT-001], [DDR-008 D-04]). BUDGET_EXCEED는 실제 스로틀 진입을 알리는 정보성 값이다.

**Q4. 마감은 활성화 시각에서 D만큼 뒤인가?**
A. 아니다. 마감은 `t_arr + D`다. 활성화, 예산 보충, 재개, 워커 지연이 마감을 뒤로 옮기지 않는다([REQ-008 PRT-008]). 활성화 시각에 D를 더한 값은 마감이 아니다.

**Q5. 큐가 차면 어떻게 되나?**
A. 게시가 E_FULL을 돌려준다. 그 입력은 Event가 아니므로 앱이 버린 수를 세고 처리 정책을 정한다. 수락된 Event도 게시자 사망, 재등록, 데몬 중단 때는 계수나 감시가 빠질 수 있다(10장).

**Q6. Job 안에서 로그를 찍어도 되나?**
A. 권하지 않는다. 로그 출력은 블로킹이 될 수 있다. 가득 찬 파이프나 터미널 출력도 블로킹이다. Job 안에서는 로그를 출력하지 않는다. 필요한 값은 변수에 담아 done 뒤에 출력한다([HLD-008 §8.1] 6항).

**Q7. 워커를 여러 개 두어 Event를 나눠 처리할 수 있나?**
A. P1에서는 없다. source당 워커 하나만 쓴다. 검토 대상은 P2다.

**Q8. C는 어떻게 정하나? 처음에는 얼마로 두나?**
A. 측정으로 정한다. 방법은 4장 3단계와 [HLD-008 §8.2]에 있다. 측정 뒤 `budget_used_us`를 쓸 수 있다. 이 필드는 승인 전이다. FCW 시작값은 C 2 ms다. 이 값은 후보다.

**Q9. 앱이 MIT를 직접 검사해야 하나?**
A. 아니다. 앱은 자체 MIT 판정과 마감 판정을 두지 않는다. TIMPANI-N이 도달을 계수하고 보고한다([HLD-008 §8.1] 12항).

**Q10. 처리 시간이 D를 넘으면?**
A. 마감 실패(DMISS)다. DeadlineMonitor가 완료 전에 감지한다. 워커가 나중에 끝내도 LATE_COMPLETION 표식이 붙는다. Job은 한 번만 실패로 센다(7.4절, 7.5절).

**Q11. 데몬이 재시작하면 큐의 Event는?**
A. 재시작 전 객체의 Event는 정리되고 새 객체는 다른 `gen`을 쓴다([REQ-008 AC-008-15] (d)).
데몬이 내려간 동안의 마감은 실시간으로 감시하지 않으며, 재시작 뒤 DETECTED_LATE로 확인한다(10장). 데몬 하트비트와 SHM 격리는 미정(H-16)이다.

## 12. 참조 문서와 미정 사항

**참조 문서**
- 요구사항과 용어: [REQ-008-event-driven-workload-support.md](REQ-008-event-driven-workload-support.md)
- 시스템 요구사항: [SRS-008-event-driven-workload-support.md](SRS-008-event-driven-workload-support.md)
- 설계 결정: [DDR-008-event-driven-workload-support.md](DDR-008-event-driven-workload-support.md)
- 상위 설계: [HLD-008-event-driven-workload-support.md](HLD-008-event-driven-workload-support.md) (8장이 앱 적용 절차)
- 상세 설계: [LLD-008-event-driven-workload-support.md](LLD-008-event-driven-workload-support.md) (6장 API, 2장 proto 제안, 9장 실패 모드)
- 계약 보강 합의: [P1-008-event-job-contract-amendment.md](P1-008-event-job-contract-amendment.md)
- 개요: [OVERVIEW-008-event-driven-workload-support.md](OVERVIEW-008-event-driven-workload-support.md)
- 현행 예제: [examples/someip/README.md](../../../examples/someip/README.md)
- 현행 예제 소스: [fcw_consumer.cpp](../../../examples/someip/radar_based_fcw/fcw_consumer.cpp)
- 현행 예제 소스: [mock_radar_producer.cpp](../../../examples/someip/radar_based_fcw/mock_radar_producer.cpp)
- 현행 워크로드 YAML: [scripts/mock_pullpiri.yaml](../../../scripts/mock_pullpiri.yaml)
- 현행 샘플 앱: [sample-apps/README.md](../../../sample-apps/README.md), [sample-apps/src/libttsched.h](../../../sample-apps/src/libttsched.h)

**미정 사항 (이 가이드를 바꿀 수 있는 항목)**

| ID | 미정 내용 | 이 가이드에 주는 영향 |
|:--|:--|:--|
| H-8 | vsomeip 핸들러가 한 스레드에서만 불리는지 확인 | 4장 6단계 게시 직렬화, `t_arr` 순서, E_INVAL |
| H-10 | 완료 시각 `t_end`의 정의와 경합 허용 오차. 정의는 해석으로 두었고 감시·경합 허용 오차(OI-20)가 열려 있다 | 1.2절 Job 상태, 7.2 완료 판정 |
| H-11, OI-21 | TIMPANI가 닫은 Job(CANCELLED, ABANDONED)을 실패로 세지 않는 해석 | 7.2 CANCELLED와 ABANDONED 표 |
| H-14, OI-18 | 게시 비용, $L_{post}$, $L_{del}^{max}$, $\delta_{ovr}$, 400 µs 여유 측정 | 5장 호출 규칙과 9장 수용 산술 |
| H-15 | FCW 예제 설정 변경(period 18 ms, 조합 D 6 ms) 승인 | 4장 4단계와 8.3 표 |
| H-19, OI-23 | C 측정 절차와 여유율 규칙 | 4장 3단계 |
| H-20, OI-24 | 재바인딩 때 `t_arr` 기준(원래 도달 시각 또는 재게시 시각) | 4장 6단계, 7.1 |
| OI-19 | 공개 API, SHM v2 레이아웃, proto 확장의 Project Leader 승인과 카운터 노출 위치 | 전체 |
| H-12, OI-22 | DeadlineMonitor 스캔 비용과 지터 PoC | 7.3 감지 지연 |
| H-16 | 데몬 하트비트와 SHM 격리 | 10장 데몬 중단, 11장 Q11 |
| H-17 | P2에서 블로킹 Job 보장 방법 | 6장 블로킹 규칙 |
| L-8, L-14 | 대기 취소 API와 source 활성화 틈의 처리 | 5장, 10장 |

이 가이드는 설계 단계 문서다. 공개 API와 수치가 승인·측정되면 이 가이드를 개정한다.
