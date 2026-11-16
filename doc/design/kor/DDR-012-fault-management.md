<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.

SPDX-License-Identifier: Apache-2.0
-->

# DDR-012: Fault Management 아키텍처 (Fault Monitor Integration v4)

**작성일:** 2026-07-13  
**최종 수정:** 2026-07-15  
**상태:** 승인 / 구현 완료 (Approved / Implemented) — 단, §4.2 `dmiss_counting_enabled_` 이벤트 전이 게이트는 `timpani26`에 없음  
**작성자:** 김재현 (Jaehyun Kim)  
**관련 문서:** DDR-002 (Scheduling Architecture), DDR-003 (Interface / Protocol), DDR-006 (Communication Architecture), DDR-011 (Runtime Table Update)

> **상태 범위 보충(2026-11-13):** 신규 §4.4는 Weekly 2026-11-12 후속 **검토용 제안**이다. 위 승인/구현 완료 표기에 포함되지 않는다. 기존 구현 상태와 새 원인·복구 정책의 합의를 구분한다.

---

## 1. 개요

본 문서는 `feat/fault-monitor-integration_v4` 브랜치에서 구현된 **Fault Management 경로**를 정의한다.  
핵심 범위는 아래 두 가지다.

1. Fault Monitor가 Deadline miss 이벤트를 오탐지 없이 수집하고 보고하는 경로
2. Fault 정책(`FaultAction`)에 따라 복구 액션을 트리거하는 경로

---

## 2. 용어 통일 (DDR-001~011 기준)

본 문서는 기존 DDR 용어를 다음과 같이 따른다.

| 용어 | 본 문서 표기 | 기준 문서 |
|:--|:--|:--|
| 데드라인 미스 | **Deadline miss (DMISS)** | DDR-003, DDR-005 |
| 장애 보고 메시지 | **FaultInfo / FaultNotification** | DDR-003 |
| 복구 액션 | **FaultAction.NOTIFY / RESTART / STOP** | DDR-003 |
| 타이머 컴포넌트 | **Timer Master** | DDR-002, DDR-005, DDR-011 |
| 기준 시각 필드 | **epoch_ns** | DDR-003, DDR-004 |
| 반복 주기 | **Hyperperiod** | DDR-001, DDR-004, DDR-011 |
| 사용자 영역 | **userspace** | DDR-002, DDR-006 |

용어 일관성을 위해 본 문서에서는 `ACTION_STOP`, `TimerMaster`, `유저 공간`, `DMISS-only` 같은 변형 표현을 사용하지 않는다.

---

## 3. Fault Management 데이터 경로

```
sched_ext (kernel)
  fault_ringbuf (Deadline miss / budget 관련 이벤트)
        ↓
Fault Monitor (timpani-n, userspace)
  - 임계값(current_limit) 조회
  - 과도기 게이트(dmiss_counting_enabled_) 적용
  - 조건 충족 시 FaultInfo 생성
        ↓
timpani-o
        ↓
Pullpiri
  - FaultAction 정책 적용 (NOTIFY / RESTART / STOP)
```

핵심 목표는 다음과 같다.

- 과도기 이벤트를 무시하여 오탐지 차단
- 임계값 초과에 대해서만 보고하여 신호 품질 유지
- 정책 기반 복구 동작과의 명확한 인터페이스 유지

---

## 4. 설계 결정

### 4.1 Pre-Pass 임계값 동기화 (BPF 등록 이전)

기존에는 BPF 등록 후 `current_limit`를 채우는 순서였고, 이 구간에서 Deadline miss가 먼저 도착하면 오탐지가 발생할 수 있었다.

v4에서는 스케줄 테이블 적용 시 아래 순서를 강제한다.

1. `TtSlot` / `CbsEntry`의 `current_limit`를 Fault Monitor에 선등록
2. 이후 task/BPF map 등록 및 `sched_ext` 집행 시작

아래 예시는 설명용 pseudo-code이며, 실제 구현을 그대로 복사한 코드가 아님을 명시한다.
```cpp
// 1) Pre-Pass: current_limit 수집 후 일괄(bulk) 등록
std::map<std::pair<uint64_t, uint64_t>, uint32_t> new_limits;
for (const auto& partition : table.partitions()) {
    for (const auto& layer : partition.layers()) {
        for (const auto& tt_slot : layer.tt_slots()) {
            if (tt_slot.current_limit() > 0)
                new_limits[{tt_slot.workload_id_hash(), tt_slot.task_id_hash()}] = tt_slot.current_limit();
        }
        for (const auto& cbs_entry : layer.cbs_entries()) {
            if (cbs_entry.current_limit() > 0)
                new_limits[{cbs_entry.workload_id_hash(), cbs_entry.task_id_hash()}] = cbs_entry.current_limit();
        }
    }
}
fault_monitor.update_current_limits(new_limits);  // bulk setter (FaultMonitor::update_current_limits)

// 2) Main Pass: BPF 등록 및 스케줄링 적용
for (const auto& partition : table.partitions()) {
  for (const auto& layer : partition.layers()) {
    for (const auto& tt_slot : layer.tt_slots()) {
      bpf_loader.upsert_task_meta(tt_slot.task_id(), tt_slot.workload_id());
      bpf_loader.upsert_tt_slot(tt_slot);
    }
    for (const auto& cbs_entry : layer.cbs_entries()) {
      bpf_loader.upsert_task_meta(cbs_entry.task_id(), cbs_entry.workload_id());
      bpf_loader.upsert_cbs_budget(cbs_entry);
    }
  }
}

timer_master.apply_schedule_table(table);
timer_master.start_or_update_dispatch_loop();
```

### 4.2 이벤트 기반 과도기 게이트 (`dmiss_counting_enabled_`)

고정 시간 warm-up(예: 500ms) 방식은 결정론이 낮아 제거했다.  
대신 Timer Master 정렬 완료 이벤트와 연동한 게이트를 사용한다.

1. 새 테이블 수신 직후: `set_dmiss_counting_enabled(false)`
2. Timer Master가 새 기준 시각 정렬 완료 후: `timing_ready_callback_()`
3. 콜백에서 카운팅 재활성화: `set_dmiss_counting_enabled(true)`

이 방식으로 과도기 동안의 이벤트는 차단되고, 정렬 완료 이후 이벤트만 집계된다.

### 4.3 보고 조건 정규화

Fault Monitor는 카운팅 활성 상태에서만 아래 조건을 검사한다.

$$
\text{report} \iff (\text{dmiss\_count} > \text{current\_limit})
$$

즉, 임계값 이내 지터는 노이즈로 처리하고 초과 시에만 `FaultInfo`를 상위로 전달한다.

---

### 4.4 2026-11-12 Weekly 후속 — 피해 태스크와 원인 태스크 분리 (검토용)

**상태:** [회의 후속 답변](https://github.com/eclipse-timpani/timpani26/discussions/4)을 반영한 정책 검토 요구다. 문서 머리말의 승인/구현 완료는 이 신규 절에 적용되지 않는다. 아직 원인 분석·복구 정책이 구현되거나 승인되었다는 의미가 아니다.

- TT 초과 실행으로 CBS가 마감을 놓쳤더라도 CBS의 마감 실패 **사실**은 기록한다. 실패한 태스크와 원인을 제공한 태스크·플랫폼을 구분하며, CBS 자체의 결함으로 자동 단정해 불이익을 주지 않는 방향으로 정책을 정한다.
- **단계 구분(2026-11-13 범위 합의):** P1은 공통 Event/Job 실행 환경과 마감 실패 감지·계수·기본 보고를 구현한다. 기존 관측 정보는 유지하지만 TT 간섭 원인 분석·새 제어 지연 계측·원인 인지형 복구는 P1 완료 조건으로 추가하지 않는다. 기존 §4.3의 임계 초과는 보고 조건이지 원인 확정 규칙이 아니다.
- **P2 결정:** 임계 초과 시 원인 미확정·TT 간섭 증거가 있는 경우의 통지 및 RESTART/STOP 취급, 피해 태스크와 원인 태스크의 구분을 설계한다. TT overrun·예산 소진의 동시 관측만으로 인과를 확정하지 않는다. P1 기본 보고의 범위·지연 합의는 유지하며 이 절은 기존 복구를 임의로 비활성화하거나 새 FaultAction을 추가하지 않는다.
- 원인 분류·다중 원인 분석·원인별 복구 선택은 **P2**이며 [DDR-008 D-23](DDR-008-event-driven-workload-support.md#d-23-마감-실패-사실과-원인복구-정책의-분리--p2)에서 범위를 추적한다. TT 경계 집행 자체도 P2의 [DDR-007 §4.3.1](DDR-007-tt-cbs-integrated-scheduling.md#431-tt-슬롯-종료-강제--2026-11-12-weekly-후속-설계)과 DDR-005의 책임이다.
- **검증(미실행):** 정상 TT와 자체 초과 CBS, 초과 TT의 피해 CBS, Timer Master 지연, 복수 원인이 겹친 경우를 구분한다. 관측 실패를 숨기지 않으면서 원인 미확정 상태를 표현하고 잘못된 태스크에 복구를 적용하지 않는지 확인한다. proto 변경은 별도 승인 대상이다.

---

## 5. FaultAction.STOP 연동

본 DDR에서 정의하는 범위는 STOP 액션의 **fault 연동 지점**이다.

- Pullpiri가 `FaultAction.STOP` 정책을 적용하면 대상 워크로드 제거 흐름으로 진입
- timpani-o/timpani-n은 STOP 대상 워크로드의 스케줄 항목 및 런타임 상태를 정리

STOP 세부 시퀀스(테이블 갱신, BPF map 정리, 경계 동기화)는 아래 문서가 기준이다.

- 인터페이스/정책 필드: DDR-003
- 런타임 테이블 교체 및 정리: DDR-011
- 통신 전달 구조: DDR-006

### 5.1 구현된 Recovery 및 Fault-Forward 경로 (`timpani26`)

- **Recovery 집행**: Pullpiri가 TIMPANI-O에 `RecoveryService.EnforceRecoveryPolicy(RecoveryCommand{workload_id, RECOVERY_STOP})`를 호출. `RecoveryServiceImpl::EnforceRecoveryPolicy`가 워크로드를 제거하고 `RecoverySignal{ACTION_STOP}`을 `OrchestratorService.NodeStream`으로 해당 TIMPANI-N에 broadcast. 노드는 `TimerMaster::remove_workload()` + `BpfLoader::delete_tt_slot()/delete_cbs_state()/delete_task_meta()`로 워크로드를 evict (DDR-011 §3.2).
- **Fault 전달 (O → Pullpiri)**: `FaultServiceClient`가 fault 이벤트를 Pullpiri의 `FaultService.NotifyFault`로 전달. 전송 실패 시 bounded in-memory `retry_queue_`(deque)에 넣고 다음 전송 때 flush; 큐가 `kMaxRetryQueue`에 도달하면 가장 오래된 이벤트를 drop (검증/디버그용 fault 이벤트는 best-effort이며 전달 보장이 아님).

---

## 6. 구현 모듈 요약

| 컴포넌트 | 파일 / 모듈 | Fault Management 관점 역할 |
|:--|:--|:--|
| **Protobuf** | `proto/node_control.proto`<br>`proto/schedinfo.proto` | `FaultAction`, `FaultInfo`, `current_limit` 연관 필드 |
| **Timpani-N** | `src/fault_monitor.h/.cpp` | Pre-Pass limit 동기화, 이벤트 게이트, 임계값 초과 판정 |
| **Timpani-N** | `src/main.cpp` | 테이블 수신 시 fault 게이트 on/off 및 콜백 연결 |
| **Timpani-O** | `src/recovery_service.h/.cpp` | Fault 수신 후 정책에 따른 제어 흐름 연결 |

---

## 7. 검증 요약

### 7.1 자동화 테스트

`ctest` 기준 주요 테스트가 정상 통과했고, fault 경로 회귀가 없음을 확인했다.

- `SchedInfoServiceTest`
- `FaultServiceClientTest`
- `NodeConfigTest`
- `GlobalSchedulerTest`
- `HierarchicalTableBuilderTest`
