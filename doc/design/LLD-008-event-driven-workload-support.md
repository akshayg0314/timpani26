<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# LLD-008: Event-Driven Workload Support — Low-Level Design

**Date:** 2026-11-11
**Status:** Draft (v5.3 — P1 Event–Job contract alignment (Issue #53): one Event–one Job, explicit Job boundary, arrival-based deadline and DeadlineMonitor, kernel execution-interval observation. All of §1–§12 are aligned. v5.1 reflected D-22~D-24 and review comments. v5.3 cleaned up the poster and producer terms and corrected the final review findings) — premised on D-01~D-24 of DDR-008 (Proposal)
**Author:** Human + AI
**Parent documents:** [REQ-008](REQ-008-event-driven-workload-support.md), [SRS-008](SRS-008-event-driven-workload-support.md), [DDR-008](DDR-008-event-driven-workload-support.md), [HLD-008](HLD-008-event-driven-workload-support.md), [P1 contract amendment agreement](P1-008-event-job-contract-amendment.md)
**Baseline code:** `timpani26` HEAD `4201573` (`timpani-o/`, `timpani-n/`, `sample-apps/`, `examples/someip/`)

> This document expands the components of HLD-008 to the **file, struct, and function level**. The code blocks are sketches for implementation guidance, and identifiers and field numbers use this document as the single source. All numeric values (candidate values) and changes to the public API, SHM layout, and protobuf fields are **proposals** and are subject to Project Leader approval. It does not claim that implementation or verification is complete.
> Coding rules: Follow the existing repository style (C++17, `TLOG_*`, SPDX header, 4-space) and `.github/copilot-instructions.md` (no gRPC or allocation on the RT path, proto changes require approval).

---

## Revision History

| Version | Date | Changes |
|:--|:--|:--|
| v4 | 2026-11-10 | Common Event/Job model, independent review and verification reflected, FCW fix sketch extended, Phase 1 scope |
| v5 | 2026-11-11 | Revised §1–§12 to align with the P1 Event–Job contract amendment agreement (Issue #53, `P1-008-event-job-contract-amendment.md`, P1 design brief v3). In §6–§12, updated the libttsched v2 contract and implementation sketch, the interleaving rationale and P1 limitations, the samples (evttask, FCW), timing candidates and measurement method, failure modes, the AC-008-01~16 mapping, WBS, and open items (L-7 withdrawn, L-9~L-23 added). Changed to one Event–one Job and the explicit Job boundary, added the arrival-based deadline (`t_arr + D`) and DeadlineMonitor, SHM v2 slot with Event record ring and Job log, libttsched post, Event wait, and done contract, reduced BPF to kernel execution-interval observation (MIT check and L2 DMISS determination removed), single admission formula (`L_del_max`, `δ_ovr`, `T_i ≤ MIT_i`, `D_i ≤ MIT_i`), unified the FaultInfo flag table, and marked proto changes as approval required. The adapter K/M distinction, batch count, and activation-based DMISS were retired |
| v5.1 | 2026-11-16 | Reflected the design chain review. To align with DDR-008 D-22 (CBS execution on CPUs without TT) and D-24 (confirmation of the allowed CPU set), added FR-O-022, FR-B-025, and FR-N-032~033 to the §1 mapping. Corrected `DEFAULT_HP_US` in §3.1 and §3.3 to a table-format value (the wording change is OI-25), added the placement on CPUs without TT in §3.3, and added `TtFreeCpuAdmitsL2WithZeroBtt` in §3.5. Added the allowed CPU check (FR-N-033) to the binding sketch in §5.1, and stated that the BPF dispatch and replenishment and monitoring in §4 and §5 do not depend on TT events. Added the C measurement option for evttask in §7, AC-008-17~19 in §10, WBS in §11, and L-24~L-25 in §12. This is a design proposal, and the API, protobuf, and SHM fields are not changed |
| v5.2 | 2026-11-18 | 2026-11-17 changes: unified the Korean wording of the admission terms and of the arrival term, and the English terms admission, admission formula and arrival are unchanged. The budget-only L2 class description was replaced with a division of responsibility between CBS budget management and Event/Job management. No class or configuration flag is kept (D-21, decided by Project Leader (author)). Job start is confirmed inside wait, and no separate public acquire API is provided. Added the execution context (D-24) and the timer alternatives comparison (F-10, OI-22). 2026-11-18 review correction: changed the copy target of `take()` to the caller's `ev` buffer and restored the sentence that does not depend on application classification. The formal documents are REQ-008 v5.2, SRS-008 v4.2, DDR-008 v3.3, HLD-008 v5.3, and LLD-008 v5.2. The terms in the earlier history rows were also changed to the new terms. The $L_{post}$ upper bound (TIM-011) is kept as a requirement that TIMPANI upholds under the reference poster configuration, and the assumption that deployment satisfies that configuration was added as A-10 (Project Leader (author) decision). The starting point of $L_{post}$ was unified to $t_{arr}$ |
| v5.3 | 2026-11-26 | Term clean-up (Project Leader (author) decision): the side that calls post is the poster, and the side that creates the input to be posted to an Event source is the producer. "Producer" where it meant the thread that calls post was changed to "poster", and "posting thread" to "poster thread" (reference producer configuration → reference poster configuration, multi-producer posting → multi-poster posting). The code comment "reference producer configuration" was changed to "reference poster configuration". "Producer" for the writer of the monitor record queue and for the FCW producer, and "producer" in the code comments for the worker that writes the Job log, are unchanged. Final review corrections: in §3.3, the condition that holds automatically for a task that declares no MIT was limited to $T_i \le MIT_i$. In §5.6 and the slot structure comment, the `counter` of an EVT slot is now stated to be incremented by an accepted post as well. Other design content and numbers are unchanged. The terms in the earlier history rows were also changed to the new terms. The formal documents are REQ-008 v5.3, SRS-008 v4.3, DDR-008 v3.4, HLD-008 v5.4, and LLD-008 v5.3 |

---

## Summary

- **Model**: One accepted Event is one Job. The worker explicitly starts a Job (`ttsched_evt_wait`) and explicitly ends it (`ttsched_evt_done`). The absolute deadline is the arrival time + D and is fixed at acceptance. The Event lifecycle is QUEUED → RUNNING → COMPLETED/DISCARDED, and CANCELLED and ABANDONED, which TIMPANI closes, are kept as separate results.
- **proto (approval required)**: `schedinfo` has `TaskInfo.min_inter_arrival_us=11`, `FaultType` DMISS/BUDGET_EXCEED/MIT_VIOLATION (WATCHDOG reserved), `FaultInfo` `timestamp_ns=6`, `count=7`, `flags=8`. `node_control` has `CbsConfig.min_inter_arrival_us=9`, `cpu=10`, `FaultInfo` `count=10`, `flags=11`, `job_seq=12`, `job_gen=13`. Before approval, reports carry only the count.
- **timpani-o**: Validates `0 < T_i ≤ MIT_i` and `D_i ≤ MIT_i` for L2 tasks. The per-CPU admission applies the same formula to all L2 tasks, `C_i + B_tt(k, D_i) + L_del_max + Σ_{j≠i}(C_j + δ_ovr) ≤ D_i` (`L_del_max` candidate 100 µs, `δ_ovr` candidate 1 ms). It lists the eight assumptions of the sufficient condition. If the FCW task is placed on the same CPU as a second L2 task, the default FCW parameters are rejected.
- **timpani-n**: TaskRegistry activates the slot at binding time and puts `bind_gen` into the BPF TaskMeta and the close records. It ignores stale close records. TimerMaster SHM v2 has, per slot, an Event record ring (depth 8, record 128 B) and a Job log (32 entries, 48 B) (slot 2752 B, 64 slots candidate, object about 172 KiB). Activation (protocol 1), deactivation and the close rule (protocol 5), and startup cleanup (protocol 7) use seq_cst atomic operations. The new DeadlineMonitor scans at a 1 ms period (candidate) on a SCHED_FIFO thread and marks the elapsed deadline of QUEUED and RUNNING records with CAS. FaultMonitor is split into a counting path (lossless) and a detail path (best effort). EventSourceMonitor reads the cumulative counts. JobTraceCollector reads the Job log and the execution-interval records. Replenisher is not changed.
- **BPF**: Performs only execution-interval observation and enforcement (CBS budget, CPU pinning, activation-order queue, CARRYOVER and THROTTLED flags). It removes the activation-based MIT check, the `FAULT_MIT_VIOLATION` emission, the L2 DMISS determination, and early DMISS. The TT DMISS path (DDR-012) is kept as is.
- **libttsched v2 (approval required)**: `ttsched_evt_open`, `ttsched_evt_post` (accepts the arrival time and payload inline), `ttsched_evt_wait` (starts one Job), `ttsched_evt_done` (mandatory done). There is one posting context and one worker per source. The race rules for posting steps (a)–(f) and for take, done, and close are written in §6.3. The public API and SHM layout are subject to approval (§12 L-9, L-10).
- **Samples and libttsched details (§6–§7)**: `evttask` uses the same API from a timer, datagram, pipe, and self-generated source. In FCW, the receive callback posts `RadarTarget` inline and the worker ends the Job. There is no notification API, batch count, or ring and invalid-mark convention.
- **Timing and failure modes (§8–§9)**: The detection upper bound, reporting upper bound, arrival latency upper bound, and cost are all candidate values before measurement. The measurement method is also described. The failure-mode table includes post rejection, queue full, the death of the poster and the worker, daemon restart, monitoring stop, and record overflow.
- **Tests**: Adds GTest for timpani-o and creates a GTest target for timpani-n. It is designed so that `job_fsm.h`, the SHM protocol race, and the libttsched race tests run on the host (§5.8, §5.9, §6.5). AC-008-01~19 are mapped to tests or procedures (§10). It makes no claim that they were executed or verified.
- **Figures**: Detection latency, reporting latency, `L_del_max`, `δ_ovr`, queue depth, and payload size are all candidate values, and they require agreement and measurement on a Linux target.

---

## 1. Purpose and Corresponding Requirements

| Area | SRS-008 ID | Section in this document |
|:--|:--|:--|
| Protocol | IF-PO-001/004/005/006, IF-ON-001~004/006 | §2 |
| TIMPANI-O | FR-O-001/003/005/010~016/020/022 | §3 |
| BPF | FR-B-001~005/010~017/022~023, FR-B-024/025 | §4 |
| TIMPANI-N | FR-N-002~007/010~015/020~022/024, FR-N-027~033 | §5 |
| Common model and three-layer structure | FR-G-001/002, FR-G-003 | §1.1, §1.2 |
| Library | FR-L-001~012, IF-L-001~004 | §5.6(SHM layout), §6 |
| Sample app | FR-G-001, FR-L-006 | §7 |
| Timing | TMG-001~016 | §8 |
| Verification interface | IF-V-001~004, FR-N-026, OBS-004 | §5.4, §10 |
| Non-functional | NFR-001(capacity), NFR-002(BPF repetition upper bound), NFR-003(TT no regression), NFR-005(proto compatibility) | §4.1, §4.5, §8, §2.3 |
| Test | NFR-006/007, AC-008-* | §5.8, §5.9, §10 |

### 1.1 Common Model ↔ Implementation Element Correspondence

The distinction between "adapter K/M" in the earlier draft (two equivalent ways of acquiring an Event) is retired. An Event is created only through the explicit connection (libttsched Event queue), and kernel observation (BPF) only observes execution intervals and does not create an Event.

| Model element (REQ-008 §2, MOD-009/010/011) | Implementation element | Layer | Section |
|:--|:--|:--|:--|
| Event source S (`task_id`, L2 task 1:1) | `CbsConfig{task_id, min_inter_arrival_us, deadline_us}` → SHM slot `name`, `mit_us`, `deadline_us` | Explicit connection | §2.2, §3.1, §5.6 |
| Event identifier `(gen, seq)` | Record `gen` (registration generation), `seq` (value of `tail` at post time) | Explicit connection | §5.6, §6.2 |
| Event arrival time `t_arr`, absolute deadline `t_dl = t_arr + D` | Record `t_arr_ns`, `t_dl_ns` (written once at acceptance, immutable afterward) | Explicit connection | §5.6 |
| Arrival (MIT target) | `arrivals` and `mit_viol` counts of step (c) of `ttsched_evt_post()` | Explicit connection | §5.6, §6.2 |
| Acceptance rejection (E_INACTIVE, E_FULL) | Rejection before posting, `overflow` count | Explicit connection | §5.6, §6.2 |
| Worker Job start (R1–R3) | QUEUED → RUNNING transition of `ttsched_evt_wait()`, `t_begin`, `started` | Explicit connection | §5.6, §6.2 |
| Worker Job end (COMPLETED / DISCARDED) | End transition of `ttsched_evt_done()`, `t_end`, Job log | Explicit connection | §5.6, §6.2 |
| Close (CANCELLED / ABANDONED, close by TIMPANI) | Close rules of TimerMaster deactivation, activation, and startup cleanup | TIMPANI-N | §5.1, §5.6 |
| Deadline-miss detection (MISSED, once per Job) | Periodic scan by `DeadlineMonitor`, CAS, `missed++` | TIMPANI-N monitoring | §5.9 |
| Execution interval (kernel activation → going to sleep) | BPF `runnable` / `quiescent` → `JobRecord` (execution-interval record) | Kernel observation (BPF) | §4.3, §4.4, §5.4(a) |
| Replenishment window (D-04) | `CbsState.replenish_at_ns`, `cbs_on_activation()`, `cbs_try_promote()` | Kernel enforcement (BPF) | §4.3, §4.4 |
| CPU pinning, activation order, and throttling | `cbs_map`, per-CPU DSQ (ordered by `queue_key_ns`) | Kernel enforcement (BPF) | §4.2, §4.4 |
| Registration generation and binding | Slot `gen` (u64), `bind_gen` (TaskMeta, execution-interval and close records) | TIMPANI-N | §4.1, §5.2, §5.6 |
| Event source statistics | Slot cumulative counts → read by `EventSourceMonitor` | TIMPANI-N | §5.4(b) |
| Job record (including deadline and identification) | SHM Job log (produced by the worker) + close record of TIMPANI-N | Explicit connection + TIMPANI-N | §5.4(a), §5.6 |

### 1.2 Boundary Between Concepts and Mechanisms (model-first)

Common terms follow [REQ-008 §2](REQ-008-event-driven-workload-support.md#2-conceptual-model-common-eventjob-model). **CBS budget management** and **Event/Job management** are complementary responsibilities of the same L2 worker, and they are not classes or modes of an application. The L2 classification of DDR-001 and the compatibility with the existing optional connection are not changed.

- **Concepts (REQ-008 §2)**: Event source, Event, Job, arrival time, absolute deadline, lifecycle state, deadline miss, execution interval. Their definitions are independent of the OS, the scheduler, and the library.
- **Mechanisms (this document)**: futex, CAS, SHM layout, BPF callbacks, and API names are means of implementing the concepts. The concept definitions are not changed to fit the mechanisms.
- **Rules (REQ-008 MOD-009/010, DET-007, PRT-008/009)**:
  - R1: An Event accepted while the worker is waiting immediately becomes the target of a Job.
  - R2: An Event accepted while a Job is running stays QUEUED and is not merged into the Job in progress.
  - R3: When a Job has ended and a QUEUED Event exists, the worker starts the next Job with the oldest Event without going to sleep. Consecutive Jobs are different Jobs.
  - R4: One Job consumes one Event. Each source is FIFO.
  - I1 (invariant): Every accepted Event reaches exactly one terminal state, and Jobs and accepted Events correspond 1:1.
- **MIT**: A constraint on arrivals at the adapter boundary. A post that enters the MIT count ends in one of three ways: acceptance, rejection (queue full), or rollback (REVOKED). A post to an inactive source is not counted as an arrival. An arrival that violates the MIT is also accepted and placed in the queue, and the violation is counted and reported. **There is no MIT check based on activation** (A-04 withdrawn).
- **Three-layer structure**: (1) model, (2) explicit connection and kernel observation. The explicit connection (libttsched Event queue) is the only path that provides Events, Jobs, and deadlines, and kernel observation (BPF) observes the execution intervals and budgets of all L2 tasks. (3) enforcement: CBS, CPU, and TT are handled by BPF, deadline monitoring and reporting by TIMPANI-N, and admission by TIMPANI-O.
- **Responsibility and observation scope (MOD-011, D-21)**: Even without the explicit Event connection, CBS budget management, CPU pinning, BUDGET_EXCEED, and execution-interval recording are kept. Without the connection, the information for Event identification, arrival time, Job record, MIT count, and Event-based deadline monitoring is not obtained, and it is not estimated from BPF execution intervals. When there is no Event record, the declared D is a parameter for admission, not a monitored Job deadline. P1 does not add any extra classification or configuration flag, and no component depends on the application classification. If a source has no Event record, there is no object for DeadlineMonitor to determine, and the application is not reclassified because of a temporary silence in posting.
- **A-06 (blocking)**: It is no longer a premise of consistency. Because the Job boundary is explicit, blocking during a Job is allowed and observed (it is included in R, and a wake-up after blocking is a kernel activation within the open budget window). However, the admission result (§3.3) is guaranteed only for Jobs that do not block, and the deadline of a source that blocks is monitored but not guaranteed.

### 1.3 P1 Limitations Summary (details in each section)

- Deadline-miss detection depends on the monitoring period and wake-up jitter and does not guarantee zero latency (§5.9, TIM-009). Reporting latency is separate (TIM-010).
- During a daemon outage, there is no real-time detection or reporting. It is confirmed late as `DETECTED_LATE` at the next startup cleanup. There is no heartbeat (§5.6 protocol 7).
- A source has one posting context. Multi-poster posting is P2 (F-13).
- The payload is inline with a maximum of 64 B (candidate). A large payload and release notification are P2.
- Every process that writes to the SHM object is trusted (§6, isolation is F-14).
- At a re-registration boundary, one post and one `missed++` may straddle the counts of the previous and new registration (§5.6 limitation).
- There is no API that cancels the worker's termination wait (LLD L-8, 9209bc5 retained).
- Completion time `t_end` is the time when the worker has finished the declared processing and the result posting. It does not mean completion of network delivery or device update (P1-008 §2). Budgets for IRQ/ISR, middleware-internal threads, and the whole network path are not set automatically by TIMPANI (REQ-008 §7, Issue #53).
- If the poster dies in the middle of posting, there are two cases. If it dies after making the entry visible and before advancing `tail`, the next post counts that entry as accepted through R-b recovery. If it dies after advancing `tail` and before (f), one `accepted` count is dropped and is not recovered. In both cases, the worker does not wake until the next post or a TimerMaster change, and that Event remains a target of deadline monitoring (§6.4, A8).
- During daemon startup cleanup (`kind = NONE`) and the activation gap (after the sched_ext switch and before slot activation), a post receives E_INACTIVE (§6.4). TT republication (`magic` toggle) does not reject posts. Post step (a) checks only the slot `kind` (§6.2), which closes L-15 (§12).

---

## 2. Protocol Changes (`timpani-o/proto`) — Proposal, Project Leader Approval Required

All changes are field additions. They are not implemented before approval. Without approval, reports carry only the count (`count`), and `flags`, `job_seq`, and `job_gen` are absent (reduced behavior under SRS/REQ).

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
    BUDGET_EXCEED = 2;   // informational: CBS throttle entry (once per episode)
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

The FaultFlags table uses the same values for `schedinfo.FaultInfo.flags` and `node_control.FaultInfo.flags`. This table is the single source of the flag values. It is a different table from the BPF execution-interval flags (§4.1).

| Bit | Name | Meaning | Set by |
|:--|:--|:--|:--|
| 0x1 | MIT_RELATED | For the Job that missed its deadline, an MIT-violating arrival occurred after `t_arr − T_s`, based on its `t_arr` (`last_viol_ns ≥ t_arr − T_s`) | FaultMonitor (calculated by reading `last_viol_ns`) |
| 0x2 | THROTTLED | The server was in the throttled state at the time of detection. It is set only on the detection path (DeadlineMonitor) | DeadlineMonitor (read of CbsState) |
| 0x4 | NOT_STARTED | At the time of detection, the Event was still QUEUED (replaces the old EARLY bit) | DeadlineMonitor |
| 0x8 | LATE_COMPLETION | The deadline miss was set at the worker's end transition (`t_end > t_dl`) (replaces the old MARKER bit) | Worker (Job log `miss_src = TTSCHED_MISS_END`) |
| 0x10 | DETECTED_LATE | The deadline miss was set in the close procedure (startup cleanup, deactivation and activation close rules, cancellation of a stale generation during a worker wait `close_stale`) | TIMPANI-N close procedure |

- The old `EARLY`(0x4) and `MARKER`(0x8) are retired. The same bits are reused with the meanings above, and there is no old implementation (design stage).
- The old `CARRYOVER`(0x40) is removed from `FaultInfo.flags`. This value remains only in the BPF execution-interval record (§4.1). In P1, it is not a DMISS report flag.
- `THROTTLED` has a value only on the detection path. For a deadline miss set by a worker or by a close procedure, it has no meaning, so it is 0.

Job identifiers (`job_seq`, `job_gen`) are not carried in schedinfo. These values are passed only on the node_control side (§2.2). Whether identifiers are passed when TIMPANI-O forwards to Pullpiri is an open question (§12, follow-up alignment).

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

- `job_seq`/`job_gen` are filled only for DMISS. BUDGET_EXCEED and MIT_VIOLATION are set to 0.
- `job_gen` is `uint64`. The upper 32 bits come from `instance_id` of the SHM object, and the lower 32 bits come from the binding counter (§5.6). That the values do not overlap after a daemon restart is probabilistic (§5.6, A14). With a restart or wall-clock change whose interval is a multiple of 2^32 ns, the upper bits may become the same.

**Publication rule** (no message change): `PartitionConfig.layers = [ {layer_index=1, model=TIME_TRIGGERED, tt_slots=[...], cbs_entries=[]}, {layer_index=2, model=EVENT_DRIVEN, tt_slots=[], cbs_entries=[...]} ]`. An empty layer may be omitted. `table_id = "table_" + generation`.

### 2.3 Backward Compatibility Rules
- All changes are field additions. Messages sent by old Pullpiri (fields 1~10) and old timpani-n (fields 1~8) are deserialized unchanged.
- O always fills `cpu`, and CPU 0 is an explicit value. timpani-n treats an entry for which `has_cpu()` is false, or which is not an isolated CPU, as an apply error (it is not replaced with the partition cpuset, because replacing it would invalidate the per-CPU check during admission). O and N are deployed together. proto3 `optional` requires protoc 3.15 or later.
- Add `min_inter_arrival_us` to `scripts/mock_pullpiri.py`, `scripts/mock_pullpiri.yaml`, and `timpani-o/scripts/grpcurl/cbs.json`. If the proposed fields (`count`, `flags`, `job_seq`, `job_gen`) are not approved, these fields are not added.

---

## 3. TIMPANI-O Details

### 3.1 `global_scheduler.h` — Data Structures

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
static constexpr uint32_t L_POST_MAX_US  = 50;     // NEW: TIM-011 part 1 (t_arr → runnable), candidate bound under the reference poster configuration (REQ-008 A-10), to be measured
static constexpr uint32_t L_DEL_MAX_US   = L_POST_MAX_US + L_WAKE_MAX_US;   // NEW: TIM-011 delivery bound, candidate 100 µs
static constexpr uint32_t DELTA_OVR_US   = 1000;   // NEW: TIM-004 / TMG-004 overrun bound δ_ovr = one tick at HZ=1000, candidate
static constexpr uint32_t DEFAULT_HP_US  = 10000;  // table-format value for a node without TT (FR-O-015). CBS execution does not use this value (D-22)
static constexpr size_t   MAX_L2_PER_CPU = 16;     // equals BPF PROMOTE_BUDGET (§4.4), so a due throttled server is never skipped
static constexpr size_t   MAX_TASK_ID_LEN = 15;    // Linux comm limit (TASK_COMM_LEN − 1)
```

`L_DEL_MAX_US` and `DELTA_OVR_US` are candidate values before measurement. Do not use these two values as "proven worst-case values" (§3.3, review comments on the overview).

Correspondence between internal reasons and the SRS-008 FR-O-004 reason codes (P1 logs them, P2 uses `Response.reason`):

| SRS reason code | `InfeasibleReason` |
|:--|:--|
| `PARAM_INVALID` | `ParamInvalid` (including T > MIT and D > MIT) |
| `UTIL_EXCEEDED` | `UtilizationExceeded`, `NoCbsBandwidth`, `SafetyCbsExceeded` |
| `DEADLINE_INFEASIBLE` | `DeadlineInfeasible`, `GapTooSmall` |
| `NO_CPU` | `NoCbsCpu`(including exceeding the L2-per-CPU upper bound), `TtSlotConflict` |
| `NODE_UNKNOWN` | `NodeUnknown` |
| `NON_HARMONIC` | `NonHarmonicPeriod` |

### 3.2 `schedinfo_service.cpp` — Conversion, Validation, and Restoration

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

### 3.3 `global_scheduler.cpp` — Pipeline and Admission

```text
generate_schedule(node, tasks):
  1. split TT / CBS (unchanged). CBS: T_s = period (or MIT when no period), MIT, D, C
  2. global_hp = TT present ? LCM(all TT periods on node) : DEFAULT_HP_US     // FR-O-015; the wording change is OI-25
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

A CPU without TT slots (including a node without TT tasks) also goes through step 4 and receives its own `PerCpuSchedule` (FR-O-022). Because `slots_k` is empty for this CPU, `btt_k` is 0, and `occupancy_within_us` returns 0 for the empty `merged_us`. The Gap is a single interval covering the whole period. `DEFAULT_HP_US` is used only as the table-format value and as the horizon of the gap computation, and it is not a CBS execution parameter (D-22). The admission for this CPU uses $B_{tt} = 0$ (CBS-007).

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

In the CBS worst-fit step of `assign_cpus()`, conditions (2)/(3) and the count upper bound (`MAX_L2_PER_CPU`) are checked in advance for each candidate CPU, and only the CPUs that pass are kept as candidates (FR-O-014). If every CPU fails, `NoCbsCpu` is returned. The $\sum_{j \ne i} C_j$ term of condition (3) comes from the activation-order queue of BPF and the replenishment window rule (§4.3, DDR-008 D-04 (P-c)). The δ_ovr term is the upper bound on how far another server runs over its budget (TIM-004 / TMG-004).

**Single admission formula (D-09, P1)**. Every L2 task `i` must satisfy it on CPU `k`. This formula is independent of whether the explicit Event connection is used.

$C_i + B_{tt}(k, D_i) + L_{del,max} + \sum_{j \ne i} (C_j + \delta_{ovr}) \le D_i$ and $D_i \le MIT_i$ and $T_i \le MIT_i$

- $MIT_i$ is `min_inter_arrival_us`, and if it is absent, the period is used (D-11 rule). Therefore, for a task that does not declare an MIT, $T_i \le MIT_i$ is automatically true. In that case $MIT_i$ is the period, so $D_i \le MIT_i$ is true only when $D_i$ is not greater than the period, and a $D_i$ greater than the period is rejected as ParamInvalid (§3.2).
- $L_{del,max}$ = arrival time → first dispatch of the worker on an idle, TT-free CPU = $L_{post}$ (t_arr → runnable, new timing item, candidate 50 µs) + $L_{wake}$ (runnable → first run, TIM-001 on an idle, TT-free CPU, 50 µs). The wake-up during TT occupancy (TIM-002) is not included in this sum. The TT occupancy is covered by the $B_{tt}$ term (A24). Candidate $L_{del,max}$ = 100 µs. It is a target to be measured and is not a proven worst-case value. TIM-001, TIM-002, and TIM-011 keep their kernel-side definitions unchanged. The $L_{post}$ upper bound holds under the reference poster configuration (a configuration in which the poster thread is not preempted by another thread from the time of recording the arrival time until the post call returns, REQ-008 A-10).
- $\delta_{ovr}$ = the upper bound on how far one server can run over its budget before it is throttled (TIM-004 / TMG-004). In P1, 1 ms, one tick at HZ=1000, is used as the candidate. The nohz_full CPUs and the tick restart delay are to be measured. This term answers the overview review comment that the previous formula ignored tick overrun.
- **Consequences**: If there are several L2 tasks on the same CPU, the parameters that can be admitted shrink. This is a risk, and it is mitigated by P2 (HRTICK).
- If $D_i \le MIT_i$, a source has at most one live Event at a conforming arrival. Therefore, no self-queueing term is needed. If there is an MIT violation, the queue (up to QDEPTH) builds up and the later Job misses its deadline. This is reported, not prevented.

This formula is a **sufficient condition** under the following assumptions. The document must state these assumptions.

1. TT tasks run within their declared execution time. A TT overrun is a TT DMISS and is outside the guarantee of L2.
2. $\delta_{ovr}$ and $L_{del,max}$ hold as measured values (candidate values are only assumptions until they are measured). The $L_{post}$ upper bound holds under the reference poster configuration (A-10).
3. Arrivals from a source respect the MIT.
4. Jobs do not block (§1.2).
5. The adapter posts at the time of arrival. If the supplied value of `t_arr` is earlier, the same amount of $D$ is consumed.
6. The charged execution time from activation to actual sleep (including the cost of entering the wait) is at most the declared budget $C_i$ (budget margin recommendation).
7. Each Job ends early enough for the worker to sleep before the next arrival (§4.5, P-a boundary case).
8. `tol` (`CBS_ACT_TOL_US`) covers the measured activation jitter.

- Regardless of whether the explicit Event connection is used, all L2 tasks pass the same admission formula. When there is no Event record, D is an admission parameter based on kernel activation and is not a monitored Job deadline. Therefore, the $L_{post}$ term is only an additional margin.
- **FCW check**: $2000 + 2000 + 100 + 0 = 4100 \le 5000$ µs (when no other L2 task is on the same CPU). If any second L2 task is on the same CPU, the default FCW parameters fail. This is because $2000 + 2000 + 100 + (C_j + 1000) > 5000$ always holds for $C_j \ge 0$. Coexistence requires different parameters (§8, AC-008-16).
- **No proto change**: The queue depth and payload size are build constants in P1 (§5.6). `deadline_us` and MIT are already in the table and are published to the slot.

### 3.4 `orchestrator_service.cpp` — Preservation of Fault Types

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

### 3.5 timpani-o Tests (GTest, `timpani-o/tests`)

| Test | Verification |
|:--|:--|
| `SchedInfoServiceTest.MitFieldPreferredOverPeriod` | FR-O-001 |
| `SchedInfoServiceTest.LegacyPeriodAsMitWarns` | FR-O-001 transition rule |
| `SchedInfoServiceTest.RejectWcetGtDeadline` / `RejectDeadlineGtMit` / `RejectZeroMit` / `RejectTaskIdLongerThan15` / `RejectDuplicateTaskIdOnNode` | FR-O-003 |
| `SchedInfoServiceTest.RejectPeriodGtMit` (T=12 ms, MIT=10 ms → ParamInvalid) / `PeriodAbsentUsesMitAsServerPeriod` | FR-O-003, T_i ≤ MIT_i |
| `SchedInfoServiceTest.RejectRestoresPreviousWorkload` | FR-O-005 |
| `GlobalSchedulerTest.TtBlockingMergesAdjacentSlots` / `TtBlockingWrapAround` / `TtBlockingWindowSpansTwoSlots` (D=15 ms, TT 2 ms @ 10 ms → B_tt=4 ms) | compute_tt_blocking, occupancy_within_us |
| `GlobalSchedulerTest.TtBlockingWindowLongerThanHyperperiod` (D=25 ms, hp=10 ms, TT 2 ms → 2·2 ms + B_tt(5 ms)) | `total_us` branch |
| `GlobalSchedulerTest.FifoAdmission_FcwConfigPasses` (C=2000, B_tt=2000, L_del_max=100, D=5000 → 4100 ≤ 5000) | FR-O-013, single formula |
| `GlobalSchedulerTest.FifoAdmission_SecondL2Rejected` (C=2000×2, B_tt=2000, D=5000 → 2000+2000+100+3000 = 7100 > 5000) | FR-O-013 |
| `GlobalSchedulerTest.FifoAdmission_DeltaOvrCounted` (C=1000×2, B_tt=0, D=3000 → 1000+0+100+2000 = 3100 > 3000 rejected; the boundary case that passes at 2100 without δ_ovr) | δ_ovr term (TIM-004) |
| `GlobalSchedulerTest.FifoAdmission_ExistingTaskRechecked` (rejected if an existing task's condition is broken by a new task) / `MaxL2PerCpuEnforced` | FR-O-013 |
| `GlobalSchedulerTest.MinGapRejects` / `TtOnlyCpuWithSmallGapNotRejected` (50 µs gap on a CPU without L2) | FR-O-014 |
| `GlobalSchedulerTest.WorstFitUsesServerPeriod` (selects the CPU by C/T_s. Does not select it by C/MIT) | FR-O-014 |
| `GlobalSchedulerTest.CbsEntryCpuZeroIsExplicit` (`has_cpu()` && `cpu()==0`) | IF-ON-003 |
| `GlobalSchedulerTest.EventDrivenLayerEmitted` / `TtLayerHasNoCbsEntries` | FR-O-016 |
| `GlobalSchedulerTest.CbsOnlyNodeHyperperiodNonZero` / `SlotsRepeatPerGlobalHyperperiod` | FR-O-015 |
| `GlobalSchedulerTest.TtFreeCpuAdmitsL2WithZeroBtt` (places and admits an L2 on a CPU with no TT slots and checks it with B_tt = 0; the Gap is the whole interval. Planned test) | FR-O-022, CBS-007 |
| `GlobalSchedulerTest.TtUtilizationBoundEnforced` | FR-O-011 |
| `OrchestratorServiceTest.FaultTypePreserved_{BudgetExceed,MitViolation}` | FR-O-020 |
| `OrchestratorServiceTest.FaultCountForwarded` / (after approval) `OrchestratorServiceTest.FaultFlagsForwarded` | FR-O-020, §2.1 |
| `PublicationCoordinatorTest.TableIdMonotonic` | IF-ON-006 |

---

## 4. BPF Scheduler Details (`timpani-n/src/bpf`)

> **Identifiers and meanings (common rules)**: The BPF state machine now observes **execution intervals**. An execution interval consists of kernel activation (a task that was asleep becomes runnable) → first run → voluntary sleep. This interval is not a Job. The Job boundary is specified by the worker (§5.6, Job log), and BPF does not count Jobs. The identifiers `job_state`, `JOB_WAITING`, `JOB_ACTIVE`, `JOB_FLAG_*`, `JobRecord`, `job_trace_rb`, `job_fsm.h`, and `job_fsm_on_*` are carried over from 9209bc5, and **only the names are kept**. Their meaning is the execution interval. The field `job_seq` is changed to `run_seq`, and `t_done_ns` is changed to `t_sleep_ns` (because these fields changed meaning).

### 4.1 `maps.h` — Structures (full replacement)

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
#define CBS_ACT_TOL_US  50u   /* tolerance of replenishment window rule 1 only (D-04). Candidate 50 µs = L_post (bound under the reference poster configuration, A-10), to be measured.
                                 It is no longer shared with an MIT check nor tied to L_wake_max */
#define PROMOTE_BUDGET  16    /* promotion scan bound = MAX_L2_PER_CPU (timpani-o admission) */
#define JOB_FLAG_THROTTLED     (1u << 1)   /* the server was throttled during this interval (enqueue) */
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

The `mit_us`, `mit_viol_count`, and `last_act_ns` fields are removed. They were used only for the activation-based MIT check. The `TaskMeta` layout change is built together with the timpani-n userspace that uses the same header, so no additional compatibility measure is needed.

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
    __u32 throttle_episodes;  /* incremented on entry to the throttled DSQ */
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
    __u64 actual_completion_ns;   /* TT DMISS: t_done; BUDGET_EXCEED: throttle entry time (enqueue) */
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

> The `TaskMeta`/`CbsState` layout change is built together with the `timpani-n` userspace (same header), so no additional compatibility measure is needed. The meaning of the TT-path fields is unchanged (NFR-003).
> Capacity (NFR-001): `task_meta_map` 4096, `cbs_map` 1024, SHM 64 slots (sum of TT and L2: if 32 L2 are used, TT is limited to 32) → L2 ≥ 32 per node is satisfied. L2 per CPU is 8 or more, with the upper bound `MAX_L2_PER_CPU` (16). Per-CPU DSQs have no length limit.
> The `remaining_us`/`replenish_at_ns`/`exec_*` fields of `cbs_map` are written only by BPF. Userspace writes them only when it creates an entry and when a parameter changes (§5.1 (C)).


### 4.2 DSQ and Map Additions (`timpani.bpf.c`)


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


### 4.3 Shared Helper — `job_fsm.h` (shared by BPF and the host, pure functions)

The BPF kernel code and GTest share the state machine rules through the **same header**. The header takes only the `TaskMeta`/`CbsState` pointers and `now`, with no map access and no kfunc. The header has no MIT check, deadline determination, or event emission. It contains only the state transitions and budget rules of the execution interval.

```c
/* timpani-n/src/bpf/job_fsm.h — no BPF helpers; compiled into both scx_timpani and timpani-n tests */

/* returns 1 if this wake-up starts an execution interval (WAITING → ACTIVE), 0 otherwise.
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
/* No other code refills remaining_us (rule 4): not enqueue, not stopping, and not userspace while the task is in SCHED_EXT.
 * Enforcement looks only at the window state; no MIT flag or Job count is consulted (D-05). */

static __always_inline void job_fsm_on_running(struct TaskMeta *m, __u64 now)
{
    if (m->job_state == JOB_ACTIVE && m->t_start_ns == 0) m->t_start_ns = now;   /* FR-B-005 */
}

/* Charge CPU time only; exhaustion does not start a throttle episode. */
static __always_inline __u32 cbs_charge(struct CbsState *c, __u64 now)
{
    if (c->exec_start_ns == 0) return 0;
    __u64 total = (now - c->exec_start_ns) + c->exec_frac_ns;
    __u32 us = (__u32)(total / 1000ULL);
    c->exec_frac_ns = (__u32)(total % 1000ULL);               /* FR-B-012: ns carry */
    c->exec_start_ns = 0;
    if (c->remaining_us == 0) return us;
    if (us >= c->remaining_us) c->remaining_us = 0;
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

- The `FsmEvent` struct and the `emit_fault` output argument are removed. The header does not create events.
- Because the header contents changed, the host test target also changes (§5.8, `JobFsmTest`).

### 4.4 ops Implementation

**Sole-runnable path.** On Linux 6.12/6.13, if there is no other candidate, a `prev` whose slice has expired can keep running without `SCX_OPS_ENQ_LAST`. Making the slice 0 in `stopping` alone does not block this path. The pseudocode below uses this option on both versions so that the task goes through enqueue after expiry. When the last CBS task enters the runnable queue, a reschedule is requested, and a throttled task runs again only through the existing replenishment path. Because this option applies to the whole SCX class, the handling of `SCX_ENQ_LAST` for TT, BE, and the fallback enqueue is also verified at implementation time (reschedule only runnable work and keep the TT gate). Other supported kernels confirm, in their own API, a path that gives the same guarantee. This is pseudocode not yet verified on the target.

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

/* SCX_ENQ_LAST is delivered for the previous task on this CPU. */
static __always_inline void enqueue_global_ready(struct task_struct *p, u64 flags)
{
    scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, flags);
    if (flags & SCX_ENQ_LAST)
        scx_bpf_kick_cpu(bpf_get_smp_processor_id(), 0);
}

SEC("struct_ops/enqueue")
void BPF_PROG(enqueue, struct task_struct *p, u64 enq_flags)
{
    struct TaskMeta *m = meta_of(p);
    if (!m) { enqueue_global_ready(p, enq_flags); return; }
    if (m->scheduling_type == SCHED_TYPE_TT) { /* unchanged: LOCAL_ON | acpu + PREEMPT kick */ return; }
    if (m->scheduling_type == SCHED_TYPE_CBS) {
        __u32 cpu = m->assigned_cpu;
        if (!cpu_is_isolated(cpu)) { enqueue_global_ready(p, enq_flags); return; }
        __u64 key = cbs_key_from_meta(m);
        struct CbsState *c = bpf_map_lookup_elem(&cbs_map, &key);
        if (!c) { enqueue_global_ready(p, enq_flags); return; }
        __u64 flags = cbs_enqueue_flags(enq_flags);
        /* No refill here (D-04 rule 4). The old cbs_lazy_replenish() call is removed. */
        if (c->remaining_us == 0) {                                          /* throttled: wait for the window end */
            m->job_flags |= JOB_FLAG_THROTTLED;
            if (!c->throttled) {
                c->throttled = 1;
                c->throttle_episodes++;
                emit_fault(m, FAULT_BUDGET_EXCEED, c->replenish_at_ns, bpf_ktime_get_ns());
            }
            scx_bpf_dsq_insert_vtime(p, dsq_throttled(cpu), SCX_SLICE_DFL, c->replenish_at_ns, flags);
        } else {                                                             /* ready: activation order (D-08, FR-B-016) */
            c->throttled = 0;
            scx_bpf_dsq_insert_vtime(p, dsq_cbs(cpu), SCX_SLICE_DFL, m->queue_key_ns, flags);
            if (enq_flags & SCX_ENQ_LAST)
                scx_bpf_kick_cpu(cpu, 0);  /* ready work needs another scheduling cycle */
        }
        return;
    }
    scx_bpf_dispatch(p, DSQ_BE, SCX_SLICE_DFL, enq_flags);
    /* BE is eligible only on non-isolated CPUs; do not bypass the dispatch gate. */
    if ((enq_flags & SCX_ENQ_LAST) && !cpu_is_isolated(bpf_get_smp_processor_id()))
        scx_bpf_kick_cpu(bpf_get_smp_processor_id(), 0);
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
    if (tt_active_on_cpu((__u32)cpu, now)) return;          /* TT-first (DDR-007). Always false on a CPU without TT slots (FR-B-025) */
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
       On a CPU without TT slots there is no next TT slot, so the only cap is remaining_us (FR-B-025) */
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
            m->budget_used_us += cbs_charge(c, now);
            if (runnable && c->remaining_us == 0)
                p->scx.slice = 0;  /* budget is 0 and the task is still runnable: expire the slice so it goes through enqueue (throttle), not back to the local DSQ head */
        }
        return;
    }
    /* TT path: unchanged (mark_tt_not_runnable + current_slot deadline check, DDR-012).
     * The early-DMISS judgement that used to run here for L2 tasks is removed (D-20). */
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
    args->remaining_us = rem; args->replenish_at_ns = rep; args->throttled = thr;   /* copied back to userspace */
    return 0;
}

SEC(".struct_ops.link")
struct sched_ext_ops timpani_ops = {
    .flags = SCX_OPS_SWITCH_PARTIAL | SCX_OPS_ENQ_LAST, /* Linux 6.12/6.13 */
    .init = (void*)init, .init_task = (void*)init_task, .exit_task = (void*)exit_task,
    .select_cpu = (void*)select_cpu, .enqueue = (void*)enqueue, .dispatch = (void*)dispatch,
    .runnable = (void*)runnable, .running = (void*)running, .stopping = (void*)stopping,
    .quiescent = (void*)quiescent, .name = "timpani" };
```

- `emit_job_record(m, now, extra_flags)` submits `JobRecord{workload_id_hash, task_id_hash, bind_gen = m->bind_gen, run_seq = m->run_seq, cpu, budget_used_us, t_act_ns = m->activation_ns, t_start_ns = m->t_start_ns, t_sleep_ns = now, flags = m->job_flags | extra_flags}` to `job_trace_rb`. A failure is reported only as a record loss (FR-B-023).
- `emit_fault()` fills `FaultEvent.run_seq`, `activation_ns`, and `flags` based on `m`, and on a `bpf_ringbuf_reserve` failure it increments `fault_drop_count++` (FR-B-023).
- The `runnable` argument of `stopping` is used only on the L2 path. When the remaining budget is 0 and the task is runnable, the slice is set to 0 so that it goes through enqueue together with `SCX_OPS_ENQ_LAST` (see "Sole-runnable path" above; not verified on the target). The TT path does not use this argument.

### 4.5 Verifier and Compatibility Notes
- `ops.runnable`/`ops.quiescent`/`ops.exit_task` exist as sched_ext 6.12 struct_ops members. `SCX_DEQ_SLEEP` is a vmlinux.h enum.
- `select_cpu` may not be called for a task with `nr_cpus_allowed == 1`, so moving the activation record to `runnable` is also needed for correctness.
- The iteration upper bound of `promote_throttled_if_replenished` is `PROMOTE_BUDGET=16` (SRS NFR-002). Because TIMPANI-O admits at most 16 L2 tasks per CPU (§3.3), a server whose window has ended is not skipped by the scan.
- The only places that write the budget state (`remaining_us`, `replenish_at_ns`) are `runnable` (activation), `stopping` (charge), and `dispatch` (promotion). All of them run under the run-queue lock of the assigned CPU that the task belongs to (the D-06 premise that an L2 task is only in the run queue of its assigned CPU). `timpani_replenish_kick` only reads.
- The kfunc names are unified to the new DSQ API used by the current build (`TIMPANI_SCX_USE_NEW_DSQ_API`): `scx_bpf_dsq_insert`, `scx_bpf_dsq_insert_vtime`, `scx_bpf_dsq_move`, `scx_bpf_dsq_move_set_vtime`, `scx_bpf_dsq_move_vtime`, `scx_bpf_dsq_move_to_local`. The names are wrapped in compatibility macros from `scx/compat.bpf.h`, and the signatures are checked against the `vmlinux.h` of the target kernel.
- Whether the `scx_bpf_kick_cpu` call in `timpani_replenish_kick` is possible, and the context return (copied back through `ctx_in`), depend on the kernel → **OI-9 PoC** (item: DDR-008 D-07). If not possible, use §5.3 `KickBackend::KickerThreads`.
- `job_fsm.h` uses only `__always_inline` and fixed-width types, and does not include `bpf_helpers.h`, so it can be compiled on the host.
- Limitation of the execution interval (REQ-008 §2, DDR-008 D-02): If `ttsched_evt_wait()` returns without sleeping because an unconsumed Event exists (R3), `quiescent` is not called, so two consecutive Jobs are observed as **one execution interval**. BPF cannot know the Job boundary. The worker signals the Job boundary with `wait`/`done`, and it is recorded in the SHM Job log (§5.4(a), §5.6). The execution-interval records and the Job records are matched only by time, based on `t_arr`, `t_begin`, and `t_end` (DDR-008 D-16). The matching is 1:1 when the Job does not block.
- Budget of consecutive Jobs: If a second Job starts within the same execution interval, it does not receive a new budget (D-04 rule 4). It uses the remaining budget together with the interval until the interval ends. As a result, the later Job may be throttled or miss its deadline. This is reported, not prevented.
- The budget charge accumulates the on-CPU time from `running` to `stopping` in ns and reflects it in µs. The remaining budget saturates at 0, and the cost of entering the wait is also charged. If the task sleeps right after it is exhausted exactly, only the charge is applied. If the task is still runnable while the remaining budget is 0, enqueue blocks further execution. When it wakes after sleeping with the remaining budget at 0 and the activation replenishment condition (`t_act + CBS_ACT_TOL_US ≥ replenish_at_ns`, DDR-008 D-04 rule 1) is not met, there is no replenishment, so enqueue throttles it and BUDGET_EXCEED is emitted once. If the condition is met, the task receives activation replenishment and is not throttled. `replenish_at_ns` keeps the end of the existing window, and the report and the count are made once per actual throttle entry.
- Behavior after a violation (window terms): An execution interval activated before its window has ended has `JOB_FLAG_CARRYOVER` and runs with the remaining budget. An arrival closer than the MIT, or a Job that exceeded its deadline, falls under this case. The process by which the server returns to the arrival phase follows DDR-008 D-04 "Behavior after violation". Its example is described with a window timeline (§5.8).
- Blocking Job: If a Job blocks, the execution interval is split by the sleep. A wake-up after blocking is a kernel activation, and if it is within an open window, no new budget is given (`cbs_on_activation`, but marked as CARRYOVER). A split interval of a blocking Job is not a Job boundary. The Job boundary is carried by the SHM Job log. The admission does not provide a guarantee for a blocking Job (§3.3 assumption 4).

---

## 5. TIMPANI-N Userspace Details

> Common rule: TimerMaster owns the SHM slot layout (§5.6). The constants of this section and §6 are defined in §5.6, and §6 refers to them. Every SHM atomic operation is `seq_cst`. The RT path (the TimerMaster thread, DeadlineMonitor, Replenisher, post, wait, done) has no gRPC, allocation, or blocking I/O. Binding and close are serialized by `bind_mu_` on the non-RT path (the table apply thread or the TaskRegistry thread).

### 5.1 `main.cpp` — Table Apply Branch (Summary of Changes)

```cpp
// (A) Layer model branch (unchanged)
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

`allowed_cpus(tid)` reads the allowed set (`CpuSet`) with `sched_getaffinity(tid)`. If the read fails, it returns an empty set, and then no binding is made. This check runs in the first step of `bind_l2_task` (C), so both the table apply path and the re-identification path (`rescan_loop` in §5.2) pass through it. Adding a name that is already on the unresolved list does not change the list. The allowed CPU check (FR-N-033, CBS-008) differs from the isolated CPU check in (B) above. The isolated CPU check is a configuration error, so it is recorded with `apply_error`. The allowed-set check is a worker-side placement problem, so it does not use `apply_error`. It writes a log entry and leaves the name on the unresolved list. In this case `bind_l2_task` returns `nullopt` before it performs any write, so there is no SCHED_EXT switch, no BPF state registration, and no Timer Master activation. The binding path does not change the thread's affinity or cgroup. The handling of an SCHED_EXT switch failure is unchanged.

The removal and STOP path (`RecoverySignal`, table disappearance) calls `unbind_l2_task(..., worker_exited=false)`, and then performs `replenisher.remove(key)`, `fault_monitor.clear_task(key)`, `deadline_monitor.on_deactivated(slot)`, `job_trace.clear_task(key)`, `event_sources.clear_task(key)`, and `registry.remove_expected(name)` (FR-N-005). On this path, the `RUNNING` record does not become `ABANDONED` and stays under deadline monitoring. When the worker later finishes, `done()` records it (§5.6 protocol 5). Even after `kind = NONE`, a Job whose take CAS succeeded before the closing sweep stays RUNNING, and the worker finishes it (§6.4, A12).

### 5.2 `task_registry.{h,cpp}` — Redesign

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

`rescan_loop()`: (1) For each name in `unresolved_`, it calls `resolve_now()`. On success, it runs `binder_(tid, binding)`, and if a value is returned, it moves the entry to `tracked_` (including `bind_gen`). (2) For each TID in `tracked_`, it checks whether `/proc/<tid>` exists. If not, it calls `on_exited(name, tid)`. That call runs the close rule (`deactivate_evt(name, worker_exited=true)`, that is, RUNNING → ABANDONED). After that, it returns (name, binding) to `unresolved_` (FR-N-004). (3) When both sets are empty, it stays dormant until the next table. When returning an entry, if the same name is already in `unresolved_`, it does not overwrite it (the latest parameters that the table inserted take precedence).

`notify_exit(task_id_hash, bind_gen)` looks up the same `task_id_hash` in `tracked_`. **If `bind_gen` differs from the binding being tracked, it treats the record as stale and ignores it** (`stale_exit_ignored_++`). If the binding is the same, it takes the same path as `on_exited`. This is the rule that keeps a delayed exit record from deactivating a new binding.

`set_binder(bind_l2_task)` is called once, before `start()`, in the start-up sequence. The binder and the `on_exited` callback are called **after releasing `mu_`** (under the lock, only a snapshot of the call targets is made). Even if a callback re-enters the registry through `track()` or `add_unresolved()`, it does not deadlock. The binder takes `bind_mu_` (§5.1 (C)), so it is serialized with the table apply thread. `T_bind` ≤ period + scan time (≈ several ms) ≤ 1 s.

### 5.3 `replenisher.{h,cpp}` — New (No Change from the P1 Design)

This component is not changed by the P1 changes. The replenishment window rule (D-04) and the kick protocol (D-07) are independent of the Job boundary. It works the same way on a CPU without TT slots, and it does not use TT events or the hyperperiod as an execution condition (FR-N-032, D-22). Whether to merge it with deadline monitoring (DeadlineMonitor, §5.9) and a thread is an open item (F-12) and is not a decision of this section. The body is kept unchanged from 9209bc5.


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

- **Does not write state**: The Replenisher and the kick program only read `cbs_map`. Budget refresh and promotion are done by the BPF `dispatch` (§4.4). The userspace replenishment of the earlier draft (`apply_replenish_userspace`) and the replenishment inside the kick program have been removed, because they cause lost updates that overwrite budget decrements in progress on other CPUs.
- **Next check time**: `replenish_at_ns` changes only to "update time + $T_s$". If the window is open at the check time, the server is checked again at the window end. If no window is open, it is checked at `now + T_s`, so it is not later than the end of a window that opened in the meantime (≥ `now + T_s`). A server whose window has ended but is not yet promoted (because a TT slot is active and no kick was sent, or because it was read before the kick was processed) is checked again every 1 ms until it is promoted. "TT slot active" means the determination of `tt_active_on_cpu`, that is, there is a runnable TT task on that CPU. When a TT task sleeps, the CPU picks the next task and calls dispatch, so promotion happens even without a kick.
- **Table change and stop notification**: Uses only a condition variable. The `clock_nanosleep` + `SIGUSR2` of the earlier draft has been removed, because without a handler the signal terminates the process and can be lost. `epoch_` and `running_` are read and written only under the lock. `epoch_` is bumped only when the whole table is replaced. `remove()` marks the server so that the loop does not push it back only when the server the loop is holding is that server, so removing another server does not drop the check schedule of the server in progress. `stop()` sets `running_ = false` under the lock, wakes `cv_`, and then joins.
- **Allocation**: `set_servers()` builds the heap storage outside the lock and swaps it in. `remove()` erases in place, so it does not allocate. The loop only does `push` after `pop`, so the capacity does not grow.

Additional `BpfLoader` API: `bool lookup_cbs_state(uint64_t, CbsState*)` (read-only), `bool delete_cbs_state(uint64_t)`, `bool delete_task_meta(uint32_t)`, `int run_replenish_kick(ReplenishKickArgs*)`, `int get_job_trace_rb_fd()`, `bool set_trace_enabled(bool)` (writes the bss map), `bool lookup_task_meta(uint32_t, TaskMeta*)`.


### 5.4 `job_trace.{h,cpp}`, `event_source_monitor.{h,cpp}` — Collectors

**(a) `JobTraceCollector` — Job Log and Execution-Interval Records**

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

- **Reading the Job log**: Only the entries written by the worker are read. The entry order is the production order of the SPSC. Each entry has `t_arr`, `t_dl`, `t_begin`, `t_end`, `gen`, and `seq`, so there is no need to match execution-interval records and times to associate an Event with a Job. The response time histogram is `t_end − t_arr` (not `t_done − t_act`).
- Entries with `miss_src = TTSCHED_MISS_END` (2, the deadline miss is set in the worker's end transition) are aggregated as `LATE_COMPLETION` (0x8). Entries with `miss_src = TTSCHED_MISS_MONITOR` (1) have the same `(task, gen, seq)` as the miss record of DeadlineMonitor, so they are de-duplicated in the detail path of FaultMonitor (§5.5).
- **Execution-interval records**: These are the BPF execution-interval records delivered as `JobRecord` (§4.1). They carry no Job identifier. Only `bind_gen` and `run_seq` distinguish the intervals within a binding. A `JOB_FLAG_TASK_EXIT` record is passed to `TaskRegistry::notify_exit` through `on_task_exit(task_hash, bind_gen)`.
- The response time histogram is computed as `t_end − t_arr`. The interval wake-up latency is `t_start − t_act` (excluded if `t_start == 0`). 1024 bins of 10 µs (= 10.24 ms) plus overflow.
- Output file: `<node_id>.jobtrace.csv` (disabled by default; toggled by the `-t` CLI option or `SIGUSR1`).

**(b) `EventSourceMonitor` — Reading Cumulative Counters (IF-V-004)**

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

- `poll()` reads the cumulative counters of each slot by atomic load. **Read order**: `started`, `cancelled`, `completed`, … and `accepted` is read last. The post side runs `accepted++` after `publish`, so the worker's `started++` can become visible before `accepted++`. Even when `accepted` is read last, if the poster stops between (e) and (f), the worker's `started++` can become visible first. So `pending = accepted − started − cancelled` can temporarily become negative. The calculation uses `max(0, accepted − started − cancelled)`, and the exact value is obtained only in the quiescent state (§5.6 counter relation, A10).
- Counters are cumulative values. Changes between polls are not lost (FR-N-026). Differences are computed with `uint64_t` arithmetic.
- The baseline values do not change during the registration lifetime. At activation, TimerMaster reads them and passes them in `EvtActivation` (§5.6). A method that reads them after activation is not used, because violations accepted in between would be hidden in the baseline.
- **There is no marker-active transition, no source transition, and no discard of Events based on the activation baseline.** The "K/M source transition" and `OnMarkerActive` of the earlier draft have been retired.
- Reapplying the table (`set_shm`) replaces only the key-to-slot table under `mu_`. The counters and baselines are in the slot, so reapplication does not affect them.

### 5.5 `fault_monitor.{h,cpp}` — Counting Path and Detail Path

FaultMonitor operates on two paths. The counting path is lossless and is the basis of reporting. The detail path is best effort and attaches identifiers to reports. Both paths are described here, and duplicates are removed by `(task, gen, seq)`.

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

**Counting path (lossless, basis of reporting)**

- In each cycle, it reads `snap.missed` and `snap.mit_viol`. The number of deadline misses of the current registration is `n = snap.missed − base_missed`. The MIT violation increment of the current registration is `snap.mit_viol − max(base_mit_viol, mit_accounted)`.
- **DMISS threshold rule (DDR-012, unchanged)**: If `n > current_limit` and `dmiss_reported == false`, it sends `FaultInfo{DMISS, count = n}` once. It is not sent again in the same registration. After approval, this report carries the `job_seq`, `job_gen`, and `flags` of the most recent detail record of the same registration (if one exists). Before approval, it carries only `count` (A18).
- **MIT_VIOLATION report**: The increment is put into `add_mit(key, delta, now)`. The report is one per $T_s$ window per source, and `count` is the number of violations in that window (§2.1). Only the increment of `snap.mit_viol` is used, so there is no MIT event from BPF.
- The report format does not depend on the source (the arrival-based basis is the only source).

**Detail path (best effort, provides identification)**

- There are three inputs. (i) The miss records of DeadlineMonitor (§5.9). (ii) Job log entries with `miss_src ≠ 0` that are created when the worker ends. (iii) Deadline misses of the TimerMaster close procedure (`closed = 1`; startup cleanup is `DETECTED_LATE`). A deadline miss in a stale-generation record closed by the worker while it waits (`close_stale`) is delivered only through the `missed` counter, and no detail record remains (the counting path is the basis).
- Each entry is de-duplicated by `(task, gen, seq)` and then put into the detail table. **If `gen` differs from the `gen` of the current registration, it is excluded from the detail table and from reports** (a deadline miss of another registration remains only in the records and statistics). For an entry of the current registration, `flags` is computed as follows. After approval, it is included in the report (before approval, it is only computed and not included, A18).
  - `NOT_STARTED` (0x4): `state_at_detect == QUEUED` (miss records only).
  - `THROTTLED` (0x2): the `throttled` value of the miss record (only on the detection path).
  - `MIT_RELATED` (0x1): `snap.last_viol_ns ≥ t_arr − T_s`.
  - `LATE_COMPLETION` (0x8): `miss_src == TTSCHED_MISS_END` in the Job log.
  - `DETECTED_LATE` (0x10): set in the close procedure (`closed = 1`).
  - The `MIT_RELATED` expression applies in the same way to all deadline misses set by DeadlineMonitor, worker exit, and the cleanup sweep (FaultMonitor computes it).
- Detail records are attached to the DMISS report of the counting path above. A detail record that cannot be attached remains only in the record (trace) output. Entries discarded because the detail path is full are counted by `detail_drop`, and they do not affect the counts.

**FaultInfo reports and time basis**

- Deadline miss reports follow the threshold rule of DDR-012. Individual deadline misses that do not exceed the threshold are not reported. That criterion is an open question (§12). TMG-015 (reporting latency) is defined only for **reports that are sent**.
- Deadline misses created in the close procedure remain in the records and statistics. Reports follow the threshold rule of the current registration.

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

`NodeClient::send_fault(...)` fills `type = to_proto_fault(...)`, `task_id` (hash → name), `node_id`, `timestamp_ns` (REALTIME), and `count`, and after approval it also fills `flags`, `job_seq`, and `job_gen` (FR-N-015, A18). `job_seq` and `job_gen` are filled only in DMISS reports, and they are 0 if there is no detail.

### 5.6 `timer_master.{h,cpp}` — SHM v2 Layout, Activation, Deactivation, Startup Cleanup

This section is the single source of the SHM v2 layout. §6 and §5.8 refer to the constants and structures defined here. The current `sample-apps/src/libttsched.h` is v1 (`TIMPANI_MAX_TASKS 32`, with one `counter` per slot that expresses only TT wake-up). The v2 structure in this section is a design and is not implemented.

**Constants (proposal, build constants)**

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

**Event record (ring entry, 128 B)**

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

**Job log entry (48 B, produced by the worker only)**

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

**Slot (v2)**

```c
struct ttsched_cnt {                   /* cumulative counters over the life of the object; atomic fetch_add; monotonic */
    _Atomic uint64_t arrivals, accepted, overflow, revoked;      /* post side */
    _Atomic uint64_t started, completed, discarded, cancelled, abandoned, missed;   /* worker and closing side */
    _Atomic uint64_t mit_viol;                                   /* arrival-based MIT violations */
};                                     /* 88 bytes */

struct timpani_task_slot {             /* 2752 bytes */
    char     name[16];                 /*   0  task_id (comm, ≤ 15 bytes + NUL). Pinned for the object's lifetime (L2) */
    _Atomic uint32_t counter;          /*  16  futex word. TT: tick count. EVT: wake sequence (an accepted post, activation, deactivation and closing bump it) */
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

- Object size: 176192 B (about 172 KiB), within the 256 KiB limit. When the slot count or the queue depth changes, this size and the static assert are changed together.
- `_Atomic` is a C header type. The C++ side (TimerMaster, DeadlineMonitor, JobTraceCollector) accesses the same memory locations with `seq_cst` access through the `__atomic` built-ins or `std::atomic_ref` (C++20). The layout is fixed by the static asserts above.
- **Report identifier**: The upper 32 bits of `gen` are the lower 32 bits of `instance_id`, and the lower 32 bits, `bind_gen`, increase by 1 for each binding (`next_bind_gen`). `(gen, seq)` is unique within the same SHM object until `seq` wraps around (within 2^32 acceptances). Uniqueness across daemon restarts relies on the lower 32 bits of `instance_id`, so it is probabilistic. For offline analysis, the 64-bit `instance_id` recorded in the trace distinguishes daemon runs (A14). `seq` is the value of `tail` at the time of the post.
- **Counter relation (holds in the quiescent state)**: `accepted = started + cancelled_before_start + queued`, where `cancelled_before_start` is the number closed as `CANCELLED`, and `queued` is the number of records in the QUEUED state. It can be temporarily out of balance because of operations in progress (§5.4(b) read order).
- **Writers**: The poster writes the record fields, posts `word`, performs the rollback (CANCELLED if MISSED is set, otherwise REVOKED), writes `tail`, the arrival counters (`arrivals`, `accepted`, `overflow`, `revoked`, `mit_viol`), `last_arr_ns`, and `last_viol_ns`, and does `counter++` and the wake-up. The worker performs QUEUED → RUNNING, `t_begin`, the end transition and `t_end`, `started/completed/discarded`, `resp_max_ns`, the Job log entry, and `log_w`. DeadlineMonitor sets the MISSED bit of the record `word` (CAS) and `missed++`. TimerMaster writes `kind`, `name`, `mit_us`, `deadline_us`, `gen`, `last_arr_ns = 0` (activation), the close transitions (CANCELLED, ABANDONED, and MISSED on a deadline miss) and their counters, and `counter++` and the wake-up. `head` is advanced by CAS by whoever meets a terminal record (the worker, TimerMaster, or a poster that meets a full ring).

> `log_r` is written by JobTraceCollector (the reading thread).

**Ring rules (LLD interpretation; cross-check against the post-side code is in §6.2)**

- R-a `head` advances from `s` to `s+1` only by CAS, and only when `entry[s].word = (s, terminal)`. An entry is reused only after `head` has passed it. Therefore a QUEUED or RUNNING record is never overwritten. The worker copies the payload while it is RUNNING.
- R-b Only the poster writes `tail` and the fields of the entry at `tail`. The order is: write the fields → store `word = (tail, QUEUED)` (post) → store `tail + 1`. An entry counts as posted only when `word.seq` equals the expected seq and the state is not FREE. If the poster dies between the last two steps, the next post that sees the entry at `tail` already posted advances `tail` first (§6.2 recovery).
- R-c A reader that is not the owner (the monitor, the TimerMaster close, a helper) reads `word`, reads the fields, and reads `word` again. If the two values differ, it retries or skips the entry. The only way to change a state is a CAS on the value that was read. The monitor and the close procedure do not limit their range by `head` and `tail`. They scan all QDEPTH entries and select them by state (QUEUED/RUNNING).

**Protocol 1 — Activation (TimerMaster, at binding time)**

Activation is performed after the worker thread is bound and switched to SCHED_EXT (§5.1 (C), step 5). So every accepted Event has a managed worker. The target is every L2 task.

```text
activate_evt(name, cfg, bind_gen):          // table apply / rescan thread, under bind_mu_
  slot = pinned slot of name                // §5.1 (B0)
  1. slot.kind = NONE                       // seq_cst. No new post can pass step (a) of §6.2 from now on
                                             //   (normally already NONE; defensive when a re-bind follows a non-closed slot)
  2. closing sweep, mode ABANDON_RUNNING    // §5.6 close rule (all QDEPTH entries)
  3. slot.mit_us = cfg.mit_us; slot.deadline_us = cfg.deadline_us; slot.last_arr_ns = 0
  4. base_missed = slot.cnt.missed; base_mit_viol = slot.cnt.mit_viol      // after the sweep, before publication
  5. slot.gen = ((u64)(uint32)instance_id << 32) | bind_gen                // seq_cst
  6. slot.kind = EVT                                                        // seq_cst, publication of the slot
  7. slot.counter++; FUTEX_WAKE(&slot.counter, INT_MAX)                     // wake any waiter left from an earlier binding
  return { slot_idx, gen, base_missed, base_mit_viol }
```

- The baseline values of step 4 are read after the close rule (step 2) has finished and before publication (step 6). So a deadline miss created by the close goes into the counters of the old registration and not into the counters of the new registration. An Event that comes in between the baseline read and publication is counted in the new registration.
- `next_bind_gen(name)` only reads `lower32(slot.gen) + 1` and returns it. It does not publish the value. So `TaskMeta.bind_gen` can be written before activation (§5.1 (C), step 2).
- The `ring indices` (`head`, `tail`) are not initialized. The values are free-running, and `head` advances as it passes terminal records. Activation does not touch the indices that the poster writes (P1 design brief v3 §10, review record).

**Protocols 2–4 (post, wait, done)** — The full procedure is defined in §6.2. This section describes only what TimerMaster needs to know.

- Post step (a) looks only at `kind` (if `kind != EVT`, E_INACTIVE, with no change). It does not look at `magic`. Even when TT republication toggles `magic`, the post is not rejected. Startup cleanup (protocol 7) rejects posts by setting `kind = NONE` (A2).
- Wait (protocol 3) returns E_INACTIVE if `kind` is not EVT. Deactivation increments `counter` and wakes waiters, so a sleeping waiter does not miss it. The window between the check and the take CAS is described in the §6.4 P1 limitations (A12).
- Done (protocol 4) returns E_INACTIVE if the seq of `W0` differs from the seq of the open Job, or if `W0` is ABANDONED (A6). A worker that finishes late, after the close rule has closed a RUNNING record, or that tries to finish after the entry has been reused, returns through this path.

**Protocol 5 — Deactivation and Close Rule (TimerMaster)**

Deactivation has two callers. One is when the entry is removed from the table or is STOPped (the removal path in §5.1). The other is when the worker exit is detected (`on_exited` in §5.2).

```text
deactivate_evt(name, worker_exited):
  slot = pinned slot of name
  1. slot.kind = NONE                                        // seq_cst, first
  2. closing sweep, mode = worker_exited ? ABANDON_RUNNING : KEEP_RUNNING
  3. help head: advance head past terminal entries (R-a)
  4. slot.counter++; FUTEX_WAKE(&slot.counter, INT_MAX)      // waiters see kind = NONE and return E_INACTIVE
```

**Close procedure (sweep)** — It is performed on all QDEPTH entries.

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

- The close rule (startup cleanup and activation step 2) closes `RUNNING` unconditionally as `ABANDONED` (`mode = ABANDON_RUNNING`). Deactivation (protocol 5) uses `KEEP_RUNNING` when the worker is alive, so that the worker can finish later, and deadline monitoring keeps watching it.
- **Not written to the Job log.** TimerMaster writes the record of a closed Job to its own trace output (`ClosedRecord`, the `closed,` line of JobTraceCollector) (§5.4(a)). The Job log is an SPSC for the worker only.
- The closing sweep sets MISSED only in this CAS. `missed++` happens only when the successful CAS adds MISSED. The flag of a deadline miss created in the close procedure is DETECTED_LATE (0x10) (§2.1).
- After cleanup, it increments `counter` and wakes the waiters. A waiting consumer checks `kind` and returns E_INACTIVE.

**Protocol 6 — Deadline Result (Decision)**

MISSED is set by exactly one successful CAS. One of the following four parties sets it. (1) DeadlineMonitor (the record is QUEUED or RUNNING and `now > t_dl`), (2) the worker's end transition (`t_end > t_dl`), (3) the closing sweep (`now > t_dl`), (4) the stale-generation cancellation `close_stale` that the worker performs while waiting (`now > t_dl`, §6.4). The party that sets it performs `missed++`. Therefore, whichever side detects it, it is counted once per Job. A post rollback keeps an existing MISSED and does not set it (A11). The deadline misses of (3) and (4) are misses of the close procedure, so they set DETECTED_LATE (0x10). A completion with `t_end ≤ t_dl` is not a deadline miss even if it is collected later. No later transition clears MISSED.

**Protocol 7 — Daemon Restart (Startup Cleanup)**

The object lifecycle is carried over from 9209bc5 and follows the LLD §5.6 procedure. At startup, if the object `/timpani_ttsched` already exists (abnormal termination of the previous daemon), it is processed in the following order.

1. Open the object.
2. Invalidate it with `magic = 0`. The new `ttsched_init()` does not map an invalid object. What rejects posts is `kind = NONE` in step 3 (post step (a) looks only at `kind`, A2).
3. Write `kind = NONE` to all slots.
4. Apply the close rule (`ABANDON_RUNNING`) to all slots. Records that have passed their deadline get MISSED set and are recorded as `DETECTED_LATE`.
5. Wake the waiters with `counter++` and `FUTEX_WAKE(INT_MAX)` on all slots.
6. Log the final counters.
7. Call `shm_unlink`.
8. Create the new object with `O_CREAT|O_EXCL`, set the permissions with `fchmod(fd, 0666)` (the mode of `shm_open` is reduced by the umask), and write `version = 2` and a new `instance_id` (a nonzero value).

- It is important to invalidate (steps 2–3) before the close rule. Then the posts accepted before the invalidation are cleaned up by the close rule, and later posts receive E_INACTIVE because `kind` is NONE.
- An app that was attached to the old object receives E_INACTIVE and reattaches to the new object with `ttsched_init()`. `seq` starts again from 0 in the new object. Because `gen` differs, `(gen, seq)` is unique.
- The current constructor's behavior of "opening the existing object as it is if present" is removed.

**P1 limitation (those belonging to this section)**

- Post at the re-registration boundary: one post in progress during re-registration can go into the `arrivals`/`mit_viol` counters of the new registration (at most one per registration).
- `missed++` of the last record: If the last Job of the previous binding set MISSED in the worker's end transition, and the new registration reads its baseline before `missed++` has run, then that `missed++` goes into the new registration (at most one per binding). The `missed++` of the monitor (DeadlineMonitor) also runs after the CAS, so it can go in after the baseline (at most one per binding, A9).
- While the daemon is down: the old object still has `kind = EVT`. Posts are accepted, and the worker keeps processing. Only the worker's end transition determines a late completion. An Event that expires in the queue or during execution is not detected by anyone. Such a deadline miss is confirmed late as `DETECTED_LATE` at the next startup cleanup, and it appears only in the final statistics. There is no real-time report. P1 has no daemon heartbeat.
- Poster death (in the middle of a post): If the poster dies after the entry is made visible and before `tail` advances, the next post recovers the entry under R-b, raises `accepted`, and (f) of that post wakes the worker. If it dies after `tail` advances and before (f), one `accepted` counter is lost and is not recovered. In both cases, the worker does not wake until the next post or a TimerMaster change, and the Event is monitored (§6.4, A8).
- Trust: a process that can write to the SHM object can corrupt all slots (§6, F-14).

**TT slots**

- TT slots use the same-size slots of the same object. The existing handshake is unchanged. `magic = 0` → TT slot `counter++` + wake-up → write the name → `n_tasks` (release) → `generation++` (release) → `magic = TIMPANI_TTSCHED_MAGIC` (release). A retained TT task keeps its previous index, and a new TT task uses the lowest free index that has never been assigned to an EVT.
- "Incrementing `counter` on all slots" applies **only to TT slots**. The `counter` of an EVT slot is incremented by TIMPANI-N on activation, deactivation, and close, and by the poster in step (f) of an accepted post.

**Assignment and retention of EVT slots (`update_evt_slots`)**

- **Fixed assignment**: A slot assigned once to an L2 `task_id` belongs to that name for the lifetime of the daemon (`evt_slot_of_name_`). It is not reused for another name, and it is not cleared to 0. A new name receives a slot that has never been written (the zero-initialized state). If no slots remain, that entry is an apply error.
- **Writes only the name**: It records `name`. It does not write `kind`, `mit_us`, `deadline_us`, `gen`, the counters, or the baselines. So during table republication, posting and waiting are not interrupted, and unconsumed Events are preserved.
- **Retained binding**: If the parameters are the same, nothing is written according to D-17(a) in §5.1 (C). If the parameters change, the binding is released (protocol 5) and bound again (protocol 1), and a new `gen` is issued at that time.
- `EvtSlotTable` maps `key → {slot index}`. The baselines are not in this table, and they are passed on in the activation result (`EvtActivation`).

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

- The existing `set_schedule_table(slots, hp, epoch)` handles only TT slots and TT threads. Its EVT argument and return value are removed (removed from the signature without a name change; proposal, approval required).
- `cpu_thread_loop`: jitter samples are passed to `JobTraceCollector::add_tt_jitter_sample()` (a lock-free SPSC ring, RT safe). The stdout statistics are kept.

### 5.7 CLI/Configuration Additions
`-t` (job trace on), `-r <ms>` (rescan period, default 500), `-k syscall|thread` (kick backend, default syscall), `-T` (trace sink path). All of them are optional. The deadline monitoring scan period (`DL_SCAN_PERIOD_US`) is a build constant, not a CLI option (§5.9).

### 5.8 timpani-n Tests (new GTest target: `timpani-n/tests/`)

Every test follows the AAA structure. SHM protocol race tests write the same object from two or more host threads. For each pair, there is one test (§5.6 seq_cst pairs).

| Test | Target |
|:--|:--|
| `JobFsmTest.RunnableStartsIntervalOnlyFromWaiting` / `PreemptionInsideIntervalDoesNotRestart` / `SleepEndsInterval` / `BlockingResumeStartsNewInterval` | `job_fsm.h` (FR-B-001~003, execution interval) |
| `JobFsmTest.NoMitCheckOnShortSpacing` (the state and flags do not change even when the spacing is shorter than MIT) / `NoDeadlineVerdictOnSleep` (no DMISS record even on a late sleep) | FR-B-024 (D-05, D-20) |
| `CbsChargeTest.NanosecondCarry` / `ThrottleTransitionOnce` | FR-B-012/013 |
| `CbsWindowTest.ConformingActivationAlwaysRefills` (DDR-008 D-04 counterexample: T=18 ms, C=2 ms, activations 16.0/34.0 ms → no throttling) / `EarlyActivationKeepsRemainingAndMarksCarryover` / `ToleranceAbsorbsWakeJitter` (interval 17.95 ms: refill) / `AfterViolationWindowShiftsThenRecoversWithSlack` (D-04 "behavior after violation" timeline: 0/15/33 ms, returns when there is slack) / `NoRefillWhileActiveAndNotThrottled` / `PromotionRefillsAndOpensWindowFromNow` / `ConsumptionPerWindowBounded` | `cbs_on_activation`, `cbs_try_promote` (FR-B-014/017) |
| `CbsThrottleTest.ExactBudgetThenSleepDoesNotThrottle` (remaining 0, no entry or emission) / `ExactBudgetStillRunnableThrottles` (additional execution blocked, entry and emission once) / `WakeWithoutEligibleRefillThrottles` / `WakeWithEligibleRefillDoesNotThrottle` | Budget charge and enqueue state transition (FR-B-013/014, AC-008-03). The actual kernel callbacks and rescheduling path are to be confirmed by the target tests in §10. |
| `QueueOrderTest.RequeueKeepsActivationRank` / `PromotedJobGoesToTail` | `queue_key_ns` (FR-B-016) |
| `TaskRegistryTest.ResolvesLateThread` (≤1 s after fork+prctl) / `DetectsExit` / `BinderMayReenterRegistry` | FR-N-003/004 |
| `TaskRegistryTest.StaleExitRecordIgnored` (an exit record with a different bind_gen does not deactivate the new binding being tracked) / `ExitRecordOfCurrentBindingDeactivates` | FR-N-004, FR-N-029 |
| `TaskRegistryTest.BindReturnsGenAndTracksIt` / `FailedSchedExtLeavesSlotNone` (`sched_ext` switch failure → no activation, kind = NONE kept) | FR-N-005, FR-N-029 |
| `ReplenisherTest.HeapOrder` / `SwapOnTableChangeWakesLoop` / `RearmsFromReturnedReplenishAt` / `RetriesWhileDueButNotPromoted` / `RemoveDuringKickDoesNotPushBack` / `RemoveOfOtherServerKeepsInflightServer` / `StopWakesEmptyLoop` / `NeverWritesCbsMap` / `EmptyHeapSleeps` (kick backend mock) | FR-N-010 (unchanged, §5.3) |
| `ApplyTest.UnchangedEntryWritesNothing` (after reapplication, `run_seq`, `remaining_us`, slot `gen`, and `kind` are unchanged) / `ChangedEntryUnbindsDeactivatesThenActivatesNewGen` (`run_seq` restarts from 0, slot counters are kept) / `ServerExistsBeforeSchedExt` / `MissingCpuIsApplyError` | FR-N-005, IF-ON-003, D-17 |
| `ShmV2LayoutTest.StaticAssertsHold` (sizes and offsets: 128 B record, 48 B Job log entry, 2752 B slot, 176192 B object) | §5.6, IF-L-004 |
| `ShmV2Test.PinnedSlotKeepsNameAcrossReAdd` / `UpdateEvtSlotsDoesNotTouchKindOrCounters` / `TtRepublishDoesNotBumpEvtCounter` / `StaleObjectKilledAndRecreatedAtStartup` (new `instance_id`, mode 0666) | IF-L-001/002, FR-N-006 |
| `ActivationTest.ClosesLeftoverRecordsBeforePublishing` (old QUEUED → CANCELLED, old RUNNING → ABANDONED, and those past their deadline are counted after MISSED is added) / `BaselineReadAfterSweep` (a deadline miss from the close of the old registration does not go into the `base_missed` of the new registration) / `GenIsInstanceHighBindLow` (upper 32 bits = lower bits of instance_id, lower 32 bits = bind_gen) / `NextBindGenDoesNotPublish` | §5.6 protocol 1 |
| `ActivationTest.KindNoneBeforeSweepDefensive` (reactivation in EVT state → step 1 blocks posts) | §5.6 protocol 1 step 1 |
| `DeactivationTest.KindFirstThenSweep` (post attempt during close → E_INACTIVE or post after closing) / `QueuedClosedCancelledNotAnEvent` / `RunningKeptWhenWorkerAlive` (RUNNING remains under KEEP_RUNNING and stays a monitoring target) / `RunningAbandonedWhenWorkerExited` / `WakesWaiterWithKindNone` | §5.6 protocol 5 |
| `StartupCleanupTest.OrderMagicFirstThenKindThenSweep` (check of the recorded order) / `OverdueRecordsGetMissedDetectedLate` (DETECTED_LATE 0x10) / `CountersLoggedBeforeUnlink` | §5.6 protocol 7 |
| `ProtocolRaceTest.PublishVsKindNone` (race between the `kind`/`gen` recheck of post step (f) and `kind = NONE` of deactivation → post accepted or rolled back, no double counting) | §5.6 seq_cst pair (poster: publish → read kind/gen) |
| `ProtocolRaceTest.ActivationGenVsPost` (if activation changes gen after the post has read gen, it takes the rollback path) | §5.6 seq_cst pair (TimerMaster: write kind → read words) |
| `ProtocolRaceTest.CloseVsWorkerEnd` (race between the close CAS and the worker end CAS → exactly one wins, no duplicate deadline miss) | §5.6 protocols 4, 6 |
| `ProtocolRaceTest.MonitorVsWorkerEndSameBoundary` (a completion near `t_end = t_dl` and the monitor CAS → no MISSED if the completion has `t_end ≤ t_dl`, otherwise exactly once) | §5.6 protocols 4, 6, §5.9 |
| `EvtDeadlineRaceTest.ReadsWordFieldsWordAndSkipsChanged` (R-c recheck) | §5.6 R-c |
| `FaultMonitorTest.BudgetExceedOncePerEpisode` / `DmissThresholdUnchanged` (once when count > limit) / `DmissCountUsesRegistrationBaseline` (the counters of the old registration are excluded) | FR-N-012~014 |
| `FaultMonitorTest.MitWindowAggregation` (one per T_s window, count = number of violations) / `MitDeltaNotLostBetweenPolls` / `MitCountEqualsArrivalCounterOnly` (no BPF MIT event) | FR-N-012/014, D-05 |
| `FaultMonitorTest.DetailDedupByTaskGenSeq` (a miss record and a Job log entry with the same (gen, seq) → one detail) / `DetailOfOtherGenerationNotReported` / `DetailDropCountedNotCounted` (detail drops do not affect the counters) | FR-N-031 |
| `FaultMonitorTest.FlagsNotStartedThrottledMitRelatedLateCompletionDetectedLate` (setting condition of each bit) / `MitRelatedUsesArrivalTime` (`last_viol_ns ≥ t_arr − T_s`, kept even after the window has passed) / `DmissNotMitRelatedWhenViolationIsOlderThanTs` | FR-N-031, PRT-007 |
| `JobTraceTest.HistogramPercentiles` / `ResponseIsEndMinusArrival` (`t_end − t_arr`) / `IntervalWakeExcludesZeroStart` / `TracksMaxBudgetUsed` / `TaskExitRecordNotifiesRegistryWithBindGen` / `ClosedRecordHasNoBeginWhenNeverStarted` | FR-N-020~022 |
| `EventSourceMonitorTest.SnapshotPendingClampedWhilePosterStalled` (while the poster is stalled between (e) and (f), `pending` is clamped to 0, A10) / `CumulativeCountersNotLostBetweenPolls` / `BaselineFromActivationNotFromPoll` / `BaselineKeptAcrossReapply` | FR-N-026 |
| `TimerMasterTest.SlotPinnedToNameAcrossReAdd` / `CloseRecordWrittenByTimerMasterNotByWorker` (a closed Job is not written to the Job log) | IF-L-004, §5.6 |

### 5.9 `deadline_monitor.{h,cpp}` — New (DeadlineMonitor)

> This component is newly added in P1. It monitors the absolute deadline `t_dl` of every Event regardless of queue waiting, execution, preemption, throttling, and idle CPU state. It does not wait for a completion callback. The result is once per Job. It works the same way on a CPU without TT slots, and it does not use TT events or hyperperiod boundaries as execution conditions (FR-N-032, D-22).

**Thread and Time**

- A dedicated SCHED_FIFO thread. Priority 97 (candidate; lower than TimerMaster 99 and Replenisher 98, and higher than general app RT threads, A1). It runs on a non-isolated CPU. It does not assume that the Replenisher already provides this function. Whether the two threads are merged is F-12.
- Clock: `CLOCK_MONOTONIC`. The periodic wait is `clock_nanosleep(TIMER_ABSTIME)`, and it does not use signals. The use of `CLOCK_REALTIME` on the TT path is not changed.
- No heap allocation, no blocking I/O, and no gRPC. The miss record queue is a static array.

**Period and cost (candidate)**

- `DL_SCAN_PERIOD_US = 1000` (candidate). One scan reads `TIMPANI_MAX_TASKS × TTSCHED_EVT_QDEPTH = 512` records. Each record takes a few atomic loads. Each deadline miss adds one CAS, `missed++`, at most one BPF map lookup, and one queue push.
- Cost upper bound: the worst case of one scan is reading 512 records and handling 512 deadline misses (in practice, far fewer because the number of Jobs is limited). The measurement targets for scan time and wake-up jitter are a PoC item (adjacent to OI-9).

The loop below keeps the periodic scan as the basis. The single-timer alternative for the earliest absolute deadline is a design review and target PoC item of DDR-008 D-20/F-10 and REQ-008 OI-22, and it is not introduced in this sketch. The completion-time race OI-20 is also open.

**Monitoring loop (C++ sketch)**

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

- In the sketch above, `read_throttled_if_any` is called only for a detected record. It reads the BPF `cbs_map` once with `cbs_key_of_[idx]` (a non-blocking map lookup). If there is no key, `throttled = 0`.
- `now > t_dl` is a strict inequality (REQ-008 §2.5, PRT-009). This is why a completion on time, `t_end ≤ t_dl`, does not obtain MISSED.

**Per-slot BPF key table**

- `cbs_key_of_[idx]` (atomic) is written with `on_activated(slot, cbs_key)` at activation (§5.1 (C), step 5), and it is reset to 0 at deactivation. The monitoring thread only reads it.

**Miss records and queue (SPSC)**

- `MissRecord { slot, gen, seq, t_arr_ns, t_dl_ns, t_detect_ns, state_at_detect, throttled }`. It has a fixed size.
- The queue is a static SPSC array (candidate 256). The producer is the monitoring thread, and the consumer is `poll_tick` of FaultMonitor.
- **When the queue is full**: the MISSED mark and `missed++` are already completed by the CAS and atomic operations, so they are kept. Only the record is discarded, and `miss_drop_` is counted. The counting path depends on the counters, so the basis of reporting is not affected. Only the detail path is reduced.

**Result rule (once per Job, race)**

- The monitor CAS races with the worker's end transition (§5.6 protocol 4) on the same word. If the worker completed first (recorded the completion) and `t_end ≤ t_dl`, the monitor with `now > t_dl` does not see the completed record (it is skipped because its state is terminal). If the monitor set MISSED first, the worker's transition CAS fails, and the worker reads the clock again (§6.2 done (i)–(iv)). The recorded `t_end` and MISSED do not disagree.
- A completion on time that is collected late is not a deadline miss (the monitor does not look at terminal records. The determination uses the `t_end` at the time it was recorded).

**Restart and Deactivation Linkage**

- When the daemon stops, monitoring also stops. A deadline miss found late at startup cleanup (§5.6 protocol 7) is counted as `DETECTED_LATE`. There is no real-time detection (§1.3).
- A RUNNING record of a deactivated source (the worker is alive) stays monitored until it becomes terminal. When the close rule makes it ABANDONED, it is no longer a monitoring target.

**Measurement items (PoC, adjacent to OI-9)**: Measure the cost and jitter of the 1 kHz scan on the target CPU, under TT load and on the selected CPU. Determine whether event-based wake-up (instead of the periodic scan) is needed for the agreed upper bound (F-10).

**DeadlineMonitor interface (proposal)**

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

**Time basis and latency items**

- Detection latency (TIM-009) = upper bound of `t_detect − t_dl` = scan period + wake-up jitter of the monitoring thread (candidate 2 ms, requires agreement and measurement).
- Reporting latency (TIM-010) = FaultInfo report time − MISSED set time (candidate 200 ms, requires agreement and measurement). This value is a bound and not a guarantee.
- Both values are candidates until they are measured in the Linux target PoC. Detection once the deadline has elapsed does not mean zero latency (REQ-008 PRT-009 limitation).

**DeadlineMonitor unit tests** (`DeadlineMonitorTest.*`) are listed in the table below:

| Test | Verification |
|:--|:--|
| `DeadlineMonitorTest.DetectsWhileQueued` (QUEUED, no worker, deadline elapsed → MISSED and NOT_STARTED) | PRT-009, FR-N-027/028 |
| `DeadlineMonitorTest.DetectsWhileRunning` (RUNNING, the worker loses the CPU → MISSED, THROTTLED follows CbsState) | PRT-009, FR-N-027 |
| `DeadlineMonitorTest.DetectsWhileIdleCpu` (detected even when no task is runnable) | FR-N-027 |
| `DeadlineMonitorTest.NoDoubleCountAgainstConcurrentDone` (race between the worker end CAS and the monitor CAS → missed once) | FR-N-028, §5.6 protocol 6 |
| `DeadlineMonitorTest.RetryOnChangedWord` (skipped if the R-c recheck differs) | §5.6 R-c |
| `DeadlineMonitorTest.StrictGreaterThan` (`now == t_dl` is not a miss) | REQ-008 §2.5, PRT-009 |
| `DeadlineMonitorTest.TerminalRecordOfDeactivatedSlotUntouched` (the terminal record of a deactivated slot is not touched) | FR-N-029 |
| `DeadlineMonitorTest.OldGenQueuedRecordNoMissAfterReactivation` (a QUEUED record with gen g1, different from the current generation g2, has no MISSED even when `now > t_dl`, and `missed` does not change; monitor gen check A9) | FR-N-029 |
| `DeadlineMonitorTest.StaleGenRecordChangesNothingOfNewRegistration` (a MISSED record and counter of the old generation: the new baseline and the new counter do not change, AC-008-15(c)) | FR-N-029, AC-008-15 |
| `DeadlineMonitorTest.FullQueueKeepsMarkAndCount` (when the queue is full, MISSED and `missed` are kept and only `miss_drops` increases) | FR-N-031 |
| `DeadlineMonitorTest.KeyTableClearedOnDeactivation` | §5.9 |
| `DeadlineMonitorTest.ScanCostBounded` (upper bound of a 512-record scan, host measurement target; actual target measurement is the PoC) | §5.9 cost upper bound |

---


---

## 6. `libttsched` v2 (`sample-apps/src/libttsched.{h,c}`) — Explicit Event Connection API

> SRS OI-14 resolved: `libttsched` is extended without a separate library (DDR-008 D-03).
> This section gives the application-side contract of the explicit Event connection (`ttsched_evt_post`, `ttsched_evt_wait`, `ttsched_evt_done`) and the implementation outline. The public API, constants, and shared-memory references are **proposals subject to Project Leader approval**. No implementation is done before approval. This section does not claim that implementation or verification is complete.
> REQ-008 §2 defines the concepts (Event, Job, arrival time, deadline). libttsched does not make the deadline determination, and BPF also does not count Jobs. The deadline determination is made by DeadlineMonitor (§5.9) in TIMPANI-N. The code in this section is a sketch that shows the acceptance convention and the race rules, and it was not compile-verified.

### 6.1 Public Header and Call Conventions (proposal)

The single source of the constants and structs is §5.6. The public header exposes only the part that applications see, and the SHM structs and internal constants go in the internal header `libttsched_shm.h` (new; it holds the §5.6 content as it is).

**Execution context (D-24).** The API runs on the calling thread, and the SHM is a passive store. The registered L2 worker TID is the target of `SCHED_EXT` / the timpani-n kernel BPF CBS, and the actual CPU cost of wait and done is also included in the worker CBS (the blocking wait time itself is not a CPU cost). The post cost belongs to the context of the calling poster, and the whole process does not become a target of the worker CBS. The input adapter and the policy choice and management of other application threads are outside the P1 scope. The placement and environment checks are kept. The condition that isolates only the worker, and the open item on TID movement and verification responsibility in N, follow DDR-008 D-24 and HLD-008 §5.3. This description does not add a cgroup move to the current binding sketch.

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

Call conventions and state:

- The public API names are post, Event wait, and done (REQ-008 §2). wait blocks until there is an Event to process. Within the same call, it confirms and records the Job start, hands over the Event, and then returns. If a QUEUED Event already exists, it can return without blocking. No separate public acquire API exists.
- **Job-start state transition (`QUEUED → RUNNING`), that is, confirmation of processing start,** is performed by the CAS (compare-and-swap) inside wait. The CAS is an atomic conditional update that changes the value to the new one only when the current value matches the state word that was read earlier. It is not an additional queue, a public API, or a scheduler operation, and it is not a payload copy. The actual order in §6.2 is: check the state and generation → transition CAS that preserves MISSED (on failure, re-read) → in the internal `take()`, record `t_begin`, set the open Job, and increment `started` → copy the metadata and payload into the `ev` buffer that the caller supplied → return 0. `take()` is not a public API, and the copy is performed after the CAS succeeds.
- QUEUED and RUNNING are the logical lifecycle of an SHM Event record. They are not membership in a BPF DSQ or whether the CPU is executing it now. While the worker processes the previous Job on the CPU, the next Event can be QUEUED. The RUNNING state of a started Job is kept until the end, including during preemption, throttling, and blocking.

- Worker loop: `for (;;) { if (ttsched_evt_wait(&ev) < 0) { ttsched_init(); continue; } process(&ev); ttsched_evt_done(TTSCHED_EVT_COMPLETED); }`. `ttsched_evt_done()` cannot be omitted. There is no implicit termination that is treated as completion. This is because a failed Job could be recorded as a success.
- Before `ttsched_evt_wait()` returns E_INACTIVE, it performs a short back-off (candidate 10 ms, same as 9209bc5). This keeps a retry loop from consuming CPU while the daemon is down or the task is not yet bound. The back-off is outside the RT cost bound.
- An `ttsched_evt_t` handle refers to a `task_id`. It stays valid after `ttsched_init()`, and at the next use it finds the slot again by name (a bounded scan of the fixed slot table, no system call). Even when a poster receives E_INACTIVE, it does not map again within the post call. Calling `ttsched_init()`, which is on the non-RT path, makes later posts accepted.
- Thread-local state (per worker thread): the slot pointer and index of this thread, the thread name (`task_id`), the open-Job flag, the private copy of the open Event (`gen`, `seq`, `t_arr`, `t_dl`, `t_begin`), and the slot pointer that the Job belongs to. The private copy is used only when the Job log is created, and it does not read the shared record again.
- Process-wide state: the SHM mapping pointer and its writable flag (atomic), and the handle table (16 entries, name, slot hint). Only `ttsched_init()` and `ttsched_evt_open()` replace the mapping, under `g_init_lock`. Post and wait do not take a lock. Calling `ttsched_init()` repeatedly on the same object does not increase the number of mappings (A7).
- Call permission: only one posting context at a time may post to one source (§6.4). Only one thread calls wait and done on a source. If another thread with the same comm calls wait or done, it is a contract violation (REQ-008 §2.7: there is one worker thread per source. A-03 defines only the comm name).
- Arrival time: if `t_arr_ns == 0`, the post call reads CLOCK_MONOTONIC. If a value is given, it must be CLOCK_MONOTONIC; a value in the future, or earlier than the previous arrival of the same source, is E_INVAL. A value earlier than the current time is accepted as is. A time that the adapter has already written is included in the response time, and admission does not overwrite it (§3.3 assumption 5).
- Cost bound: post, wait (when it does not block), and done use a bounded number of atomic operations, a `memcpy` of at most 64 B, and at most one system call (futex). They do not use the heap or a lock, and post and done do not block. The mapping in `ttsched_evt_open()` and `ttsched_init()` is done on the non-RT path. If the mapping is not writable, post and wait return E_INACTIVE and do not map again by themselves. This is the answer to the review comment on "lazy initialization and RT call bound."
- Trust (P1): every process that can write to the SHM object is trusted. A writer that writes incorrectly can corrupt any slot. Per-object isolation is P2 (F-14).

**Removed APIs and concepts**: `ttsched_evt_notify` from the earlier draft, the `ttsched_evt_wait` that returns a batch count `n`, the app-owned payload queue and the invalidate and clear convention, `TTSCHED_EVT_RESET`, `TTSCHED_EVT_BITSET`, and the marker wait point are retired. The reasons are recorded in the Change History and §6.3.

### 6.2 Implementation Sketch

The code below is a design sketch. All shared-memory accesses use `seq_cst` atomic operations, the C11 default order (`atomic_load`, `atomic_store`, `atomic_fetch_add`, `atomic_compare_exchange_strong`). Field accesses that are not stated explicitly follow the `_Atomic` declarations in §5.6.

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
        /* W0 is RUNNING for the open seq. The head cannot pass a RUNNING entry, and ABANDONED and seq mismatches are filtered out above (§6.3) */
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

**Atomic accesses per call (A25, candidate, before measurement)**: counted from the sketch above. The success path of `post` (no help, recovery, or rollback) makes 21 atomic accesses (12 loads, 6 stores, 3 RMW: `arrivals`, `accepted`, `counter`). A futex system call is at most 1. The take path of `wait` (no wake-up wait) makes 12 accesses (10 loads, 2 RMW: take CAS, `started`). The wait path adds 4 loads and 1 futex per scan. The success path of `done` (excluding the clock read) makes 8 accesses (4 loads, 1 store, 3 RMW: end CAS, `completed`, head CAS). The help, recovery, and rollback of post, the log-drop count of `done`, and the deadline-miss CAS are added to the numbers above. These numbers are candidates before measurement, and the upper bound of SRS FR-L-004 refers to this section.

### 6.3 Interleaving Rationale

- **R-a, no overwriting.** `head` passes entry `s` only by CAS, and it passes it only when the word of that entry is `(s, terminal)`. Entry indexes are reused only after `head` has passed them, so the fields of a QUEUED or RUNNING record are not rewritten by the poster. After the worker obtains RUNNING with the take CAS, it copies the payload and the times, and the entry stays fixed until the end transition and the advance of `head`.
- **FIFO and single take.** Only the worker takes the entry at the `head` position. `head` passes only terminal entries, and taking is the CAS that changes `(s, QUEUED)` to `(s, RUNNING)`. The same entry cannot be taken twice. The CAS of the close procedure (QUEUED→CANCELLED) and the worker's take CAS compete for the same word, and exactly one wins. The loser reads again.
- **No lost wake-up.** The waiter reads the wake-up word `c` before the scan. Post (f), and the activation, deactivation, and close of TimerMaster, increment `counter` after they change the state. So if the waiter missed a post in the scan, the later increment of `counter` either makes the value comparison of `FUTEX_WAIT(c)` fail or wakes the waiter. If post (f) fails and the entry becomes REVOKED, it does not need to wake anyone. A rejected post is not an Event, so the worker has nothing to do, and the next accepted post or the next change in TimerMaster wakes the worker.
- **Post and deactivation (store-then-load pair).** The poster makes the entry visible (stores `word`) and then reads `kind` and `gen`. TimerMaster stores `kind = NONE` and then reads the `word` of every entry. In the global order of `seq_cst`, at least one of the two loads sees the other side's store. If the poster sees `kind` as EVT, the close procedure sees the entry and closes it as CANCELLED. If the poster sees NONE or a different `gen`, the rollback in (f) handles REVOKED or CANCELLED. There is no case where both miss. Each side's transition is one successful CAS, so the count increases only once.
- **Post across a registration boundary.** If activation changes `gen` after the poster has read `g`, (f) fails and the post goes to rollback. If the close procedure of the activation has already closed that entry as CANCELLED, the poster only increases `accepted` and returns 0, not E_INACTIVE (an Event was created and then closed). If the close had not happened yet, the entry becomes REVOKED. The worker closes a QUEUED record whose `gen` differs as CANCELLED. The boundary where counts may be mixed before and after reading the baseline values (`base_*`) is a limitation in §6.4.
- **Post rollback versus worker take.** The rollback changes only a QUEUED entry, using CAS. If the worker took it first to RUNNING, the rollback sees "already being processed" and ends it as an acceptance. If the rollback changed it to REVOKED first, the worker's CAS fails, and the worker passes the terminal entry with `head`. A REVOKED entry is not a target for taking.
- **End and close procedure.** The worker's end CAS (ii–iv) and the close procedure's CAS (RUNNING→ABANDONED) compete for the same word. If the end wins, the close procedure skips the terminal entry. If the close wins, the worker's W0 re-read sees ABANDONED and ends with E_INACTIVE. In both cases, only one of `abandoned` or `completed/discarded` is counted.
- **Seq check before end (A6).** End (i) compares the seq of W0 with the seq of the open Job. An ABANDONED entry is terminal, so after head has passed it, seq s+QDEPTH can reuse the same slot. If seq is not checked, the CAS of done overwrites the Event that follows. `RaceTest.AbandonedEntryReusedBeforeDone` covers this path.
- **Completion and monitor race.** The monitor CAS and the end CAS compete for the same word. If the end CAS is first, the monitor sees the terminal entry and skips it. If the monitor set MISSED first, the end CAS fails, and the end re-reads W0 and re-reads the clock. A mark that the monitor sets after seeing `now > t_dl` comes before the clock that the end reads, so the recorded `t_end` is later than `t_dl`. Therefore the recorded `t_end` and the MISSED flag do not disagree. If the end CAS succeeds, `t_end` is the clock just before the CAS, and MISSED was not set at that time. The side that adds MISSED increments `missed++` only once.
- **Concurrency of the close procedure with take, end, and monitor.** The close procedure scans all QDEPTH entries and uses CAS only on the value it read for each entry. It changes QUEUED to CANCELLED, and RUNNING (at worker end) to ABANDONED. If an entry passes its deadline while the monitor handles it, MISSED is put into the same CAS, and `missed++` is done once. The close procedure does not bound its range by `head` and `tail`, so even when it races with the advance of `head`, it neither misses an entry nor closes one twice (R-c).
- **Daemon restart.** Startup cleanup writes `magic = 0`, and then writes `kind = NONE`. Post (a) looks only at `kind` (A2), so `kind = NONE` blocks posts. A post that passed (a) before this point is cleaned up as CANCELLED if it became visible before the close procedure of startup cleanup, and if it became visible after startup cleanup, (f) sees `kind` as NONE and changes it to REVOKED. In neither case does an entry of the old object affect the new object. A worker that was attached to the old object receives E_INACTIVE and attaches to the new object with `ttsched_init()`. Because `gen` carries the new `instance_id`, `(gen, seq)` is probabilistically unique across restarts (A14).

### 6.4 P1 Limitations

- **A posting context is one per source.** Post takes no lock. If two contexts post to the same source at the same time, only the queue of that source can be corrupted, and libttsched does not detect this. Multi-poster posting is P2 (F-13). A poster is a thread of the worker process or another process that has permission to write to the SHM object.
- **Writes by trusted parties.** Every process that writes to the SHM object is trusted. Per-object isolation and a daemon heartbeat are P2 (F-14).
- **Daemon outage.** If the daemon exits abnormally, the old object remains with `kind = EVT`. Posts are accepted, and the worker keeps processing. Only the end transition of the worker determines a late completion. No one detects in real time an Event that has passed its deadline in the queue or during execution. The startup cleanup of the next daemon confirms it as `DETECTED_LATE`, and it appears only in the final statistics. There is no heartbeat.
- **One count at the re-registration boundary.** The `arrivals` and `mit_viol` of one post that arrives during activation may be included in the counts of the new registration (at most one per registration). If the last Job of the previous binding attached MISSED with the worker's end transition, and the new registration reads its baseline value before `missed++` has run, then that `missed++` is included in the new registration (at most one per binding). In both cases only the attribution of one count changes, and the determination and Event processing are not affected.
- **Poster death (A8).** If the post's process dies after the entry becomes visible, that Event is visible and monitored. (i) If it died before the advance of `tail`, the next post recovers it with R-b and increases `accepted`, and the (f) of that post wakes the worker. (ii) If it died after the advance of `tail` and before (f), one `accepted` count is lost and is not recovered. In both cases the worker is not woken until the next post or a change in TimerMaster, so in the meantime this Event is not processed and only receives deadline monitoring. If the poster dies before (e), the entry does not become visible and leaves no trace.
- **u32 wrap-around.** `seq` wraps back after the 2^32nd accepted post of an object. At 1 kHz posting, this is about 49.7 days. The ring logic is safe under wrap-around (QDEPTH is a power of two, and the comparison is u32 equality). However, the uniqueness of `(gen, seq)` is guaranteed only within 2^32 posts under the same `gen`.
- **Activation gap.** Between the `sched_ext` switch (§5.1 (C) step 4) and slot activation (step 5) in the binding, a post receives E_INACTIVE. This gap is not accepted, so it does not create a count mismatch. Whether to accept this gap is an open item (§12, L-14).
- **Post during TT republication.** Post (a) looks only at `kind` (A2). Even if TT table republication toggles `magic`, an EVT post is not rejected. `ttsched_init()` checks `magic` and `version`, so it does not newly map an invalid object. This decision closes L-15 (§12).
- **No wait cancellation API.** P1 has no means for an app to wake a waiting worker without an Event (§12, L-8).
- **Take and deactivation gap (A12).** After the worker passes the `kind` check, if deactivation writes `kind = NONE` and the take CAS succeeds before the close sweep reaches that entry, the Job starts. This Job remains RUNNING, the worker processes it to the end and records it with `done()`, and it continues to receive deadline monitoring. P1 allows this behavior. If `kind` is read again just before taking, the gap becomes smaller but is not closed.
- **ttsched_init mapping (A7).** Calling `ttsched_init()` repeatedly on the same object does not increase the number of mappings. When the object is replaced, the old mapping remains (limitation).

### 6.5 Test List (libttsched host tests, new `sample-apps/tests/libttsched/`)

These are tests that write to the same object from two or more threads on the host. To widen the race window, a delay injection point (`TTSCHED_TEST_HOOK`, compiled only in test builds) is placed. There is no claim that these tests have been executed.

| Test | Verification |
|:--|:--|
| `LibttschedTest.FirstPostOnFreshObjectUsesSeqZero` (zero-filled object; the first post makes `(0, QUEUED)` visible, and the id is `(gen, 0)`) | §6.2 post (e), FREE state |
| `LibttschedTest.PublishedTailNotAdvancedIsRecovered` (reproduces a poster dying before the advance of tail; the next post advances tail first and increases `accepted` once) | R-b recovery |
| `LibttschedTest.OversizedPayloadRejectedWithoutCounting` / `FutureOrDecreasingArrivalRejectedWithoutCounting` | E_INVAL, no change to counts |
| `LibttschedTest.InactiveSourceNotAnArrival` (if kind is NONE, E_INACTIVE is returned and `arrivals` does not increase) | §6.2 post (a) |
| `LibttschedTest.MitViolationCountedOnlyAsCount` (an arrival closer than the MIT is accepted and only `mit_viol` is increased) | D-05, §5.6 |
| `LibttschedTest.FullQueueCountsOverflowAndHelpsHead` (terminal entries are passed with help from head; if the queue is still full, E_FULL and `overflow`) | R-a, post (d) |
| `LibttschedTest.WaitTakesOldestFirst` | FIFO (R4) |
| `LibttschedTest.WaitInactiveBacksOffThenReturnsEInactive` | §6.1 back-off |
| `LibttschedTest.WaitWhileJobOpenIsEState` / `DoneWithoutOpenJobIsEState` | §6.1 call conventions |
| `LibttschedTest.InvalidOutcomeEndsJobAsDiscarded` (returns E_INVAL and the open flag is cleared) | E_INVAL, done call conventions |
| `LibttschedTest.DoneOnTimeIsNotMissedEvenWhenCollectedLate` (`t_end = t_dl` is not a miss) | REQ-008 §2.5, PRT-009 |
| `LibttschedTest.DoneOneTickAfterDeadlineIsMissedOnce` | strict inequality, count once |
| `LibttschedTest.DoneAfterAbandonedReturnsEInactiveAndClearsOpen` | Protocol 4 (i) |
| `LibttschedTest.JobLogOverflowCountsDropOnly` | Job log drop count, counters unchanged |
| `LibttschedTest.StaleGenerationRecordCancelledByWait` | §6.2 close_stale |
| `LibttschedTest.InitReattachesAfterInstanceChange` | §6.1 re-initialization |
| `LibttschedTest.HandleSurvivesInitAndResolvesByName` | handle re-resolution |
| `RaceTest.PostVsDeactivateStoreThenLoad` (two threads; the publish→load kind of post and the store kind→load word of deactivation; neither is missed) | store-then-load pair 1 |
| `RaceTest.ActivationGenVsPostStoreThenLoad` (gen store of activation and gen load of post) | store-then-load pair 2 |
| `RaceTest.WaitNoLostWakeupAgainstPublish` (between the waiter's read of c and the post's making the entry visible and its counter increment) | no lost wake-up |
| `RaceTest.TakeVsCloseExactlyOnce` (take CAS and close CAS) | FIFO and single take |
| `RaceTest.PostRollbackVsWorkerTake` (rollback CAS and take CAS; the result agrees as an acceptance) | rollback versus take |
| `RaceTest.PostPublishedAfterDeactivationRevoked` (post (a) passed, then deactivation; after the entry becomes visible, REVOKED at (f)) | post rollback path |
| `RaceTest.CloseVsWorkerEndAbandon` (close CAS and end CAS; exactly one wins; if the end loses, E_INACTIVE) | Protocol 4 (i)–(iv) |
| `RaceTest.MonitorVsDoneSameBoundary` (monitor CAS and end CAS when `t_end` is near `t_dl`; the recorded `t_end` and MISSED agree, and `missed` is counted once) | Protocols 4, 6 |
| `RaceTest.MonitorMarkBetweenClockAndCas` (if the monitor mark arrives between the end's clock read and CAS, the end re-reads the clock and records it as late) | Protocol 4 retry |
| `RaceTest.HeadHelpVsHeadHelp` (head CAS of two helpers; advances once only) | R-a |
| `RaceTest.StaleGenCancelVsPostRollback` | close_stale versus rollback |
| `RaceTest.DaemonRestartInvalidateVsPost` (a post after kind = NONE is E_INACTIVE and is not counted, A2) | Protocol 7 |
| `RaceTest.AbandonedEntryReusedBeforeDone` (a RUNNING record is closed as ABANDONED, head passes it, and seq s+QDEPTH is posted to the same slot; if the worker then calls done, it returns E_INACTIVE and does not overwrite that entry) | Protocol 4 (i), A6 |
| `RaceTest.TakeAfterKindNoneCompletesNormally` (if the take CAS succeeds before the sweep even after kind = NONE, the Job ends normally, is recorded, and is monitored) | §6.4 take and deactivation gap, A12 |
| `RaceTest.RecoveredPublishedRecordWakesWorkerOnlyOnNextPost` (the entry of a poster that died before the advance of tail wakes the worker at (f) of the next post) | post (d) and (f), A8 |
| `LibttschedTest.DiedAfterTailAdvanceMissesOneAcceptedCount` (the entry of a poster that died after the advance of tail and before (f) is not recovered and misses one `accepted` count; the next post wakes the worker) | A8 (ii), §6.4 |
| `LibttschedTest.RepeatedInitDoesNotGrowMappings` (after calling `ttsched_init()` 1000 times on the same object, the number of mappings in `/proc/self/maps` does not change) | A7, §6.1 |

---

## 7. Sample Apps

### 7.1 `sample-apps/src/evttask.c` (new, generic Event generator: the reference verification means for the acceptance criteria)

Only the kind of Event source (timer, datagram, pipe, self-generated) changes, and the TIMPANI configuration and worker code stay the same. The application-side explicit connection uses only `ttsched_evt_open`, `ttsched_evt_post`, `ttsched_evt_wait`, and `ttsched_evt_done`. The `--marker` option and the "adapter K" path do not exist.

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

- `--midjob-block-us` is not an option that reproduces the A-06 constraint. Blocking during a Job is allowed and observed, so this option creates the case where blocking is outside the guarantee of response time and admission.
- `--count K` stops generation after K Events. The worker's `done` result is written to `--log`.
- `cbstask.c` is kept, but the README lists `evttask` as the recommended generator.
- **Options for C measurement (required tool work, not implemented, HLD-008 §8.2, OI-23):** selection of input content (specifying the payload content), replay of the recorded input order, and a function that reads `CLOCK_THREAD_CPUTIME_ID` right after the worker returns from `ttsched_evt_wait` and right before the next call, and writes the difference to `--log` as the CPU time per Job (including `done`. CPU time spent inside wait is not included). The arrival interval can only produce a period and a burst through `--mit-us` and `--burst`. The three items above are a design list, and they are not yet in `evttask.c`. Option names are decided at implementation time.

### 7.2 FCW Adapter Reduction (`examples/someip/radar_based_fcw/fcw_consumer.cpp`)

The current file uses `timpani_rt.h`, `timpani_evt_register`, `timpani_evt_mark_arrival`, `timpani_cbs_bind_thread`, and `cbs_cv`. The items to modify and the reasons are in HLD-008 §8.4. This section shows the shape of the code after modification.

| Current call (location in file) | After modification |
|:--|:--|
| `#include <timpani_rt.h>` (line 7) | `#include <libttsched.h>` |
| `timpani_rt_init()` (main, line 187) | Deleted. The worker calls `ttsched_init()` |
| `timpani_evt_register(wl, task, svc, inst, evt, 18000)` (main, line 198) | `g_src = ttsched_evt_open("cbs_task")`. The MIT is the workload declaration (`min_inter_arrival_us`) and is not in the application code |
| `timpani_evt_mark_arrival(svc, inst, evt, seq)` (on_obstacle_alert_received, line 142) | `ttsched_evt_post(g_src, t_arr, target, sizeof(*target), &id)` |
| `cbs_cv.wait(lock, ...)`, `cbs_cv.notify_one()`, `notify_all()` (lines 62, 74, 150, 227) | Calls to `ttsched_evt_wait` / `ttsched_evt_done`. The condition variable and the queue are deleted |
| `timpani_cbs_bind_thread()` / `timpani_cbs_unbind_thread()` (lines 86, 129) | Deleted |
| `cbs_queue` (`std::queue`, `cbs_mtx`, `cbs_cv`) | Deleted. The payload of an accepted Event comes directly in `ev.payload` |

Code shape after modification (sketch, before compile verification):

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

- `t_arr` is read in the first statement of the handler (A17). Receive processing and lock wait are included in the response time (HLD-008 §8.2). If posts for the same source are called from several threads, the `t_arr` order can become inconsistent, and E_INVAL can occur. This section takes as its premise the H-8 confirmation that the handler is called from only one thread, and until it is confirmed, E_INVAL is counted in `g_invalid`.
- The worker does not print per-frame console output (`std::cout`). No log ring is added (A20). The correspondence between frames and Jobs is made by `(gen, seq, t_arr)` in the Job log and the TIMPANI trace. As a result, the per-frame console output of the FCW demo is reduced (§12, L-16).
- `DISCARDED` is used only when the application has an explicit discard policy. FCW has no such policy.
- The producer (`mock_radar_producer.cpp`) removes `#include <timpani_rt.h>` and replaces `timpani_rt_init()` and `timpani_tt_register(...)` with `pthread_setname_np(pthread_self(), "tt_task"); ttsched_init();`. The rest stays the same.
- Removed: `libtimpani_rt` (`timpani_rt.h`, `timpani_rt_shm.h`, `libtimpani_rt.cpp`), the `/timpani_rt` SHM, and the MIT determination and the 3 ms E2E determination in the library. Kept: `--overrun-ms`, `-be`, `--burst-ms`, `--tt-overrun-ms`. `--wakeup-delay-ms` moves to the receiving side.

---

## 8. Timing Analysis (Worst-case paths)

The values below are candidate values for planning. They are not agreed and have not been measured on the Linux target. The measurement method column states which record is used to obtain each value. Values before measurement are not used as guarantees.

| Path | Component | Budget (candidate) | Measurement method | Corresponding requirements |
|:--|:--|:--|:--|:--|
| Arrival → runnable (t_arr → runnable, $L_{post}$) | Post (a)–(f) atomic operations and futex wake-up → worker activation (`runnable`) | ≤ 50 µs p99.99 (candidate, reference poster configuration) | Difference of `t_act` in the corresponding execution-interval record in the same worker waiting state as `t_arr` in the Job log | TMG-016, TIM-011 |
| Arrival → first execution (delivery, $L_{del,max}$ = $L_{post}$ + $L_{wake}$) | The above + runqueue → first execution (TIM-001, $L_{wake}$ = 50 µs candidate, idle CPU without TT) | ≤ 100 µs candidate (of the admission formula's $L_{del,max}$) | Difference between `t_arr` in the Job log and `t_start` of the execution interval (excluding 0) | TMG-016, TIM-011, §3.3 |
| Activation → execution start (during TT) | The above + $B_{tt}$ (continuous TT occupancy) | ≤ $B_{tt}$ + 50 µs (candidate) | Compare `t_start − t_act` of the execution interval with the TT slot record | TMG-002 |
| End of window (`replenish_at`) → runnable (throttled server, idle CPU) | Wake-up jitter → `bpf_prog_test_run` → `kick_cpu` → `dispatch`, promotion (≤ 16 iterations) | ≤ 100 µs p99.99 (candidate) | Replenisher log and restoration time of `remaining_us` | TMG-003 |
| Budget overrun | Slice expiry = tick boundary (HZ=1000: at most 1 ms) + `stopping` charge | δ_ovr = 1 ms (candidate; tick restart delay to be measured under nohz_full) | Compare the actual accumulated on-CPU time per window with the budget (including cases of sleeping without throttling); compare with the tick period | TMG-004, §3.3 δ_ovr |
| Deadline-miss detection (TIM-009) — t_dl → MISSED set | DeadlineMonitor periodic scan (1 ms candidate) + monitoring thread wake-up jitter. Waiting, running, throttled, and idle are all included | ≤ 2 ms candidate (scan 1 ms + jitter) | `t_detect − t_dl` of the miss record; per scenario of queue wait, throttling, and idle CPU | TMG-014, §5.9 |
| Deadline-miss report (TIM-010) — MISSED set → report received | FaultMonitor polling (≤ 100 ms) → gRPC Write → O reception | ≤ 200 ms candidate (only for reports that were sent) | Difference between `t_detect` of the miss record and the timestamp of O's reception log | TMG-015, §5.5 |
| Completion collection delay | From the worker's end to collection by TIMPANI-N | No bound. It has no effect on the deadline determination (§1.2) | Determined from `t_end` in the Job log, so it is not a measurement target | PRT-009 (a collection delay is not a deadline miss). TMG-005 is withdrawn |
| Post·wait·completion cost | Atomic accesses: post 21, wait take 12, done 8 (candidate values counted in §6.2), copy of at most 64 B, at most 1 futex (post) | ≤ 2 µs candidate (measurement required) | Host microbenchmark and target measurement (post mean and p99) | TMG-011 |

- TMG-005 (DMISS determination at completion time) is withdrawn from the SRS. Deadline-miss detection is handled by TMG-014, and the determination at completion time remains as MISSED (`LATE_COMPLETION`) in the completion record.
- TMG items that are not in the table are checked by test, not as worst-case paths: TMG-007 (binding time $T_{bind}$, the rescan period + scan time in §5.2), TMG-009 (count accuracy, §10 AC-008-01), TMG-010 (CPU pinning, §10 AC-008-06), TMG-012 (fault report content, §10 AC-008-11), TMG-013 (reapplication, §10 AC-008-09).
- L1 impact (TMG-008): the only code that the L2 change adds to the TT path is one branch in `select_cpu` and the `cpu_is_isolated` check in `dispatch`. The kick program does not send a kick to a CPU whose TT slot is active (§4.4). The monitoring thread selects a CPU so that it does not use the same CPU as a TT slot. Its impact is to be measured in PoC item P-1.

---

## 9. Error Handling and Failure Modes

| Situation | Handling |
|:--|:--|
| L2 task not found (when the table is applied) | Not an application failure. Unresolved list → rescan. `TableApplied.success=true`, log WARN |
| Update failure of `cbs_map` / `task_meta_map` | Only that entry is recorded as `apply_error`, and processing continues with the rest |
| ringbuf out of space | `fault_drop_count++`. JobTraceCollector reads it periodically and raises WARN |
| Load or execution failure of `timpani_replenish_kick` | If detected at startup, automatically switches to `KickBackend::KickerThreads` and logs ERROR |
| SHM v1 consumer (old `libttsched`) | Because magic or version does not match, post and wait receive E_INACTIVE. **Explicit exception to NFR-003 (TT no regression)**: an application built with v1 must be rebuilt. Deployment order: build the application with `libttsched` v2 → replace it together with timpani-n (v2) |
| MIT = 0 (transition rule also not possible), or T > MIT, D > MIT | Rejected at O with `ParamInvalid` (§3.2) |
| task_id longer than 15 B, or duplicated within the node | Rejected at O with `ParamInvalid` (§3.2) |
| Task exit | If the `bind_gen` of the `exit_task` record is the same as the binding being tracked, deactivate (worker exit → close rule, RUNNING → ABANDONED; if the deadline was passed, MISSED and DETECTED_LATE). If different, ignore (stale). After that, return to unresolved |
| Source not registered or inactive, or no SHM object (before daemon start-up or after normal exit) | Post returns E_INACTIVE and is not counted as an arrival. The application retries after `ttsched_init()`. It does not need to obtain the handle again after registration |
| SHM mapped read-only while posting | Post and wait receive E_INACTIVE. It does not map again. Write permission is `0666` (§5.6, OI-15) |
| Queue full (E_FULL) | `overflow++`. A rejection is not an Event. The application counts rejections and does not process them. If the worker falls behind, it is observed as `arrivals − accepted` |
| Event arrives during an active Job | Accumulates as QUEUED (R2). When the current Job calls `done`, the next `wait` returns without sleeping (R3). No loss |
| `wait` called during an active Job, or `done` called without a Job | E_STATE. This is a violation of the call order (application bug) |
| Invalid argument (payload too large, future or early t_arr, invalid outcome) | E_INVAL. An argument error in post counts nothing. If the outcome of done is invalid, the Job ends as DISCARDED (`discarded` counted) and E_INVAL is returned |
| Poster dies between (a) and (e) | Before the entry becomes visible, no trace is left. If tail has not advanced after it becomes visible, the next post recovers it and increases `accepted` (R-b). If it is after the advance of tail and before (f), one `accepted` count is lost and is not recovered. In both cases the worker is not woken until the next post or a change in TimerMaster, and in the meantime the Event receives only deadline monitoring (§6.4, A8) |
| Worker dies during a Job | The RUNNING record is closed with the close rule (ABANDONED) after TaskRegistry detects the exit. Monitoring keeps checking until RUNNING becomes terminal. If the deadline was passed, MISSED and DETECTED_LATE |
| Worker calls done before or after the close | If ABANDONED, E_INACTIVE and the open flag is cleared. A late end after the close rule has closed RUNNING returns through this path |
| Daemon abnormal exit (SHM object remains and `kind = EVT`) | Posts are still accepted, and the worker keeps processing. Only the worker's end transition determines a late completion. Expiry during queue wait and execution is not detected in real time, and it is confirmed late as `DETECTED_LATE` at the next startup cleanup. No real-time report (§6.4, §1.3) |
| Daemon restart (after normal or abnormal exit) | Startup cleanup: `magic = 0` → `kind = NONE` for all slots → close rule → wake-up → record final counts → unlink → create new object (§5.6 Protocol 7). The application receives E_INACTIVE and attaches to the new object with `ttsched_init()`. `seq` starts again from 0, but because `gen` is different, `(gen, seq)` is probabilistically unique across restarts (A14) |
| Post during daemon restart (`kind` NONE period or old object) | If `kind` is NONE at (a), it is rejected and returns E_INACTIVE. If `kind` appears as NONE at (f), it rolls back (A2). An entry cleaned up after it becomes visible is closed as CANCELLED or REVOKED and has no effect on the new object |
| Monitoring thread stalled or overloaded (monitor stall) | Detection becomes later than TIM-009. Deadline misses are confirmed late at worker exit or by the close rule. The periodic counter of the monitoring thread is recorded in the process-internal log, and a period overrun is logged as WARN. The counting path is aligned to the exit time |
| Miss-record queue full (miss-record queue overflow) | The MISSED mark and `missed++` are already complete through CAS and atomic operations, so they are kept. Only the record is discarded, and `miss_drops` is counted. The reporting basis of the counting path is not affected, and only the detail path shrinks |
| Job log full (Job log overflow) | The entry is discarded and `log_drop` is counted. Worker counts (`completed`, `missed`, etc.) are not affected. The detail row for that Job is missing, and it appears in the trace as a difference in `log_drop` |
| Duplicate in detail path (miss record and Job log with the same (gen, seq)) | Duplicates are removed by `(task, gen, seq)`, and the result is kept as one detail record |
| One post at the re-registration boundary, one `missed++` of the last Job | One count may be attributed to the previous or the new registration (§6.4 limitation). It has no effect on determination or Event processing |
| Table reapplication (parameters unchanged) | Nothing is written. The slot's `gen`, `kind`, counts, and `run_seq` remain as they are (§5.1 (C) D-17(a)) |
| Table reapplication (parameters changed) | The binding is released by the close rule and is activated again with a new `gen`. A Job in progress is closed with the record of the old `gen` |
| TT republication (`magic` toggle) | Posts are not rejected (A2). Post (a) and wait look only at the `kind` of the EVT slot, and TT republication does not change the `kind` of the EVT slot (§6.4, §12 L-15 closed) |
| Several threads with the same comm on the node | The first match is used, and WARN is logged (current behavior). If two threads call wait or done for one source, it is a contract violation (§6.1) |
| Number of L2 tasks exceeds the number of SHM slots (64) | That entry is an apply error. Because slots are fixed to names (§5.6), they are not reused before a daemon restart |
| `CbsConfig.cpu` missing, or not an isolated CPU | That entry is an apply error (§5.1 (B)) |
| Activation gap (after the sched_ext switch, before slot activation) | Post returns E_INACTIVE and is not counted (§6.4, §12 L-14) |
| Blocking during a Job (mid-job blocking) | The Job boundary is explicit, so the Job remains one. The execution interval is split at each block (§4.5). A wake-up after blocking does not grant a new budget within the open window. A blocking Job is outside the guarantee of admission, and its deadline is monitored |

---

## 10. Test Hooks and Acceptance Criteria Mapping

The `SCX_ENQ_LAST` regression test is run per queue, with no other runnable task. On the BE of a non-isolated CPU and in the global fallback, the last task must run again without an external wake-up, and TT must keep its existing assigned CPU and slot protection. The BE test is run on a non-isolated CPU that can consume BE under the existing placement policy. A throttled CBS must run again only by promotion after replenishment, not through this rescheduling path.

All of AC-008-01 ~ 19 are mapped. AC-008-13 ~ 16 are new. AC-008-17 ~ 19 were added in v5.1. The means are `evttask` (§7.1), FCW (§7.2), `mock_pullpiri.py`, GTest, or a procedure. A "hook" is a record or count used for a determination. The Job log corresponds to Events by `(gen, seq)`.

| AC-008 | Means | Hook |
|:--|:--|:--|
| 01 (one Event–one Job, 1:1) | (a) `evttask --source timerfd`, `udp`, `pipe`, `self`, 100,000 Events each, MIT respected. (b) Check the number of generator log entries under the same conditions | In the quiescent state, `completed + discarded + cancelled + abandoned = accepted` (I1). The `(gen, seq)` set of the generator = the `(gen, seq)` set of the Job log (for completed and discarded Jobs). `started = completed + discarded + abandoned`. Since no interval violates the MIT, `mit_viol = 0` |
| 02 (TT no regression) | TT table fixed + `evttask --burst` A/B, 1 h | tt row jitter p99.99 in the trace; L1 DMISS = 0 in FaultMonitor |
| 03 (CBS budget enforcement, BUDGET_EXCEED) | Deterministic charge boundaries: sleep right after exact exhaustion, a sole runnable after exhaustion, a wake-up where replenishment is not possible, a wake-up where replenishment is possible (including the `tol` boundary). On the Linux target: return after preemption by a sole runnable and by a higher class, TT and BE regression when `SCX_ENQ_LAST` is applied, and promotion verified with `evttask --overrun-us` | In the queue and `sched_switch` records, no re-execution before replenishment; the count of throttle episodes per actual entry, and one emission; no unnecessary throttling on sleep or eligible wake-up. Overrun execution and promotion delay are determined separately by TIM-004 / TIM-003 |
| 04 (deadline detection and throttling, once only) | (a) Exceed D with `evttask --overrun-us`, FCW `--overrun-ms 10`. (b) Execution time is less than D, but it waits behind other work: `evttask --burst N`, or arrival overlapping a TT block longer than D | One deadline miss per Job (0 duplicates). (After approval) `NOT_STARTED` and `THROTTLED` flags in (b). `t_detect − t_dl` of the miss record ≤ TIM-009 candidate. O reception log ≤ TIM-010 candidate (only reports that were sent) |
| 05 (external generator, delivery latency) | `evttask --source udp` external generator (another node or CPU) | L_wake = `t_start − t_act`, split into Gap state (TIM-001) and TT occupancy state (TIM-002), aggregated at p99.99. L_post = `t_act − t_arr` (TIM-011). L_del = `t_start − t_arr` (TMG-016, `t_arr` in the Job log and `t_start` of the execution interval). L_post is measured with the poster thread in the reference poster configuration (A-10), and determined against the TIM-011 upper bound. Only Events that arrive while the worker is waiting are counted. |
| 06 (isolated CPU set) | 1 h rated load + runaway | In the `perf sched` or `sched_switch` trace, the CPU set of `cbs_task` is only its assigned CPU |
| 07 (MIT violation count and report) | `evttask --burst N` (interval MIT/3). FCW reproduction: `mock_radar_producer --burst-ms 6` | `mit_viol` = generator violation count (arrival basis). If the `count` of all reported MIT_VIOLATION is summed, it equals the increment of `mit_viol` (a report is one per T_s window, §5.5). (After approval) `flags & 0x1` (MIT_RELATED) of the deadline miss. Deadline misses of other L2 tasks = 0 |
| 08 (admission) | GTest `GlobalSchedulerTest.FifoAdmission_*` (including rechecking of existing tasks), `SchedInfoServiceTest.RejectRestoresPreviousWorkload` | Unit test results (§3.5) |
| 09 (repeated add and remove stability) | `mock_pullpiri.py` add and remove, 10 times | Daemon PID unchanged. Zero remaining entries in `bpftool map dump`. Slot names in SHM stay fixed, and counts accumulate. An exit record of the previous `bind_gen` does not release the new binding (`TaskRegistryTest.StaleExitRecordIgnored`) |
| 10 (start-up delay) | Start `evttask` after the table is applied | Time of the `Registered CBS task` log − start time |
| 11 (fault report content) | `mock_pullpiri.py` reception log | Type, `count`, (after approval) `flags`, `job_seq`, `job_gen`. Before approval, only `count` is checked |
| 12 (middleware identifier separation) | `grep -rniE 'someip\|vsomeip\|service_id' timpani-o timpani-n sample-apps` = 0. Switch `evttask --source timerfd/udp/unixdgram/pipe/self` without changing the TIMPANI configuration, and recheck 01~07 | 0 matches, and the results of 01~07 do not depend on the source kind. (`instance_id` is an identification field of the SHM object (§5.6) and not a middleware identifier, so it is not included in the check pattern) |
| 13 (B arrives while A is processed: independent deadline, separate consecutive Jobs) | (a) B arrives while the `evttask` worker processes A with a long `--wcet-us`. (b) A situation in which B is already QUEUED right after A completes (`--burst 2`, consecutive without sleep) | (a) The `t_dl = t_arr_B + D` of B does not change to the time of A (`t_dl` in the Job log). (b) The Job log has two entries (different `seq`; the `t_begin` of B is later than the `t_end` of A. The difference is the delay from done to the next wait call). The execution-interval record is one entry (R3) (§4.5). Determination is made per Job: if only B is late, only B is a deadline miss |
| 14 (detection during wait and throttling, late collection, no duplicates, discard and cancel) | (a) Queue wait: hold the worker with `--midjob-block-us` so that the next Event passes `t_dl`. (b) Throttling: exhaust the budget with `--overrun-us` so that the waiting Event passes its deadline. (c) Late collection of an on-time completion: delay the JobTraceCollector collection with a test hook. (d) Discard or CANCELLED after the deadline: place the end transition after `t_dl` with a test hook | (a) Miss record `t_detect − t_dl` ≤ TIM-009 candidate, flag NOT_STARTED. (b) THROTTLED. (c) No deadline miss (`t_end ≤ t_dl`). (d) An end or close after the deadline is counted as a deadline miss, and `missed` is once per Job (checked together with `RaceTest.MonitorVsDoneSameBoundary`) |
| 15 (no processing of another Event's data on accept, overflow, cancel, re-registration, or restart; records of old generations do not change state) | (a) Overflow: hold the worker and use `evttask --burst` beyond QDEPTH. (b) Re-registration: load while parameters are changed with `mock_pullpiri.py`. (c) Load during daemon restart | Put the generator sequence number in the payload, so the sequence number that the worker receives in the payload is the same as the generator sequence number corresponding to `(gen, seq)` (0 mismatches). `overflow` equals the number of E_FULL. After re-registration, records of the old `gen` do not change counts or state (`ActivationTest.ClosesLeftoverRecordsBeforePublishing`, `DeadlineMonitorTest.StaleGenRecordChangesNothingOfNewRegistration`) |
| 16 (same contract for FCW and non-SOME/IP app: interference) | (a) Different isolated CPUs: run FCW (overrun with `-be`) and a non-SOME/IP app (the Unix datagram sensor app of HLD-008 §8) together. (b) Same CPU, revalidated parameters: raise the FCW D to 6000 µs, and set the D of the second app so that the §3.3 formula passes. (c) Default FCW parameters + a second L2 task on the same CPU | (a) and (b): deadline misses of the other app = 0, and the response time distribution is in the same range as the reference without overrun (within TMG-016 candidate). (c) Admission rejects it with `DeadlineInfeasible` (`GlobalSchedulerTest.FifoAdmission_SecondL2Rejected` and the `mock_pullpiri` application log). The numerical basis is HLD-008 §8 and the §3.3 arithmetic |
| 17 (CBS execution on a CPU without TT, FR-O-022, FR-N-032, FR-B-025) | (a) Place an L2 task on a CPU without TT of a node without TT slots or of a mixed node, and send the first Event with `evttask --source timerfd` or `udp`. (b) After idling for $T_s$ or more following the last L2 activity, send an Event. (c) After exhausting the budget with `evttask --overrun-us`, place no other activity on the same CPU. (d) Let the deadline pass while throttled or in the QUEUED state. Count TT_START and TT_END with BPF tracing (planned test `EvtIntegration.TtFreeCpuCbsLifecycle`) | (a) The acceptance, processing, and end are recorded, and one Job record is created. (b) The start delay is within the agreed upper bounds of TIM-011 and TIM-001 (TIM-011 in the reference poster configuration), and $C_s$ is given for the start interval. (c) Replenishment finishes within the agreed upper bound of TIM-003. (d) The deadline miss is detected within the agreed upper bound of TIM-009. Throughout, that CPU has 0 TT_START and 0 TT_END records |
| 18 (L2 preserved on first TT addition and last TT removal, RTU-001) | Leave the L2 entries as they are, and repeat the first TT task addition and the last TT task removal on that CPU 10 times with `mock_pullpiri.py`. Apply Event load during the repetition (planned test `EvtIntegration.TtAddRemoveKeepsL2State`) | The remaining budget and the replenishment window are not reset. The `(gen, seq)` and `t_dl` of open Events do not change. CANCELLED and ABANDONED caused by the update are 0. The daemon PID does not change. Timer Master thread reconstruction and epoch update may occur, and they are not a reason to reset L2 state |
| 19 (explicit failure on wrong cpuset placement, CBS-008) | Deliberately set the cpuset so that the assigned CPU lies outside the allowed CPU set of the worker, and register with `mock_pullpiri.py`. Then correct the cpuset (planned test `TaskRegistryTest.WrongCpusetFailsExplicitly`) | An error log remains at the time binding is attempted. That task is not switched to the SCHED_EXT policy. The BPF scheduler keeps running, and the other L1 and L2 tasks on the same node are not affected (L1 DMISS 0). From the time the cpuset is corrected, binding completes within the late-binding upper bound (TIM-006, T_bind) (the measurement start point is the correction time) |

---

## 11. File Change List and Implementation Order (P1 WBS)

| Order | Work item | File | Depends on |
|:--|:--|:--|:--|
| O-1 | Add proto fields (§2): `schedinfo` TaskInfo 11, FaultInfo 6–8. `node_control` CbsConfig 9–10, FaultInfo 10–13 (including `job_seq`, `job_gen`; reduced according to the approval scope). Generated code | `timpani-o/proto/*.proto`, Rust `timpani_rust/timpani-o/proto/schedinfo.proto` (fields only) | Approval required (§2, L-11), L-6 |
| O-2 | MIT, T, D verification (`validate_l2_task`: 0 < T ≤ MIT, D ≤ MIT, C ≤ D), transition rules, rejection of duplicate task_id, restoration | `schedinfo_service.{h,cpp}`, `global_scheduler.h` | O-1 |
| O-3 | Single admission formula (`L_del_max`, `δ_ovr`), C/T_s of worst-fit, `compute_tt_blocking`, min-gap rejection, two-layer publication, table_id | `global_scheduler.{h,cpp}`, `publication_coordinator.cpp` | O-2 |
| O-4 | Preservation of fault types and passing of `count`, `flags` (`job_seq`, `job_gen` upon approval) | `orchestrator_service.cpp`, `fault_client.{h,cpp}` | O-1 |
| O-5 | Add GTest (§3.5) | `timpani-o/tests/*` | O-2~4 |
| N-1 | BPF simplification: `maps.h` (TaskMeta `bind_gen`, `run_seq`, JobRecord `t_sleep_ns`, cleanup of execution-interval flags), `job_fsm.h` (removal of the MIT check and L2 DMISS), ops (`runnable`, `quiescent`, `bind_gen` of `exit_task`) | `timpani-n/src/bpf/*` | — |
| N-2 | Rewrite `libttsched` v2: public header, `post`, `wait`, `done`, `open`, `init`, internal header `libttsched_shm.h` (new), host tests (§6.5) | `sample-apps/src/libttsched.{h,c}`, `sample-apps/src/libttsched_shm.h` (new), `sample-apps/tests/libttsched/` (new) | Approval (L-9, API) |
| N-3 | TimerMaster SHM v2: layout and static assert, activation (Protocol 1), deactivation and close rule (Protocol 5), startup cleanup (Protocol 7), `update_evt_slots` (name only) | `timer_master.{h,cpp}` | N-2 (layout shared) |
| N-4 | TaskRegistry redesign (`bind_gen`, ignoring stale exits, `bind_mu_`), main.cpp branches (steps B0, C, and E) | `task_registry.{h,cpp}`, `main.cpp`, `bpf_loader.{h,cpp}` | N-1, N-3 |
| N-5 | DeadlineMonitor (new): periodic scan, CAS marking, miss-record SPSC queue, per-slot BPF key table | `deadline_monitor.{h,cpp}` (new) | N-3 |
| N-6 | JobTraceCollector (Job log and execution intervals), EventSourceMonitor (cumulative counts), FaultMonitor (counting path and detail path), NodeClient fields | `job_trace.{h,cpp}`, `event_source_monitor.{h,cpp}`, `fault_monitor.*`, `grpc/node_client.*` | N-1, N-3, N-5, O-1 |
| N-7 | Replenisher and kick backend: no change (§5.3). Keep tests | `replenisher.{h,cpp}`, `bpf_loader.*` | N-0 |
| N-0 | PoC (OI-9): `scx_bpf_kick_cpu` from a SYSCALL program, context return, promotion delay on an idle CPU, effect on TT jitter (existing item) | `timpani-n/src/bpf/*` (experiment branch) | — |
| N-8 | timpani-n GTest target (§5.8, §5.9) | `timpani-n/tests/*`, `CMakeLists.txt` | N-1~6 |
| S-1 | `evttask.c` (new), FCW adapter reduction (delete `cbs_queue` and the condition variable), retire `libtimpani_rt`, MIT fields in yaml and json | `sample-apps/src/evttask.c`, `examples/someip/*`, `scripts/*` | N-2, O-1 |
| D-1 | Document alignment: DDR-002 §7, DDR-005, DDR-007 §4.2 / §4.5 (replenishment meaning) / §4.5.3 / §7, DDR-012, `structure.md`, README, CHANGELOG | `doc/**` | All |
| T-1 | Integration script `scripts/verify_event_driven.sh`: AC-008-01~12 and the automatable items among 17~19, procedures for 13~16 | `scripts/` | All |

- **PoC items**: P-1 measures the cost and jitter of the DeadlineMonitor 1 kHz scan on the target CPU (under TT load and on the selected CPU). It judges whether event-based wake-up is needed for the agreed upper bound (F-10). P-2 measures the atomic operation cost of post, wait, and done, and $L_{post}$ (t_arr → runnable). P-3 measures $\delta_{ovr}$ under nohz_full and tick restart delay. P-4 measures $L_{wake}$ (TIM-001) and $L_{del,max}$ on an idle CPU, and the wake-up under TT load (TIM-002) is used to verify the $B_{tt}$ term. The N-0 PoC keeps the existing items.
- **Removed work items**: implementation and tests of `ttsched_evt_notify`, batch count wait, app-owned queue and invalidate and clear logic, `TTSCHED_EVT_RESET` handling, marker transition and K/M source transition, the MIT check of BPF and emission of `FAULT_MIT_VIOLATION`, and the L2 DMISS determination and early DMISS of BPF.
- Recommended parallelization: (O-1→O-2→O-3→O-5), (N-1 ∥ N-2) → N-3 → (N-4 ∥ N-5) → N-6 → N-8. S-1 follows N-2 and O-1. Each group is split into a separate PR (`feat(timpani-o): ...`, `feat(timpani-n): ...`, `feat(sample-apps): ...`).

---

## 12. Open Items (LLD level)

| # | Issue | Impact |
|:--|:--|:--|
| L-1 | Whether `scx_bpf_kick_cpu` can be called from a SYSCALL program, and context return (OI-9). PoC on a kernel 6.12.x build. Also confirm: the effect of a flag-0 kick on the jitter of a CPU that is running TT, and the Replenisher priority (TM−1) | §5.3 backend selection |
| L-2 | First record of an execution interval: whether `ops.runnable` is called at the time of the `sched_setattr(SCHED_EXT)` switch (observation distortion of the first execution interval right after the switch). Whether to place a flag on the first execution interval | Statistical accuracy (execution-interval records) |
| L-3 | Whether to keep `trace_enabled` as a `.bss` variable or as a one-entry ARRAY map (ease of access from the libbpf skeleton) | Implementation convenience |
| L-4 | Handle resolution rule — **resolved**: `ttsched_evt_t` refers to a task_id. For each post, the slot hint is verified by name, and if they do not match, the slot is found again by name. There is no global cache invalidation rule for slot indexes (§6.1, §6.2) | — |
| L-5 | Whether to integrate the `JobTraceCollector` CSV with the existing `gpdata` format. HLD-008 H-2 covers it, and it is not tracked here | Toolchain |
| L-6 | Whether to sync only the fields into the Rust proto (`timpani_rust/timpani-o/proto/schedinfo.proto`), or to reflect them all at once during the Rust port work | Rust CI |
| L-7 | **Withdrawn**. The kernel identification of the marker wait point (REQ-008 OI-17, DDR-008 F-8) is superseded by the explicit Job boundary (`ttsched_evt_wait` and `ttsched_evt_done`). The `sched_yield` candidate and the marker flag are retired. Basis for the replacement: §6.1, DDR-008 D-18 | — |
| L-8 | A means to wake a waiting worker without an Event. There is none in P1. The worker is not woken at process termination (HLD-008 §8 termination procedure). A dedicated cancel API (only raise the wake-up word, and `wait` returns E_INACTIVE or a cancel value) is decided in P2. The method in which an app creates a fake Event with `ttsched_evt_post` is not used | Termination path of an app that ends only the worker |
| L-9 | **Approval required (API)**: libttsched v2 public API (§6.1) and ABI change relative to SHM v1 (an application built with v1 must be rebuilt). `ttsched_evt_notify`, batch count, and `TTSCHED_EVT_RESET` of the earlier draft were never implemented, and this revision removed them from the design. Impact: FCW, evttask, producer | App build |
| L-10 | **Approval required (SHM v2)**: object layout and constants (§5.6: slot 2752 B, object 176192 B, MAX_TASKS 64, QDEPTH 8, PAYLOAD 64 B, JOB_LOG 32). Count type (u64) and reserved space of the Event record 24 B | Shared memory compatibility |
| L-11 | **Approval required (protobuf)**: `schedinfo` TaskInfo 11 (`min_inter_arrival_us`), FaultType DMISS·BUDGET_EXCEED·WATCHDOG(P2)·MIT_VIOLATION, FaultInfo 6–8. `node_control` CbsConfig 9–10, FaultInfo 10–13 (`job_seq` = 12, `job_gen` = 13, u64). Without approval, the report carries only the count (no flags, job_seq, or job_gen). The approval scope is set in the same way as D-11 | Protocol compatibility |
| L-12 | **Approval required (timpani-n internal API)**: `TaskRegistry::Binder` returns `bind_gen`, and `bind_gen` is passed to `track` and `notify_exit`. The EVT argument is removed from `TimerMaster::set_schedule_table`. New TimerMaster method added. Names of existing public methods are not changed (§9 principle) | Internal API. Treated as a signature extension |
| L-13 | **Approval required (numerical candidates)**: `DL_SCAN_PERIOD` 1 ms, detection upper bound 2 ms (TIM-009), report upper bound 200 ms (TIM-010), $L_{post}$ 50 µs (reference poster configuration), $L_{wake}$ 50 µs, $L_{del,max}$ 100 µs (TIM-011), $\delta_{ovr}$ 1 ms, CBS_ACT_TOL 50 µs, `tol_mit` 0, 10 ms back-off, DeadlineMonitor priority 97 (Replenisher 98), `MISS_Q_N` 256, `DETAIL_KEEP` 16. All of these are candidates before agreement and measurement on the Linux target | Targets, not guarantees |
| L-14 | Activation gap (E_INACTIVE between the sched_ext switch and slot activation). It is not accepted, so there is no count mismatch. Confirmation by the Project Leader is needed on whether to accept this gap | Post right after binding |
| L-15 | **Closed (A2)**: post (a) looks only at `kind`. Even if TT republication toggles `magic`, an EVT post is not rejected. Startup cleanup blocks posts with `kind = NONE`. No separate flag indicating object invalidity is kept. | Post during table application (resolved) |
| L-16 | Reduction of per-frame console output of the FCW worker (no log ring is added, A20). The demo output format is decided in HLD-008 §8 | Demo observation |
| L-17 | Interpretation of the determination 1 (to confirm): The completion time $t_{end}$ is the clock value that `done()` reads just before the end transition, and the recorded $t_{end}$ and the MISSED flag do not disagree. The determination is made at the time of the post, and a late collection is not a deadline miss. This interpretation is the reading of `REQ-008 PRT-009` and §2.5 (completion time t_end and race rules) (REQ-008 OI-20). The completion time is the completion of processing and of the result posting, and it is not the completion of network delivery or device reflection (A21) | Determination definition (AC-008-14) |
| L-18 | Interpretation of the determination 2 (to confirm): A Job that TIMPANI closed before the deadline (CANCELLED, ABANDONED) is not a deadline miss. If it is closed after the deadline, it is a deadline miss and DETECTED_LATE. This interpretation extends the amendment sentence "explicit discard and cancellation before the deadline" to the case where TIMPANI closes the Job | Determination definition (AC-008-14, AC-008-15) |
| L-19 | Open question: whether the target of the deadline-miss report is limited to the reports under the DDR-012 threshold rule, or whether it covers all deadline misses. If the threshold rule is followed, TIM-010 is defined only for reports that were sent | Report scope and AC-008-14 |
| L-20 | Open question: whether TIMPANI-O passes `job_seq` and `job_gen` to Pullpiri's `schedinfo.FaultInfo`. Currently `schedinfo` has no Job identifier | Pullpiri compatibility |
| L-21 | Open question: whether to merge the DeadlineMonitor and Replenisher threads (F-12). Currently they are separate | Scheduling design, RT cost |
| L-22 | Open question: whether to use TMG-011 as the post·wait·completion cost item (§8 current usage) and align it with the definition on the REQ side | Traceability |
| L-23 | Open question: the replacement method for the TT registration on the FCW producer side (`timpani_tt_register`) is finalized together with HLD-008 §8 | Sample apps |
| L-24 | C measurement procedure and margin ratio (HLD-008 H-19, REQ-008 OI-23): the records that give the CPU demand per Job, and the options needed by evttask (§7.1) | Basis for input C of the admission formula |
| L-25 | Basis for `t_arr` and responsibility for duplicates on re-post at the rebinding boundary (HLD-008 H-20, REQ-008 OI-24) | Adapter contract, deadline basis |
