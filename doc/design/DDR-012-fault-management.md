<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.

SPDX-License-Identifier: Apache-2.0
-->

# DDR-012: Fault Management Architecture (Fault Monitor Integration v4)

**Date:** 2026-07-13  
**Last Updated:** 2026-07-15  
**Status:** Approved / Implemented (except the §4.2 `dmiss_counting_enabled_` event-transition gate, which is not present in `timpani26`)  
**Author:** Jaehyun Kim  
**Related:** DDR-002 (Scheduling Architecture), DDR-003 (Interface / Protocol), DDR-006 (Communication Architecture), DDR-011 (Runtime Table Update)

> **Status scope supplement (2026-11-13):** The new §4.4 is a **proposal for review** from the Weekly 2026-11-12 follow-up. It is not covered by the Approved / Implemented notation above. The existing implementation status and the agreement on the new cause and recovery policy are distinguished.

---

## 1. Overview

This document defines the **fault management path** implemented on `feat/fault-monitor-integration_v4`.
Its scope is limited to:

1. How `FaultMonitor` collects and reports deadline miss events without false positives
2. How recovery actions are triggered through `FaultAction` policy

---

## 2. Terminology Alignment (DDR-001 to DDR-011)

This document uses the same terminology as prior DDRs.

| Term | Canonical wording in this DDR | Reference |
|:--|:--|:--|
| Deadline miss | **Deadline miss (DMISS)** | DDR-003, DDR-005 |
| Fault report message | **FaultInfo / FaultNotification** | DDR-003 |
| Recovery action | **FaultAction.NOTIFY / RESTART / STOP** | DDR-003 |
| Timer component | **Timer Master** | DDR-002, DDR-005, DDR-011 |
| Epoch field | **epoch_ns** | DDR-003, DDR-004 |
| Period boundary | **Hyperperiod** | DDR-001, DDR-004, DDR-011 |
| User domain | **userspace** | DDR-002, DDR-006 |

To keep wording consistent, this DDR does not use variant labels such as `ACTION_STOP`, `TimerMaster`, or `user-space` for the same concepts.

---

## 3. Fault Management Data Path

```
sched_ext (kernel)
  fault_ringbuf (deadline miss / budget-related events)
        ↓
Fault Monitor (timpani-n, userspace)
  - reads current_limit
  - applies transition gate (dmiss_counting_enabled_)
  - emits FaultInfo only on threshold breach
        ↓
timpani-o
        ↓
Pullpiri
  - applies FaultAction policy (NOTIFY / RESTART / STOP)
```

Primary goals:

- Drop transition-phase noise to prevent false positives
- Report only threshold breaches for high signal quality
- Keep policy-driven recovery behavior explicit and deterministic

---

## 4. Design Decisions

### 4.1 Pre-Pass Threshold Synchronization (Before BPF Registration)

In older behavior, `current_limit` was filled after BPF registration, so immediate startup events could arrive before threshold state was initialized.

In v4, table application enforces this order:

1. Pre-register `current_limit` from all `TtSlot` and `CbsEntry` records
2. Register tasks/BPF state and start `sched_ext` execution

The snippet below is illustrative pseudo-code and is not intended to be a literal copy of the production implementation.
```cpp
// 1) Pre-Pass: collect current_limit before BPF attachment, then apply in bulk
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

// 2) Main Pass: register BPF state and enable scheduling
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

### 4.2 Event-Driven Transition Gate (`dmiss_counting_enabled_`)

Fixed warm-up windows (for example, 500 ms) were removed because they are non-deterministic under variable node timing.

The v4 gate is event-driven:

1. On table arrival: `set_dmiss_counting_enabled(false)`
2. After Timer Master timing alignment: `timing_ready_callback_()`
3. In callback: `set_dmiss_counting_enabled(true)`

This ensures transition events are ignored and steady-state events are evaluated.

### 4.3 Normalized Report Rule

Fault reporting checks the threshold only when counting is enabled:

$$
\text{report} \iff (\text{dmiss\_count} > \text{current\_limit})
$$

Misses at or below threshold are treated as tolerated jitter and do not emit `FaultInfo`.

---

### 4.4 2026-11-12 Weekly Follow-up — Separating Affected Task and Causing Task (for review)

**Status:** This is a policy review requirement that reflects the [meeting follow-up answer](https://github.com/eclipse-timpani/timpani26/discussions/4). The Approved / Implemented status in the document header does not apply to this new section. It does not mean that cause analysis or a recovery policy has been implemented or approved yet.

- Even if CBS misses its deadline because of TT overrun execution, the **fact** of the CBS deadline miss is recorded. The failed task and the task or platform that provided the cause are distinguished, and the policy is set in a direction that does not automatically conclude that CBS itself has a fault and penalize it.
- **Phase distinction (scope decision of the Project Leader (author) during design review, see DDR-008 §3.5 for the basis):** P1 implements the common Event/Job execution environment and deadline-miss detection, counting, and basic reporting. Existing observation information is kept, but TT interference cause analysis, new control-delay measurement, and cause-aware recovery are not added to the P1 completion criteria. The threshold excess in the existing §4.3 is a reporting condition, not a rule for confirming the cause.
- **P2 decision:** The notification for the case where the threshold is exceeded with the cause unconfirmed or with evidence of TT interference, and the handling of RESTART/STOP, are designed together with the distinction between the affected task and the causing task. Causality is not confirmed by simultaneous observation of TT overrun and budget exhaustion alone. The agreement on the scope and latency of basic P1 reporting is kept, and this section does not arbitrarily disable the existing recovery or add a new FaultAction.
- Cause classification, multi-cause analysis, and cause-specific recovery selection are **P2**, and their scope is traced in [DDR-008 D-23](DDR-008-event-driven-workload-support.md#d-23-separation-of-the-deadline-failure-fact-from-cause-and-recovery-policy--p2). The TT boundary enforcement itself is the responsibility of [DDR-007 §4.3.1](DDR-007-tt-cbs-integrated-scheduling.md#431-tt-slot-end-enforcement--2026-11-12-weekly-follow-up-design) and DDR-005 in P2.
- **Verification (not run):** Distinguish normal TT and self-overrunning CBS, affected CBS by overrunning TT, Timer Master delay, and cases where multiple causes overlap. Confirm that the unconfirmed cause state is expressed without hiding observation failures, and that recovery is not applied to the wrong task. Proto changes are subject to separate approval.

---

## 5. FaultAction.STOP Integration Point

This DDR defines the **fault integration point** for STOP policy:

- Pullpiri applies `FaultAction.STOP` for a target workload
- timpani-o/timpani-n transition that workload into removal and cleanup flow

Detailed STOP sequence ownership remains in:

- Interface/policy fields: DDR-003
- Runtime table replacement and cleanup: DDR-011
- Transport and delivery structure: DDR-006

### 5.1 Implemented Recovery & Fault-Forward Path (`timpani26`)

- **Recovery enforcement**: Pullpiri calls `RecoveryService.EnforceRecoveryPolicy(RecoveryCommand{workload_id, RECOVERY_STOP})` on TIMPANI-O. `RecoveryServiceImpl::EnforceRecoveryPolicy` removes the workload and broadcasts `RecoverySignal{ACTION_STOP}` to the owning TIMPANI-N over `OrchestratorService.NodeStream`. The node evicts the workload via `TimerMaster::remove_workload()` + `BpfLoader::delete_tt_slot()/delete_cbs_state()/delete_task_meta()` (DDR-011 §3.2).
- **Fault forwarding (O → Pullpiri)**: `FaultServiceClient` forwards fault events to Pullpiri's `FaultService.NotifyFault`. On send failure it enqueues to a bounded in-memory `retry_queue_` (deque) and flushes on the next send; when the queue reaches `kMaxRetryQueue` the oldest event is dropped (verification/debug fault events are best-effort, not guaranteed delivery).

---

## 6. Module Summary

| Component | File / Module | Responsibility from fault-management perspective |
|:--|:--|:--|
| **Protobuf** | `proto/node_control.proto`<br>`proto/schedinfo.proto` | `FaultAction`, `FaultInfo`, and `current_limit`-related fields |
| **Timpani-N** | `src/fault_monitor.h/.cpp` | Pre-pass threshold sync, event gate, threshold-breach decision |
| **Timpani-N** | `src/main.cpp` | Gate on/off wiring around table application and timing callbacks |
| **Timpani-O** | `src/recovery_service.h/.cpp` | Fault intake and policy-driven control path continuation |

---

## 7. Verification Summary

### 7.1 Automated Tests

`ctest`-based suites passed with no observed regressions on the fault path:

- `SchedInfoServiceTest`
- `FaultServiceClientTest`
- `NodeConfigTest`
- `GlobalSchedulerTest`
- `HierarchicalTableBuilderTest`
