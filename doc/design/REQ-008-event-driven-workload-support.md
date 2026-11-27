<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# REQ-008: Event-Driven Workload Support Requirements Definition

**Date:** 2026-10-25 (Last Updated: 2026-11-26)
**Status:** Draft (v5.3 — P1 Event–Job contract alignment, Issue #53. For design review; it does not mean that implementation or verification is complete)
**Author:** Human + AI
**Related documents:** [SRS-000](SRS-000-timpani-system.md), [DDR-001](DDR-001-workload-model.md), [DDR-002](DDR-002-scheduling-architecture.md), [DDR-003](DDR-003-interface-protocol.md), [DDR-004](DDR-004-resource-allocation.md), [DDR-005](DDR-005-sched-ext-bpf-scheduler.md), [DDR-007](DDR-007-tt-cbs-integrated-scheduling.md), [DDR-011](DDR-011-runtime-table-update.md), [DDR-012](DDR-012-fault-management.md), [P1-008 contract amendment agreement](P1-008-event-job-contract-amendment.md)
**Child documents:** SRS-008 (software requirements), DDR-008 (design decisions), HLD-008 (high-level design), LLD-008 (low-level design)

---

## Summary

- **Purpose**: Defines the top-level requirements for adding Event-Driven (L2, Sporadic) workload support to TIMPANI. First, it defines the **common Event/Job model** (§2), which is independent of OS and middleware. The model includes Event identification `(gen, seq)`, **one Event = one Job**, a lifecycle and results made up of accept, acquire, completion, discard, cancel, and abandon, the absolute deadline `t_arr + D`, rules R1~R4, and invariant I1. Middleware-specific identifiers and semantics, such as those of SOME/IP, are not placed in the TIMPANI core (transport-layer neutral).
- **Scope**: Event/Job conceptual model, detection (DET), CBS isolated execution (CBS), protection and faults (PRT), ordering (ORD), quantitative timing (TIM), interfaces (IF), runtime table update (RTU), observability (OBS), non-functional requirements (NFR), and acceptance criteria AC-008-01~19.
- **Phases**: Phase 1 focuses on explicit Event connection, CBS execution (including CPUs without TT), Event-based deadline monitoring (DeadlineMonitor), minimal fault handling, division of responsibility between CBS budget management and Event/Job management, checking the cpuset allowance of assigned CPUs, and basic observability and completion of the FCW sample app. Phase 2 covers advanced features such as the precise admission formula, EDF, multi-poster posting, and large payloads. Each requirement is marked with P1, P1*, or P2.
- **Structure**: Divided into three layers: model (①) / connection and observation (②: explicit Event connection is the only path that provides Events, Jobs, and deadlines, while kernel execution observation provides the execution intervals and budgets of all L2 tasks and is not a means of obtaining Events) / enforcement (③: BPF's CBS, CPU, and TT protection; TIMPANI-N's deadline monitoring and fault reporting; TIMPANI-O's admission).
- **Key change (v5)**: The deadline is an absolute deadline that starts at the Event arrival time, and one Event corresponds to one Job (Jobs are neither merged nor split). A deadline miss is detected without waiting for completion. L2 tasks without an explicit connection are classified as the budget-only class. A-04 is withdrawn, and A-06 is released from the correctness premise (it remains as a condition of the admission result). In v5.2, the responsibilities of CBS budget management and Event/Job management of the same L2 worker are separated, and applications are not reclassified.
- **Status**: Draft v5.3. Latency upper bounds, queue constants, and API/SHM/proto proposals are candidate values that require agreement, measurement, or Project Leader approval (§11 OI-18~OI-19). TIM candidate values are finalized after measurement on the target HW (OI-5). The child documents SRS/DDR/HLD/LLD-008 are being revised based on this document.

---

> **Document scope and writing principles**
> This document is the **top-level requirements specification (REQ)** for TIMPANI's `008: Event-Driven Workload Support`. It defines "what" and "why" must be guaranteed. "How" is covered by DDR-008, HLD-008, and LLD-008.
>
> **Three design principles**
> 0. **Common model first (Model-First)**: Event and Job are first defined as a **common model** (§2), independent of OS, scheduler, and middleware, and TIMPANI is designed and implemented to correspond to that model. Implementation means (kernel wake-up, futex, sched_ext callbacks, library API) are only means of providing, observing, and enforcing the model's events, and they do not define the model.
> 1. **Transport-neutral (Transport-Neutral)**: The TIMPANI core (TIMPANI-O/N, BPF, application library contract) does not include identifiers or semantics of **specific middleware or transport layers, such as SOME/IP, DDS, or CAN.** What TIMPANI knows is the Event source of the common model, the Event information that the application provided through the explicit connection path (arrival time, identifier, payload), and the execution intervals observed by the kernel only. Middleware-specific processing (service/instance/event IDs, subscription, deserialization) is the responsibility of the application or the application-side adapter.
> 2. **Phased approach (Phased)**: **Phase 1** focuses on realistic and simple basic functions (explicit Event connection, CBS isolated execution, Event-based deadline monitoring, minimal fault handling, basic observability) to drive together the `timpani26` implementation and the completion of the FCW sample app. **Phase 2** covers advanced functions after Phase 1 verification (EDF ordering, window-based admission, incremental update, middleware-boundary observation, watchdog, etc.). Requirement IDs remain stable regardless of phase, and the scope is indicated by the Phase column.
>
> **Keyword convention**: **SHALL** = mandatory, **SHOULD** = recommended, **MAY** = optional.
> **Unit convention**: All time values state their unit (µs/ms/ns). Quantitative requirements state the percentile and the observation interval together. Values that are not agreed or not measured are marked `(candidate)`. Before they are finalized, they are not used as pass/fail criteria but are referred to as the "agreed upper bound."

---

## 0. Change History

| Version | Date | Changes |
|:--|:--|:--|
| v1 | 2026-10-25 | Baseline draft. Nine requirements in MOD/DET/CBS/PRT, UC-01/02, AC-01~04 |
| v2 | 2026-11-09 | (a) Stated the transport-neutral principle and the Phase 1/2 split principle. (b) Reflected the gap analysis of the `timpani26` HEAD `4201573` codebase (§1.3, §10). (c) Added new requirement groups ORD/IF/TIM/RTU/OBS/NFR, keeping existing IDs. (d) Introduced quantitative timing requirements (TIM) and measurement methods. (e) Added numeric values and measurement methods to the acceptance criteria. (f) Stated out-of-scope items, assumptions, and open items. (g) Added UC-03~06. (h) Relationship with the preceding sample apps (PR #50~#52) and guide direction (§12) |
| v3 | 2026-11-10 | (a) Rewrote §2 as the **common Event/Job model**: Event source, Event, and Job entities, four lifecycle events, rules R1~R4, invariant I1, and the three-layer structure. (b) Separated Event (arrival) and Job (processing). Added MOD-007/008. (c) Removed sample-app-specific figures and options from UC/AC and described them based on the generic event generator. (d) Organized the marker interface (§12.2) |
| v4 | 2026-11-10 | Reflected independent review. (a) Redefined R2/R4 and the boundary rule for simultaneous occurrence. (b) Stated the suitability conditions for adapters. (c) Replaced the replenishment rule with the replenishment window based on activation (PRT-001, OI-4). (d) Stated that the admission formula must hold for all L2 tasks on the CPU (CBS-004, AC-008-08). (e) Defined the accuracy requirement for MIT violation counting (PRT-004) and the DMISS flag rule (PRT-007). (f) Corrected the detection criterion of TIM-005 and supplemented the TIM percentiles. (g) `task_id` 15-byte and node-unique constraint (A-03). (h) Corrected AC-008-01/04/08/12. (i) Stated the guarantee scope of a violating server, the budget carry-over flag, the `libttsched` rebuild exception (NFR-003), and the TIM statistics basis (§4.6). (j) Stated the two layers of the §12.2 guide |
| v5 | 2026-11-11 | Aligned with the P1 Event–Job contract amendment agreement (`P1-008-event-job-contract-amendment`, Issue #53). (a) Rewrote §2: one Event = one Job, Event identification `(gen, seq)` and absolute deadline `t_arr + D`, lifecycle and results (COMPLETED/DISCARDED/CANCELLED/ABANDONED), deadline-miss determination rule and completion-time race rule, redefined R1~R4 and I1, defined MIT as a constraint on arrivals, connection and observation layers (explicit Event connection / kernel execution observation), and the budget-only L2 class. Retired the activation-based deadline, batch consumption (n items), the A-04 approximation, and the Job boundary based on kernel wake-up. (b) Requirements: MOD-002/003/006/007/008, DET-001~005, PRT-002/004/005/007, CBS-004, ORD-001/002, TIM-001/002/005/008, IF-003/005, OBS-001/002, and NFR-002 revised. MOD-009~011, DET-007, PRT-008/009, IF-007, TIM-009~011, and OBS-004 added. (c) Acceptance criteria: AC-008-01/04/05/07/08/12 revised, AC-008-13~16 added. (d) Assumptions: A-04 withdrawn, A-06 correctness premise released, A-08 added (A-07 is an SRS-008-only number). Constraints: C-08/C-09 added (C-05~C-07 are SRS-008-only numbers). (e) Updated the §9 roadmap; organized §11 OI-1 and OI-17 and added OI-18~OI-22; updated the explicit Event connection row of the §12 guide table. (f) Review reflected: §6.2 aligned with the same mapping as SRS-008 §8.1, completion-time definition and close interpretation marked, OI-1 resolved, queue depth meaning and the poster interruption limit stated, and completion-time meaning and out-of-scope items added to §7 |
| v5.1 | 2026-11-16 | Reflected review of the 008 design chain and aligned D-22~D-24. (a) Requirements: added CBS-007 (CBS execution on CPUs without TT), CBS-008 (checking the allowed set of assigned CPUs and explicit failure), A-09 (cgroup and cpuset placement responsibility), AC-008-17~19, and OI-23~25. Added a reference to A-09 in A-02. (b) Corrected the three steps of UC-06 in §3 and the `MIT_RELATED` wording in PRT-007 to a temporal proximity flag, not a cause determination. (c) Organized the P1 and P2 scope in §0.1. Made concrete the CBS-only condition of the preceding bug (item 16 in §10) based on the current code. (d) Added traceability rows for §6.1 and §6.2 and a guide row in §12.2. Linked with SRS-008 v4.1 |
| v5.2 | 2026-11-18 | Revised on 2026-11-17 (3e307da): unified the terms (approval → admission, approval formula → admission formula, and the two Korean words for arrival unified, with the English term "arrival" unchanged). Replaced the budget-only L2 class with the division of responsibility between CBS budget management and Event/Job management, and keeps no class or flag (D-21). Job start confirmation happens inside wait, and there is no separate public acquire API. Reflected the P1 policy boundary and the review of worker-only isolation (D-24). Added a comparison with the single-timer alternative (F-10, OI-22). This scope decision is the decision of Project Leader (author). Review corrections of 2026-11-18: fixed the QUEUED return of DET-004(b) as mandatory, reverted AC-008-01(b) to CBS budget enforcement, changed A-09 to a premise sentence, and added to the term definition row a sentence that the worker's "acquire" refers to the transition. Also replaced the terms in the earlier history rows with the new terms. The $L_{post}$ upper bound (TIM-011) is kept as a requirement that TIMPANI ensures in the reference poster configuration, and the assumption that the deployment satisfies that configuration is added as A-10 (Project Leader (author) decision). The starting point of $L_{post}$ is unified to $t_{arr}$. Aligned with DDR-008 v3.3, HLD-008 v5.3, LLD-008 v5.2, and SRS-008 v4.2 |
| v5.3 | 2026-11-26 | Term clean-up (Project Leader (author) decision): the side that calls post is the poster, and the side that creates the input to be posted to an Event source is the producer. "Producer" where it meant the thread that calls post was changed to "poster" (reference producer configuration → reference poster configuration, multi-producer posting → multi-poster posting). Definition rows for poster and producer were added to the §2 term table. Final review corrections: AC-008-15 (b) was corrected to state the counters that each kind of rejection is reflected in, and (c) now reflects the P1 limits of IF-007. Other requirement IDs, numbers, and requirement content are unchanged. The terms in the earlier history rows were also changed to the new terms. Aligned with SRS-008 v4.3, DDR-008 v3.4, HLD-008 v5.4, and LLD-008 v5.3 |

### 0.1 Phase Definitions

| Phase | Goal | Completion criteria |
|:--|:--|:--|
| **P1** | Event-Driven basic functions: explicit Event connection (acceptance, Job acquisition, explicit done), CBS isolated execution based on kernel execution observation, CBS execution on CPUs without TT (CBS-007), fixed assigned CPUs (allowed CPU set check, CBS-008), bounded replenishment, Event-based deadline monitoring (DeadlineMonitor), minimal fault classification (DMISS/BUDGET_EXCEED/MIT_VIOLATION), division of responsibility between CBS budget management and Event/Job management, basic observability and tracing, and the common Event/Job model (§2). The FCW sample app is completed with the TIMPANI contract alone. Time isolation that includes execution beyond TT is not a P1 guarantee (DDR-008 D-09 conditional guarantee, D-23) | All acceptance criteria marked P1 in §5 pass. The P1 items in this document are design goals and do not mean that implementation or verification is complete |
| **P2** | Advanced features: EDF server ordering, window-demand-based admission, task-level time class, incremental table update (no disturbance to TT), NodeStatus statistics, watchdog, middleware-boundary observation, wake-up latency fault, HRTICK-based budget precision, multi-poster posting and large payloads, enforced termination of TT slots and the policy for incomplete TT (DDR-007 §4.3.1), cause analysis of deadline misses and cause-based recovery (DDR-008 D-23, DDR-012 §4.4) | Revised separately based on P1 operational data |

---

## 1. Overview & Problem Statement

### 1.1 Background
Workloads in the SDV (Software-Defined Vehicle) environment include not only control tasks that run at a fixed period (L1 Periodic), but also **safety-critical (Safety-Critical) workloads (L2) that are activated aperiodically (Sporadic) by the arrival of external events**, such as radar and lidar object detection, emergency braking triggers, and middleware message reception. Because "when they run" is determined by the external world, TIMPANI cannot fix execution times in advance. It must therefore **(a) accurately record the arrival time of each Event, (b) bound the response time after arrival, and (c) isolate them so that a budget overrun does not propagate to L1 or to other L2 workloads**.

### 1.2 General Requirement Characteristics of Event-Driven Workloads (Design-Neutral Summary)
Regardless of the TIMPANI implementation, for a real-time scheduler to support safety-critical Event-Driven workloads, the following are required. The requirements of this document are the projection of this list onto the TIMPANI architecture (HSF, sched_ext, TT+CBS).

| # | Characteristic | Description | Phase |
|:--|:--|:--|:-:|
| G1 | **Sporadic task model** | Declares MIT (minimum inter-arrival time), WCET, and relative deadline (D). D must be based on the **Event arrival time** (not on the dispatch time) | P1 |
| G2 | **Arrival recording** | The explicit connection path records when the Event arrived, at the time of acceptance. Arrival time cannot be known from kernel observation alone, and the later the observation point, the more optimistically the response time measurement is distorted | P1 (explicit connection) / P2 (middleware boundary) |
| G3 | **Job boundary definition** | The start and end of one Event's processing must be clear for response time and deadline determination to hold. Internal blocking during processing (mutex/I/O) is not a new Job, and kernel sleep must not be used as a Job boundary | P1 (explicit acquire and done by the worker; tasks without a connection have no Job boundary; OI-1 resolved) |
| G4 | **Bandwidth isolation** | Caps CPU usage with a budget (C_s) / period (T_s) server. Event overload (MIT violations) and execution-time overruns must not propagate to other workloads (FFI) | P1 |
| G5 | **Admission** | Utilization checks alone are insufficient to guarantee the deadline when D < MIT. A response time bound is required that reflects the maximum blocking by the upper layer (L1 TT slots) and interference from other servers on the same CPU | P1 (simple formula, Jobs without blocking) / P2 (window demand) |
| G6 | **Wake-up latency bound** | Bounds the delay from arrival to start of execution by percentile. Distinguishes arrival time → runnable (t_arr→runnable) from runnable → first execution. Distinguishes the case where the CPU is idle or in a Gap from the case where the upper layer is occupying it | P1 |
| G7 | **Replenishment determinism** | A throttled server must return to the runnable state reliably at its replenishment time. Delayed replenishment that relies on other activity has no bound | P1 |
| G8 | **MIT violation handling** | Counts arrivals earlier than the MIT, and separates isolation maintenance (absorption within the budget / delay / discard) from reporting | P1 |
| G9 | **Ordering of multiple servers on the same CPU** | The priority order (FIFO/EDF) of multiple Sporadic servers that share one CPU must be included in the deadline guarantee formula | P1 (FIFO) / P2 (EDF) |
| G10 | **Event silence monitoring** | A monitoring (watchdog) means for detecting when an expected arrival does not occur (sensor dropout) | P2 |
| G11 | **Runtime addition/removal** | Adding or removing Sporadic workloads must not disturb the existing L1/L2 timing | P1 (addition/removal possible) / P2 (no-disturbance guarantee) |
| G12 | **Observability** | Arrival, acquire, and completion records, execution intervals, budget usage, and violation counters must be collectable by the verification framework | P1 |
| G13 | **Application identification / resilience** | Policies must be reapplied even if the event-processing thread is created late or restarted | P1 (rescan) / P2 (cgroup metadata) |

### 1.3 Current Limitations and Issues to Resolve
The existing design ([DDR-001](DDR-001-workload-model.md), [DDR-005](DDR-005-sched-ext-bpf-scheduler.md), [DDR-007](DDR-007-tt-cbs-integrated-scheduling.md)) and the `timpani26` codebase **already have end-to-end** the mechanism that allocates and charges CBS budgets to L2 workloads (TemporalClass → Mechanism::CBS → `CbsConfig` → BPF `cbs_map` → budget charge/throttling). However, the following are either not defined for the **basic operating model of Event-Driven workloads**, or the implementation does not meet the determinism requirements (basis: gap summary table in §10).

1. **Absence of definitions of Event and Job**: From the TIMPANI perspective, there is no criterion for which points are taken as the start and end of Event acceptance and Event processing (Job). The current BPF overwrites `activation_ns` on **every** wake-up (`timpani.bpf.c:256`), so a wake-up after mutex/I/O blocking during processing is mistaken for a new activation, which contaminates deadline determination and arrival counting.
2. **Mismatch between the deadline reference and the determination time**: The reference of the deadline must be the Event arrival time, but the current code is tied to the kernel wake-up time, and the L2 deadline determination is performed at every stopping, not at the completion time.
3. **Connection between Event detection and the CBS runtime**: An end-to-end flow exists in which an activated task runs within its CBS budget without interfering with L1 (TT) and waits again after completion, but (a) replenishment depends only on entry into the BPF ops path, so **there is no upper bound on replenishment latency**, (b) because `DSQ_CBS` is a global queue, L2 tasks **can run outside their assigned CPU**, which invalidates the per-CPU Gap analysis (DDR-007), and (c) the admission check looks only at utilization and does not reflect **TT blocking, D<MIT, same-CPU server interference, or interference from tick overrun**.
4. **Absence of quantitative bounds**: Numeric requirements for wake-up latency, replenishment latency, budget overrun allowance, and deadline-miss detection and reporting latency do not exist, so the verification framework cannot make a pass/fail determination.
5. **Interface mismatch**: The Pullpiri→O proto has no MIT-specific field and reuses `period`. For O→N, `ExecutionModel.EVENT_DRIVEN` is only declared and is always sent as `TIME_TRIGGERED`. BUDGET_EXCEED/WATCHDOG faults are lost as UNKNOWN at the O→Pullpiri boundary.
6. **Transport-specific PoC**: `examples/someip/libtimpani_rt` includes the SOME/IP service/instance/event IDs in the API contract (`timpani_evt_register(wl, task, svc, inst, evt, mit)`) and records MIT and E2E faults in its own SHM (`/timpani_rt`), but TIMPANI-N does not consume them. The server key hash also does not match BPF. This violates principle 1 and is a target for redesign (§12).

---

## 2. Conceptual Model: Common Event/Job Model

> This section defines a **common model that is independent of any specific OS, scheduler, middleware, or library**. The requirements in §4, the child documents (SRS/DDR/HLD/LLD-008), the application contract, and the verification framework are all described in the terms and rules of this model. Implementation means (kernel wake-up, futex, sched_ext callbacks, connection API) belong to the **connection and observation layer** of §2.7 and do not change the model.

**Canonical source of common terms:** OVERVIEW and the detailed documents REQ/SRS/DDR/HLD/LLD/P1-008 use the terms below with the same meaning. Other documents refer to this definition and do not create separate translated names or application classifications.

| Term | Definition |
|:--|:--|
| **CBS budget management** | Responsibility of the same L2 worker for CPU budget enforcement per server window, budget replenishment, and throttling. CPU pinning and BPF execution-interval observation are common mechanisms related to it. Starting a Job alone does not grant a new budget |
| **Event/Job management** | Responsibility of the same L2 worker for Event acceptance and rejection, identification and queue management, the explicit Job lifecycle, arrival-based MIT counting, and deadline monitoring. The explicit Event connection is the means of providing the information this responsibility requires |
| **Arrival** | Occurrence recognized by the input adapter at the boundary it declares. Arrival time and the MIT counting target follow §2.5~§2.6. Only accepted arrivals become Events |
| **Accept/reject** | Result of either accepting a post request as an Event or returning it as an error. Distinguished from the admission of workload placement |
| **Post (post) / Event wait (wait) / done (done)** | The adapter posts an Event. The worker waits until there is an Event to process. Within the same wait call, it confirms and records the start of the Job for one Event, takes over the Event, and returns. If a QUEUED Event exists, wait returns without blocking. done is the worker's explicit Job completion or discard. There is no separate public acquire API. Closing by TIMPANI follows §2.3 |
| **Poster** | The thread or process that calls post for an Event source. The post call is in the code of the input adapter, and the poster is the party that executes that call. Which threads and processes can be a poster, and the constraint that posts for the same source run in only one posting context at a time, follow C-08. The assumption on the configuration of the thread that calls post follows A-10 |
| **Producer** | The party that creates the input to be posted to an Event source (§2.2). The Event source is the declared origin and is a different concept from the producer. The producer can be the same as the poster or different from it |
| **Job-start state transition (`QUEUED → RUNNING`) / confirmation of processing start** | A logical transition that confirms the start of processing of the Event inside wait. The CAS (compare-and-swap) in the SHM implementation is an atomic conditional update that replaces the state word with a new value only when the value read earlier equals the current value. It is not an additional queue, public API, or scheduler operation, and it is also distinguished from data copying. In this document, the worker's "acquire" refers to this transition and is not a separate public API |
| **Budget replenishment (replenishment)** | Operation that fills the server budget according to the existing window rules. It is distinguished from Event acceptance or Job start itself |
| **Budget exhaustion** | The state in which the remaining budget is 0 after a charge. By itself, it is not throttling (PRT-001) |
| **Throttling** | BPF blocks further execution of a runnable task whose remaining budget is 0. A task that goes to sleep after exhausting its budget is not throttled |
| **Promotion** | Operation that moves a throttled server to the run queue after budget replenishment |
| **Admission** | Preliminary schedulability determination that applies the same admission formula and the existing premises to all L2 tasks on the target CPU of placement (CBS-004) |
| **Execution interval** | Unit observed by the kernel from the runnable transition to the next voluntary sleep. It is not an Event arrival or an explicit Job boundary |

**CBS budget management and Event/Job management are complementary responsibilities of the same L2 worker, not two applications, classes, modes, or two parts of an application.** L2 follows the `Sporadic + SafetyCritical` classification of Accepted DDR-001 as it is. L2 is not reclassified by whether an explicit Event connection is used. Even without the connection, CBS budget management, CPU pinning, and execution-interval observation of the same L2 worker are maintained. However, it does not obtain the records needed for Event arrival time, Job records, MIT counting, and Event-based deadline monitoring, and it does not estimate them from BPF execution intervals. The scope of the optional connection and of existing compatibility does not change (DET-003, IF-001, NFR-003/005).

### 2.1 Three-Layer Structure

| Layer | Contents | Condition for change |
|:--|:--|:--|
| **① Model** | Event source / Event / Job, lifecycle and results, deadline rules, R1~R4, I1, timing constraints (MIT, D) | Does not change (this section) |
| **② Connection and observation** | **Explicit Event connection**: Event acceptance, Job acquisition and explicit done, result recording. It is the only path that provides Events, Jobs, and deadlines. **Kernel execution observation**: execution intervals and CBS budget of all L2 tasks. It is not a means of obtaining Events | When adding or replacing a connection method |
| **③ Enforcement** | BPF: CBS budget enforcement, CPU pinning, TT protection, execution-interval observation. TIMPANI-N: Event-based deadline monitoring (DeadlineMonitor), fault reporting, statistics. TIMPANI-O: admission, placement | When the policy changes (DDR-008) |

TIMPANI's function is defined as applying ③ to the ① events provided by ②. Explicit Event connection provides the events needed for Event/Job management and the Event-based deadline. Even without the connection, CBS budget management, CPU pinning, and execution-interval observation of the same L2 worker are maintained (MOD-011). New event delivery means (DDS, CAN, pipeline predecessor tasks, etc.) are accepted as additions of a connection method for ②, and ① and ③ remain as they are.

### 2.2 Entities

| Entity | Definition | Identification and attributes |
|:--|:--|:--|
| **Event source** | A declared origin that supplies Events to an L2 task. TIMPANI does not know the kind, content, or transport layer of the origin. Only a source that posts Events through an explicit connection has Events | Identifier `(workload_id, task_id)`, one consuming worker, MIT (minimum interval between consecutive arrivals). P1: one per L2 task, implicit declaration. P2: multiple per task, explicit declaration (OI-16) |
| **Event e** | One arrival accepted by the Event source | Identifier `(gen, seq)`: `seq` is the posting sequence number within the source, and `gen` is the registration generation (incremented each time the source is activated again). Attributes: arrival time `t_arr` (monotonic clock), absolute deadline `t_dl = t_arr + D` (computed once at acceptance, immutable), payload (bytes that TIMPANI does not interpret) |
| **Job j(e)** | Processing of one Event e. Multiple Events are not merged or split | Start `t_begin` (the time the worker acquired e), end (the worker's explicit done `t_end`, or the time TIMPANI closed the Job), result, flag |
| **L2 task** | Scheduling entity that belongs to the `Sporadic + SafetyCritical` classification of DDR-001 and has a worker thread. The division of responsibility between CBS budget management and Event/Job management does not change this classification (MOD-011) | `(workload_id, task_id)`, WCET C, relative deadline D, MIT |
| **Execution interval** | Unit of worker execution observed by the kernel: runnable transition (activation) → first execution → voluntary sleep | `t_act`, `t_start`, `t_sleep` (BPF observation). **Not a Job boundary** |

Event (arrival) and Job (processing) are different concepts. MIT applies to Event arrivals, and C and the deadline apply to Jobs. An execution interval is a unit of kernel observation and does not have to correspond 1:1 to a Job. One Job may end within one execution interval, or it may span several execution intervals because of blocking or preemption.

### 2.3 Event Lifecycle and Results

```text
  Post (post) ──► Accept/reject: E_INACTIVE, E_FULL, E_INVAL → not an Event (E_FULL is counted as overflow)
     │ accept: (gen, seq), t_arr, t_dl = t_arr + D fixed, monitoring starts
     ▼
  QUEUED ── worker acquires (t_begin) ──► RUNNING ── worker completes (t_end) ──► COMPLETED   (successful end)
     │                                  │
     │                                  ├── worker policy discard ──────────► DISCARDED   (not a success)
     │                                  └── TIMPANI close (worker exit, re-registration) ─► ABANDONED
     └── TIMPANI close, before Job start (inactive, re-registration, daemon cleanup) ─────► CANCELLED
```

- **QUEUED**: Accepted, and the start of the Job for this Event is not yet confirmed. The next Event may be QUEUED while the worker is processing the previous Job on the CPU.
- **RUNNING**: The start of the Job for this Event is confirmed inside wait. It does not mean that the Event is executing on the CPU now. It is maintained until the transition to a terminal state, including during preemption, throttling, or blocking.
- **COMPLETED**: The worker explicitly stated that processing is finished. This is the only successful end state.
- **DISCARDED**: The worker gave up the Event as a matter of policy. It is not a processing failure, but it is not a success either.
- **CANCELLED**: TIMPANI closed the Job before its start (source deactivation, re-registration, daemon cleanup).
- **ABANDONED**: TIMPANI closed the Job during execution (worker exit, re-registration of the same source).
- Terminal states (COMPLETED, DISCARDED, CANCELLED, ABANDONED) are final and cannot be reverted.
- **Rejection**: If a post is rejected, no Event is created. The codes are `E_INACTIVE` (source inactive, worker not bound, removal, daemon stopped), `E_FULL` (queue full, counted as overflow), and `E_INVAL` (argument error).
- **REVOKED**: An internal state in which the post was rolled back because the source was deactivated or re-registered during the post. It is not an Event and is not counted in the accepted count.

These states are the logical lifecycle of the common Event/Job model and are represented in the SHM Event record. They are separate from BPF DSQ membership and from whether the worker is currently executing on a CPU. The handover of the Event when wait returns is the result of the same call, not a separate public API step.

**Job times and results**

- Start: the time `t_begin` at which the worker acquired the Event. End: the time `t_end` of the worker's explicit completion or discard, or the time at which TIMPANI closed the Job.
- A Job closed by TIMPANI has the closing time instead of `t_end`. If it was closed before the Job started, it has no `t_begin`.
- Response time `R = t_end − t_arr`. Meeting the Event-based deadline means **COMPLETED and `t_end ≤ t_dl`**.
- The times `t_act`, `t_start`, and `t_sleep` of an execution interval are observed by the kernel and are not Job boundaries (§2.7).

### 2.4 Job Rules and Invariants

- **Job j(e)**: The processing of one accepted Event e. Multiple Events are not merged (MOD-009). It starts when the worker acquires the Event, and it ends with the worker's explicit done or with TIMPANI's close (DET-007).
- **R1 (arrival while waiting)**: If an Event is accepted while the worker is waiting to acquire, the Job of that Event becomes immediately acquirable.
- **R2 (arrival during processing)**: While a Job is executing, an accepted Event remains `QUEUED`. It is not added to the running Job, and it is not lost.
- **R3 (consecutive Jobs)**: When a Job ends and a `QUEUED` Event exists, the worker does not sleep and starts the Job of the oldest Event. Consecutive Jobs are different Jobs.
- **R4 (consumption)**: One Job consumes exactly one Event. Events are acquired in FIFO order within the source.
- **Invariant I1**: Every accepted Event reaches exactly one terminal state, and Jobs and accepted Events correspond 1:1.
- **Boundary for simultaneous occurrence**: The order of two occurrences (such as arrival and completion) is determined by the order in which the connection path posted them.
- Under normal conditions (MIT compliance, and every Job meets its deadline under `C ≤ D ≤ MIT`), R2 and R3 do not occur, and Jobs and Events are 1:1. The occurrence of R2 or R3 itself is an observation indicator that means either an MIT violation or a deadline overrun of a preceding Job.

### 2.5 Deadline Rules: Result and Completion Time

**Absolute deadline**: The deadline of an Event is `t_dl = t_arr + D`. It is set once at acceptance and is not moved later by activation, replenishment, resumption, or worker delay. **Activation time + D is not used as a deadline.** The determination uses `t > t_dl` (strict overrun) on the monitoring clock.

**Deadline miss (MISSED)**: A Job is a deadline miss if one of the following holds.
- At time `t > t_dl`, the Job is still open (QUEUED or RUNNING).
- The Job ended after `t_dl`. Completion, discard, cancel, and abandon all apply.

**Rules**

- MISSED is a flag set once per Job and is not cleared. A later end does not erase a failure that has already been set.
- A Job that ends at or before `t_dl` is not a failure, and deadline monitoring ends at that point. Not only normal completion but also a Job closed by TIMPANI (CANCELLED, ABANDONED) is not a failure if it is before the deadline. This close is counted, with its reason, as a separate result that is not a failure (it is not a success). This interpretation is subject to confirmation (OI-21).
- Detection of a deadline miss does not wait for completion. It is detected in all states: waiting, executing, preempted, throttled, and CPU idle (PRT-009).
- An item that the worker explicitly discards (DISCARDED) before the deadline is not a successful completion, but it is not a failure either. P1 has no cancel call that the worker uses. Discard, cancel, or abandon after the deadline is a failure.
- A timely completion is not determined to be a failure merely because collection was late. The collecting side makes the determination using the recorded completion time.

**Completion time `t_end` and the race rule**

- `t_end` is the time that the worker reads immediately before it posts completion.
- A Job is completed on time when `t_end ≤ t_dl` and the monitoring side has not already marked it as a failure.
- If the monitoring side's marking falls between the worker's time read and the completion post, the completion post fails, and the worker reads the time again and retries. The time read again is then after `t_dl`, so the recorded `t_end` and the MISSED flag do not conflict with each other.
- Tolerated error: a Job recorded as on time may be posted just after `t_dl` (when the worker is preempted between the time read and the post). However, it is not delayed beyond the first monitoring period. This interpretation is subject to confirmation (OI-20).

**P1 limitation**: While the daemon is down, no entity monitors expired Events. A failure found during cleanup after restart is a post-hoc record, not a real-time report (PRT-009).

### 2.6 Minimum Inter-Arrival Time (MIT) Constraint

- The MIT constraint applies to **arrivals**. The interval between two consecutive arrivals of the same source must be `min_inter_arrival_us` or more.
- Every attempt to count an arrival is counted as an arrival. Acceptance, rejection due to a full queue, and rollback to REVOKED are included. A post rejected at the first check because the source is inactive is not an arrival and changes nothing.
- A violating arrival is also **accepted and placed in the queue**. The violation is counted and reported (PRT-004). It is not lost.
- There is no approximate counting based on activation intervals. An activation interval is not an arrival interval, so it is not used for MIT determination.
- If violations continue, the queue builds up, and subsequent Jobs may miss their deadlines. This is reported and not prevented.

### 2.7 Connection and Observation Layer

| Path | Provides | Does not provide | Phase |
|:--|:--|:--|:-:|
| **Explicit Event connection** (called by the application) | Event acceptance (arrival time, identifier, payload retention), Job acquisition and explicit done (result), Event-based deadline and monitoring | Events that the application did not connect. Arrivals that do not pass through the connection path | P1 |
| **Kernel execution observation** (all L2, no application change) | Execution intervals (runnable transition, first execution, voluntary sleep), CBS budget use and throttling, execution CPU | Event arrival, Job boundaries, Event-based deadline. It cannot tell whether the cause of a kernel wake-up is an Event, so it is not a means of obtaining Events | P1 |
| **Middleware-boundary observation** | Arrivals at the middleware reception point | Implementation required per middleware | P2 |
| **Completion of a preceding task** | In a pipeline, completion of a preceding Job becomes the arrival of a subsequent source | REQ-DD-001 | P2+ |

- The explicit Event connection is the **only** path for Event-based deadlines. The application posts the arrival time and payload of an Event to the connection path, and the worker acquires the Events one at a time, starts each Job, and explicitly ends it. Acceptance, rejection, and overflow are determined by the connection path (IF-007), and the application does not keep a separate payload queue.
- Kernel execution observation is not a means of obtaining Events. Execution intervals are used for CBS budget enforcement and execution records. A registered L2 task is not described as "equivalent to Event-based deadline support" on the basis of kernel observation alone.
- The two paths are not described as "two methods that produce the same result."
- The API proposal for the connection path (libttsched names `ttsched_evt_open`, `ttsched_evt_post`, `ttsched_evt_wait`, `ttsched_evt_done`) is a proposal under IF-005 and IF-007 and requires Project Leader approval.
- On P1, the connection path has the following constraints (IF-005, IF-007, C-08, C-09). There is one worker thread per source. An Event is posted from only one context at a time per source. The payload is carried only inline. A process that can write to the shared SHM is a trusted party.

### 2.8 Relationship Between CBS Budget Management and Event/Job Management

The two responsibilities follow the common definitions in §2. The explicit Event connection is a means of Event/Job management and is not a subtype of application (MOD-011).

- **Maintained without the connection**: CBS budget management, CPU pinning, BUDGET_EXCEED, BPF execution-interval recording.
- **Information and monitoring not obtainable without the connection**: Event identification and arrival time, Job records, MIT counting and MIT_VIOLATION, Event-based DMISS. Events or Job deadlines are not estimated from BPF execution intervals.
- **No additional flag**: P1 does not provide additional classification or configuration flags for this division of responsibility. If a source has no Event record, there is no object for DeadlineMonitor to monitor, and a temporary silence in posting does not reclassify the application.
- **Same admission**: TIMPANI-O uses the same admission formula for all L2 tasks (CBS-004). Even when there is no Event record, the declared D remains an admission parameter based on kernel activation, and it is not a monitored Job deadline.

### 2.9 Key Timing Parameters

**Declared parameters** (Pullpiri → TIMPANI-O)

| Parameter | Symbol | Meaning | Constraint |
|:--|:--|:--|:--|
| `min_inter_arrival_us` | MIT | Minimum interval between two consecutive **arrivals** of an Event source. The basis for the upper bound of the replenishment period | > 0 |
| `wcet_us` | C | Worst-case execution time of one Job under normal conditions. The basis for the maximum CBS budget $C_s$ | 0 < C ≤ D |
| `deadline_us` | D | Relative deadline from the Event arrival (`t_arr`) to completion. Without an Event record, it is an admission parameter and not a monitored Job deadline | C ≤ D ≤ MIT |
| `max_dmiss` | — | Cumulative number of deadline misses allowed before reporting (DDR-012 `current_limit`) | ≥ 0 |

**Derived parameters** (computed by TIMPANI-O)

| Parameter | Computation | Notes |
|:--|:--|:--|
| $C_s$ | = C (WCET) | Server budget. Granted for each replenishment window |
| $T_s$ | Declared CBS period (`period`), with constraint $T_s \le \text{MIT}$ | Length of the replenishment window (DDR-008 D-04). The window starts at the kernel activation time. $T_s > \text{MIT}$ is not accepted |
| $U_s$ | = $C_s / T_s$ | Server utilization |
| $B_{tt}(k, L)$ | Maximum TT occupation time within an arbitrary window of length L on CPU k. In the admission formula, L = D_i. If the window covers several TT blocks, their sum | Blocking term of the admission (CBS-004) |

**Measurement vocabulary** (common to the verification framework)

| Symbol | Definition | Recorded by |
|:--|:--|:--|
| `(gen, seq)` | Event identifier | Connection path |
| $t_{arr}$ | Event arrival time (recorded at acceptance) | Connection path |
| $t_{dl}$ | Absolute deadline = $t_{arr}$ + D | Connection path (fixed at acceptance) |
| $t_{begin}$ | Job start (worker acquire) | Worker |
| $t_{end}$ | Job end (worker's completion or discard time) | Worker |
| $t_{act}$, $t_{start}$, $t_{sleep}$ | Runnable transition, first execution, and voluntary sleep of an execution interval | Kernel observation (BPF) |
| $R$ | Response time = $t_{end} - t_{arr}$ | Derived |
| $L_{wake}$ | Runnable → first execution = $t_{start} - t_{act}$ | Kernel observation |
| $L_{post}$ | Arrival time $t_{arr}$ → runnable | Connection path + kernel observation |
| Detection latency | $t_{detect} - t_{dl}$ (based on the time MISSED was set) | DeadlineMonitor |

---

## 3. Core Use Cases

### UC-01: Normal Event Acceptance and CBS-Based Execution (P1)
1. The worker of the L2 task waits for an Event on the explicit connection path. The worker may be asleep at the kernel level.
2. The input adapter posts an Event together with its arrival time. When the connection path accepts it, `(gen, seq)` and `t_dl = t_arr + D` are fixed, and the Event becomes a monitoring target (R1).
3. The worker acquires one Event and starts its Job (`t_begin`). Because the budget is granted in units of the replenishment window, the start of the Job does not grant a new budget.
4. The TIMPANI-N BPF scheduler checks the remaining CBS budget of the task (`remaining_us > 0`, after applying the activation-based replenishment rule at wake-up) and places the task in the L2 queue of its assigned CPU. The task runs when no L1 TT slot is running on the assigned CPU (Gap interval), and the budget is decremented by the CPU time used.
5. When the worker finishes processing and explicitly posts completion, the Job ends as COMPLETED. If `t_end ≤ t_dl`, the deadline is met.
6. When the worker goes to wait for the next Event, the execution interval ends in the kernel. If an Event is in the queue, the worker does not sleep and starts the next Job (R3).

### UC-02: Limit on Additional Execution After Budget Exhaustion, and Replenishment (P1)
1. Because of more frequent Events than expected or a long execution time, the CBS remaining budget (`remaining_us`) of the task reaches `0` (budget exhaustion). Exhaustion alone does not cause throttling, and the rule in item 2 applies.
2. If the task exhausts its budget exactly and sleeps, it is not throttled. If it remains runnable, or if its remaining budget is still 0 after applying the activation-based replenishment rule when it is woken, it is moved to the Throttled queue before further execution.
3. When the current replenishment window (from the kernel activation time, for $T_s$) ends, the budget is restored to $C_s$ and a new window starts, and the task returns to the L2 queue and resumes execution. Replenishment takes place within a bounded delay **regardless of whether other tasks are active** (PRT-003). If the deadline passes during this period, a failure is detected according to PRT-009.

### UC-03: L1 Protection under TT + CBS Coexistence (P1)
1. On the same isolated CPU, an L1 (TT) task A (period 20 ms, WCET 2 ms) and an L2 (CBS) task X that uses the explicit connection (MIT 18 ms, WCET 2 ms, D 5 ms) are placed (example figures).
2. An Event arrives at X 0.5 ms before the start of A's TT slot. X starts execution in the Gap, but its slice is limited to "until the next TT start," so it yields the CPU before A starts.
3. When A finishes its slot, X resumes and completes within its remaining budget. The activation jitter of A is within the REQ-NFR-001 bound regardless of whether X is present.

### UC-04: Runtime Addition and Removal of L2 Workloads (P1 basic / P2 no disturbance)
1. While the system is running L1/L2 workloads, Pullpiri registers a new L2 workload Y with `AddSchedInfo`.
2. TIMPANI-O performs the admission of Y and, on success, publishes the table (P1: republication of the full table is allowed. P2: affected nodes only, with epoch maintained).
3. TIMPANI-N adds Y (P1: normal re-registration of existing tasks is allowed. P2: no omission of existing L1/L2 activations is guaranteed). The same applies to removal (`RecoveryService` STOP). On removal or re-registration, open Events are closed according to the rules of §2.3 (IF-007).

### UC-05: Multiple L2 Servers on the Same CPU (P1)
1. L2 tasks X (D 5 ms) and Z (D 20 ms) are placed on one isolated CPU, and their two Events arrive almost at the same time.
2. TIMPANI-N dispatches according to the defined ordering policy (P1: FIFO by activation order), and the admission (CBS-004) checks in advance that both tasks meet their deadlines under that policy.

### UC-06: MIT Violation Burst (P1)
1. Because of an input-side malfunction, Events arrive repeatedly at X at intervals shorter than the MIT (e.g., MIT/3).
2. The connection path counts MIT violations on an **arrival basis**, and it also accepts the violating Events and places them in the queue. X runs only within the CBS budget range, so it does not affect L1 or other L2 tasks. Events that arrive while X is processing remain `QUEUED`, and the next Job consumes them (R2~R4). There is no loss, and it is observable through the increase in queue depth and the MIT violation count.
3. A failure of a Job whose deadline passed during throttling receives the `THROTTLED` flag from the detection path. If an MIT violation of the same source is close in time, it also receives the `MIT_RELATED` flag (PRT-007). The flag is not a cause determination. Cause classification and cause-based recovery are P2 (DDR-008 D-23).

### UC-07: Event Silence Monitoring (P2, reference)
If no Event arrives within the expected maximum arrival interval, the `WATCHDOG` fault is raised.

### UC-08: Detection of Elapsed Deadline During Waiting or Throttling (P1)
1. Because the worker has not finished the previous Job, Event E is in the `QUEUED` state, or the Job of E is throttled because the CBS budget is exhausted.
2. E is still open after `t_dl` has passed. DeadlineMonitor sets MISSED without waiting for the worker's completion (PRT-009, detection latency TIM-009), and passes the failure record to the TIMPANI-O reporting path (TIM-010).
3. If the worker later processes E and completes it, the failure of the same Job is not counted again. The completion record is kept separately.

---

## 4. System Requirements

> Notation: in the **Phase** column, `P1` = Phase 1 (basic functions), `P2` = Phase 2. `P1*` = satisfied in a simplified form in Phase 1 and strengthened in Phase 2. The verification of each item is linked to the "Related requirements" column of the acceptance criteria in §5. The IDs of v1 to v4 were not changed. IDs whose meaning changed in v5 were revised in the body, and retired rules are recorded in the §0 change history.

### 4.1 Event Definition and Workload Model (MOD)

| ID | Requirement | Priority | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-MOD-001** | The system SHALL classify safety-critical workloads that are activated aperiodically by external events and have a Minimum Inter-arrival Time (MIT) as **L2 (Sporadic + SafetyCritical)**, and SHALL be able to specify `min_inter_arrival_us`, `wcet_us`, and `deadline_us` for each task. The MIT is the **Event source attribute** of the task (the minimum interval between consecutive arrivals), and in P1 one Event source per L2 task is implicitly declared (§2.2). (verification: AC-008-08) | High | P1 |
| **REQ-008-MOD-002** | The system SHALL model the processing of an Event-based task as **Event acceptance → Job start (worker acquire) → explicit done (COMPLETED or DISCARDED), or TIMPANI close (CANCELLED or ABANDONED)**, and SHALL define enforcement, fault determination, and observation output by the lifecycle of §2.3 and the deadline rules of §2.5. The kernel execution interval (runnable, first execution, sleep) is not a Job boundary of this model. (verification: AC-008-01, AC-008-13) | High | P1 |
| **REQ-008-MOD-003** | The system SHALL NOT regard resumption after internal blocking during a Job (mutex, I/O, preemption) as a new Job, and the blocking time SHALL be recorded as included in the response time R of that Job. Because Job boundaries are determined only by the worker's explicit acquire and done (DET-005, DET-007), the Job boundary does not change even when blocking occurs. The deadline of a blocking Job is monitored but is not guaranteed by the admission (CBS-004) (A-06). (verification: AC-008-13, AC-008-04) | High | P1 |
| **REQ-008-MOD-004** | The system SHALL verify `0 < wcet_us ≤ deadline_us ≤ min_inter_arrival_us` for the timing parameters of an L2 task at admission, and SHALL reject the workload on violation. `deadline_us > min_inter_arrival_us` (arbitrary deadline) is out of scope. (verification: AC-008-08) | High | P1 |
| **REQ-008-MOD-005** | The system SHALL NOT admit as L2 an Aperiodic workload for which no MIT is defined. (DDR-001 §3 compliance; verification: AC-008-08) | Medium | P1 |
| **REQ-008-MOD-006** | The TIMPANI workload model, protocol, and library contract SHALL NOT include identifiers of a specific middleware or transport layer (service, instance, topic, event IDs, etc.). An Event is identified at the connection path as `(Event source, gen, seq)`. If there is no Event record, there is also no Event identifier (MOD-011). In P1, the identifier of an Event source is the `(workload_id, task_id)` of the consuming task. (verification: AC-008-12) | High | P1 |
| **REQ-008-MOD-007** | The system SHALL distinguish **Event (arrival)** from **Job (processing)**. An accepted Event corresponds to one Job, and an Event accepted during processing must remain `QUEUED` and must be delivered to the next Job without loss or duplication (R2~R4). A post that cannot be accepted (source not registered, queue full, etc.) SHALL be returned to the poster as an error. Under normal conditions, Jobs and Events are 1:1 (I1). (verification: AC-008-01, AC-008-07, AC-008-13) | High | P1 |
| **REQ-008-MOD-008** | The meaning of the lifecycle events (acceptance, acquire, completion, discard, cancel, abandon, and the kernel execution interval) SHALL be **identical regardless of the connection method**. Adding or replacing a connection method must not change the model, the application contract, or the observation output format, and the provision scope and limits of each method must be documented (§2.7). (verification: AC-008-12) | High | P1 |
| **REQ-008-MOD-009** | The system SHALL process each accepted Event as **one independent Job**. Multiple pending Events must not be merged into one Job, and a Job must not be split into several Jobs according to the number of Events. Jobs acquire Events in FIFO order, and an Event that is discarded or cancelled before processing is recorded as a separate result, not as a completion (§2.3). (verification: AC-008-13, AC-008-01) | High | P1 |
| **REQ-008-MOD-010** | An Event SHALL be identified by `(gen, seq)`, `seq` SHALL increase in posting order within the source, and `gen` SHALL change each time the source is activated again. Each accepted Event SHALL reach exactly one of the terminal states of §2.3 (I1), and, in periods in which the poster stays alive, the results SHALL agree with the aggregate statistics. (verification: AC-008-01, AC-008-15) | High | P1 |
| **REQ-008-MOD-011** | The **CBS budget management** and **Event/Job management** of the same L2 worker SHALL be distinguished as complementary responsibilities, and the application SHALL NOT be reclassified by whether the explicit Event connection is used. Even without the connection, CBS budget management, CPU pinning, BUDGET_EXCEED, and execution-interval recording are maintained. Without the connection, Event identification and arrival time, Job records, MIT counting and MIT_VIOLATION, and Event-based DMISS cannot be obtained. The same admission formula is used for all L2 tasks, and when there is no Event record, D is an admission parameter and not a monitored Job deadline (§2.8). (verification: AC-008-01(b), AC-008-02, AC-008-16) | High | P1 |

### 4.2 Event Detection and Activation (DET)

| ID | Requirement | Priority | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-DET-001** | TIMPANI-N SHALL observe and record the **kernel execution intervals** of all L2 tasks (runnable transition `t_act`, first execution `t_start`, voluntary sleep `t_sleep`). Execution intervals are used for CBS budget enforcement and execution observation, and they are not Job boundaries (DET-007). Because the kernel does not distinguish the cause of a wake-up (whether it is an Event arrival), an Event arrival is not estimated from an execution interval. (verification: AC-008-05, AC-008-01(b)) | High | P1 |
| **REQ-008-DET-002** | TIMPANI-N SHALL detect Job completion by the explicit done posted by the worker (DET-007). The kernel's voluntary sleep, preemption, and slice expiry are not Job completion. The completion time follows the definition of `t_end` in §2.5. (verification: AC-008-13, AC-008-14) | High | P1 |
| **REQ-008-DET-003** | Event detection and scheduling control SHALL guarantee **application transparency (Transparency)**. An L2 application must be able to use standard Linux event-waiting mechanisms (socket, epoll, poll, futex, condvar, etc.) as they are, and even in that case CBS budget management is maintained (MOD-011). It must not be forced to use the TIMPANI-specific periodic synchronization API (`ttsched_wait_next_period()`) or to make direct calls to real-time scheduling system calls. To obtain Event-based deadlines and Job records, the explicit connection API (DET-004) must be called. (verification: AC-008-01(b), AC-008-12) | High | P1 |
| **REQ-008-DET-004** | TIMPANI SHALL provide a transport-neutral **explicit Event connection path** that applications may use **optionally**. (a) **Post (post)**: accepts one Event into the specified source together with its arrival time and payload. If the arrival time is omitted, the time of the post call is used. If the post cannot be accepted, it returns the rejection reason (IF-007). (b) **Event wait (wait)**: waits until there is an Event to process, and within the same call confirms and records the start of the Job for one Event, then hands over the Event and returns (DET-007). If a QUEUED Event exists, it returns without blocking (R3). There is no separate public acquire API. (c) **done (done)**: the worker explicitly ends the Job by completion or discard. (d) Source handle acquisition: performed at initialization. This path is the only source of Event-based deadlines and Job boundaries (§2.7). The C ABI names and signatures of the provided operations are proposals of IF-005. The injection point of middleware-boundary observation (REQ-FUN-009) is also this path. (verification: AC-008-01, AC-008-12) | High | P1 |
| **REQ-008-DET-005** | TIMPANI-N SHALL determine Job boundaries **only by explicit acquire and done signals**. The worker's voluntary sleep, preemption, and blocking are not Job boundaries, and they are recorded as events inside the Job. Job boundaries are not estimated without an explicit Event connection (MOD-011). A-06 (the former assumption prohibiting blocking within a Job) is not a correctness premise. However, the admission (CBS-004) guarantees only Jobs that do not block, so the deadline of a blocking Job is monitored but not guaranteed. (verification: AC-008-13, AC-008-04) | High | P1 |
| **REQ-008-DET-006** | TIMPANI-N SHALL identify, **within `T_bind` after thread creation**, an L2 task thread that did not exist when the table was applied (a worker created late, or a thread whose PID changed because of a container restart), and SHALL apply the CBS policy to it. The target value of `T_bind` is given in §4.6, TIM-006. P1 is periodic rescan; P2 is based on cgroup and Pullpiri metadata (SRS-000 `REQ-NFR-004`). (verification: AC-008-10) | High | P1* |
| **REQ-008-DET-007** | The worker SHALL convey **explicit Job boundaries**. A Job starts at the time the worker acquires an Event and ends at the time the worker states completion or discard explicitly. Each consecutive Job (R3) also must state its acquire and end explicitly, and TIMPANI records and determines the two Jobs separately. If the worker attempts the next acquire without having explicitly ended the Job, the attempt SHALL be rejected as an error. (verification: AC-008-13, AC-008-01(a)) | High | P1 |

### 4.3 CBS-Based Scheduling Behavior (CBS)

| ID | Requirement | Priority | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-CBS-001** | TIMPANI-O SHALL derive the CBS server parameters from the timing specification of the L2 task (budget $C_s = \text{wcet\_us}$, replenishment period $T_s \le \text{min\_inter\_arrival\_us}$), and, after placing the L1 TT slots of the target isolated CPU, SHALL verify schedulability within the residual capacity (Gap) ($U_{\text{tt}} + U_{\text{cbs}} + U_{\text{overhead}} \le U_{\text{bound}}$). | High | P1 |
| **REQ-008-CBS-002** | TIMPANI-N SHALL dispatch an activated L2 task only in the **TT Gap interval** (the time interval in which the L1 TT slot is inactive) of its assigned isolated CPU, and when the start time of an L1 TT slot arrives during execution, it SHALL immediately yield the CPU (Slice Capping and Preemption) to guarantee the temporal determinism of L1. (verification: AC-008-02) | High | P1 |
| **REQ-008-CBS-003** | TIMPANI-N SHALL track the actual execution time each time an L2 task runs on the CPU, and SHALL charge it to the remaining budget (`remaining_us`) of the CBS server. (verification: AC-008-03) | High | P1 |
| **REQ-008-CBS-004** | The admission of TIMPANI-O SHALL, in addition to the utilization condition, verify a **sufficient condition for deadline satisfaction** for **all** L2 tasks on the target CPU (both newly added tasks and tasks already placed). The same admission formula is used regardless of whether the explicit Event connection is used. **P1 condition** (conservative simple formula): for each L2 task $i$ (CPU $k$), $C_i + B_{tt}(k, D_i) + L_{del}^{max} + \sum_{j \ne i} (C_j + \delta_{ovr}) \le D_i$, and $D_i \le \text{MIT}_i$ and $T_i \le \text{MIT}_i$ must hold. $\text{MIT}_i$ is `min_inter_arrival_us`, and if it is absent the period is used (D-11 rule). $L_{del}^{max}$ is the upper bound from the arrival time to the worker's first execution (on an idle CPU without TT, the sum of TIM-011 and TIM-001; waiting during TT occupation is covered by the $B_{tt}$ term), and $\delta_{ovr}$ is the positive upper bound on the amount by which one server can run beyond its budget before it is throttled (TIM-004). The P1 candidate values of the two and their agreement status follow TIM-004 and TIM-011. **Premises (P1)**: (1) TT tasks keep their declared execution times (a TT overrun is a TT deadline miss and is outside the L2 guarantee). (2) $L_{del}^{max}$ and $\delta_{ovr}$ are values confirmed by measurement. The $L_{post}$ upper bound holds in the reference poster configuration (the configuration in which the poster thread is not preempted by another thread from the recording of the arrival time until the post call returns) (TIM-011, A-10). (3) The arrivals of each source keep the MIT. (4) Jobs do not block (DET-005; on violation, that Job has no guarantee and is only monitored). (5) The adapter posts immediately after the arrival. An Event posted with a past time uses part of $D$ and is outside the guarantee of the admission. (6) The charged execution time from activation to the actual sleep (including the cost of entering the wait) is within the declared $C_i$. (7) The worker returns to the wait state before the next arrival (a boundary case of PRT-001). (8) `tol` covers the measured activation jitter. **P2 condition**: a precise formula based on window demand and supply functions (EDF order). (verification: AC-008-08, AC-008-16) | High | P1* |
| **REQ-008-CBS-005** | L2 tasks SHALL execute only on the CPU assigned by TIMPANI-O (Partitioned Scheduling — DDR-004 §1). Migration to another isolated CPU is not allowed. (verification: AC-008-06) | High | P1 |
| **REQ-008-CBS-006** | CBS budget charging SHALL be performed at 1 µs resolution, and the cumulative **overrun** per server within one replenishment period SHALL NOT exceed the bound of TIM-004 in §4.6. (verification: AC-008-03) | High | P1* |
| **REQ-008-CBS-007** | The CBS execution of L2 SHALL NOT depend on whether TT exists on that CPU or node. On a CPU with no TT slots (a node without TT, or a CPU without TT in a mixed node), Event acceptance, dispatch, budget enforcement, budget replenishment (including backup replenishment), and deadline monitoring must operate without TT_START/TT_END events. The admission on this CPU uses $B_{tt} = 0$ (CBS-004). (DDR-008 D-22. verification: AC-008-17, AC-008-18) | High | P1 |
| **REQ-008-CBS-008** | If the assigned CPU is not in the allowed CPU set of the worker thread (the affinity mask constrained by `cpuset.cpus.effective`), TIMPANI-N SHALL NOT bind the task (no switch to SCHED_EXT) and SHALL record an explicit error. The L2 binding path SHALL NOT silently run the task on another CPU, move the cgroup, or widen the affinity. The task remains in the unresolved list and is retried in the re-identification cycle (DET-006). (DDR-008 D-24. verification: AC-008-19) | High | P1 |

### 4.4 Protection and Fault Handling (PRT)

| ID | Requirement | Priority | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-PRT-001** | A remaining budget of 0 means exhaustion. If the remaining budget is 0 after charging and the task remains runnable, or if the remaining budget is still 0 when the task is woken even after applying rule (a), TIMPANI-N SHALL throttle the task before further execution. A task that exhausts its budget exactly and sleeps is not throttled. Budget is replenished in units of windows of length $T_s$. (a) At kernel activation, if the previous window has ended (including the tolerance of DDR-008 D-04), the budget is filled to $C_s$ and a new window starts. (b) A throttled server is filled to $C_s$ and starts a new window at promotion after the existing window has ended. Execution per window SHALL NOT exceed the sum of $C_s$ and the overrun allowance of TIM-004. Replenishment is not performed by a new Job alone. Consecutive Jobs, or Jobs resumed after blocking, share the budget of the open window. When the premises of CBS-004 (in particular MIT compliance, actual sleep before the next arrival, and `tol` covering the activation jitter) hold, the activation for a Job SHALL receive the full budget. If the worker cannot sleep before the next arrival, the next Job shares the remaining budget and may be throttled, so a margin between $D$ and the MIT should be kept. After a violation, the budget is recovered at a qualified activation or at promotion. The cost of a violation is borne by the violating task and is not passed on to other tasks. Budget carry-over SHALL be identifiable by the CARRYOVER flag of PRT-007. (verification: AC-008-03, AC-008-07) | High | P1 |
| **REQ-008-PRT-002** | TIMPANI-N SHALL determine **deadline miss (DMISS)** on an Event basis, and when the configured allowed threshold (`max_dmiss`) is exceeded, SHALL report it to the upper layer (TIMPANI-O). All failures are counted in the mandatory statistics regardless of whether they are reported (OBS-004). The criterion of determination is the absolute deadline of the Event, $t_{arr} + D$ (PRT-008, §2.5), and the deadline is not approximated by the kernel activation time. A failure is counted only once per Job and is not reported twice. The detection time and method are determined by PRT-009. (verification: AC-008-04, AC-008-14) | High | P1 |
| **REQ-008-PRT-003** | Budget replenishment SHALL complete **without depending on scheduler activity (enqueue/dispatch of other tasks)**, within the delay bound of TIM-003 in §4.6 from the replenishment time. (Making the Backup path among Lazy + Backup in DDR-007 §4.5 mandatory. verification: AC-008-03) | High | P1 |
| **REQ-008-PRT-004** | TIMPANI SHALL count and report as an **MIT violation** a case in which the interval between consecutive **arrivals** of an Event source is shorter than `min_inter_arrival_us`. The counting criterion is the arrival time $t_{arr}$, and it is counted by the explicit connection path. The attempts counted as arrivals are acceptance, rejection due to a full queue, and rollback to REVOKED. A post rejected at the first check because the source is inactive is not an arrival (§2.6). A violating Event is also accepted and placed in the queue, and only the fact of the violation is counted and reported. CBS isolation SHALL be maintained even in case of an MIT violation (FFI). The **P1 policy** is "absorption within the remaining budget" (a violating arrival does not receive new budget). Alternative policies (delay/discard) are reviewed in P2 (OI-3). Approximate counting based on activation intervals is not included in the requirements. (verification: AC-008-07) | High | P1 |
| **REQ-008-PRT-005** | The system SHALL classify and report the following anomalies as **mutually distinct fault types**: `DMISS` (deadline miss based on Event), `BUDGET_EXCEED` (actual entry into throttling; informational, and does not mean a positive overrun execution), `MIT_VIOLATION` (model violation, determined by TIMPANI-N through arrival counting) [P1]; `WAKEUP_LATENCY_EXCEED` (wake-up latency exceeded), `WATCHDOG` (event silence) [P2]. Each type SHALL be delivered **without loss of type** along the TIMPANI-N → TIMPANI-O → Pullpiri path. BPF neither determines nor emits MIT violations (FR-B-024). (SRS-000 REQ-FUN-014. verification: AC-008-11) | High | P1* |
| **REQ-008-PRT-006** | Informational events (`BUDGET_EXCEED`) SHALL be **aggregated (rate-limited)** and reported at most once per throttle episode per server, and SHALL NOT exhaust the bandwidth of the fault reporting path. (verification: AC-008-11) | Medium | P1 |
| **REQ-008-PRT-007** | The `DMISS` report SHALL include a **flag**. The flag field is carried in the report only after the FaultInfo protocol proposal (IF-004, subject to approval) is approved. Before approval, the report carries only the count. The meaning of the flags is as follows. `NOT_STARTED`: at the detection time, the Event was still in the `QUEUED` state and had not been acquired. `THROTTLED`: the CBS state at the detection time was throttled (it is filled only with the value read on the detection path; a failure set by the worker or by close cleanup does not carry this flag). `MIT_RELATED`: the time of the last MIT violation of the same source was after $t_{arr} - T_s$ (associated with an input-side violation). `LATE_COMPLETION`: the worker's completion transition set the deadline flag. `DETECTED_LATE`: set by close cleanup (occurred while the daemon was stopped, §2.5). Budget carry-over (CARRYOVER) is a flag of the BPF execution-interval record and is not a P1 DMISS report flag. `MIT_RELATED` is not a cause determination but a temporal proximity flag. The distinction between model violation and execution fault is made in the P2 cause classification (DDR-008 D-23). (verification: AC-008-07, AC-008-14) | Medium | P1 |
| **REQ-008-PRT-008** | The **absolute deadline** of an Event SHALL be $t_{arr} + D$, and it is fixed and stored once at acceptance. Activation, budget replenishment, resumption, and worker delay SHALL NOT move the deadline later. The activation time + D must not be used as a deadline. Even if another Event of the same source arrives, the deadline of this Event does not change. (verification: AC-008-13) | High | P1 |
| **REQ-008-PRT-009** | TIMPANI-N SHALL detect the deadline miss of an Event **without waiting for the completion of the Job**. The target states are all of the following: waiting (QUEUED), running (RUNNING), preempted, throttled, and CPU idle. The detection criterion is current time > $t_{dl}$. MISSED is set only once per Job, and the race between completion and detection is resolved by the completion-time race rule of §2.5. A timely completion with late collection is not a failure. Completion, discard, cancel, or abandon after the deadline is a failure. A Job closed by TIMPANI before the deadline is not a failure (OI-21). The upper bound of detection latency is TIM-009 (agreement required). **P1 limitation**: while the daemon is down (abnormal termination), there is no entity to detect. A failure found by cleanup after restart is recorded after the fact as `DETECTED_LATE` and is not a real-time report. A daemon liveness signal (heartbeat) is not in P1 (F-14). (verification: AC-008-14) | High | P1 |

### 4.5 Placement and Ordering (ORD)

| ID | Requirement | Priority | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-ORD-001** | When placing an L2 task on an isolated CPU, TIMPANI-O SHALL consider the Gap structure per CPU (minimum Gap length ≥ `CBS_MIN_EXEC_US`, maximum continuous TT occupation $B_{tt}$), and if the placement result does not satisfy the admission formula of CBS-004, it SHALL try another CPU or reject the placement. The admission input $C_i$ is the worst-case execution time of one Job under normal conditions. Because the budget is granted per replenishment window rather than per Job, the admission formula does not multiply by the number of Jobs. (verification: AC-008-08) | High | P1 |
| **REQ-008-ORD-002** | When two or more L2 servers exist on the same CPU, TIMPANI-N SHALL apply a **deterministic dispatch order policy**. **P1**: FIFO of kernel activation order (the execution-ready transition of the execution interval). A server throttled after exhausting its budget must enter the back of the queue after budget replenishment, and must not overtake the execution of other servers again merely because it was activated earlier. **P2**: based on the absolute deadline (EDF). The admission (CBS-004) must assume the same policy. (verification: AC-008-07, AC-008-16) | High | P1* |
| **REQ-008-ORD-003** | Budget isolation SHALL hold among L2 servers. The budget overrun or MIT violation of one L2 task must not increase the response time of other L2 tasks on the same CPU beyond the interference assumed at the time of admission. (verification: AC-008-07, AC-008-16) | High | P1 |

### 4.6 Quantitative Timing Requirements (TIM)

> Statistical basis: for items stated with a percentile, the basis is that percentile. For items stated as "maximum" (TIM-004/005/006/009/010), the basis is the maximum within the observation interval (p100). For TIM-008, the basis is the mean over the same interval. Unless stated otherwise, the observation interval is one acceptance test run (rated load, 1 h).

> Each item also states its measurement method. A target value marked `(candidate)` must be finalized by measurement on the target HW (aarch64 — for example, Raspberry Pi 5, PREEMPT_RT 6.12, sched_ext), and until it is finalized it is used temporarily as the pass/fail criterion of SRS-008 and the test strategy. Here, "agreed upper bound" refers to the value to be agreed in this table, and until it is finalized only the candidate value is referenced. The observation condition is by default "isolated CPU, rated load (all accepted L1/L2 active), continuous observation for 1 hour." All of them start measurement in P1.

| ID | Requirement | Target value | Measurement method |
|:--|:--|:--|:--|
| **REQ-008-TIM-001** | **Wake-up latency (Gap state, kernel-side definition)**: When the assigned CPU is in a Gap and idle, the delay $L_{wake} = t_{start} − t_{act}$ from the runnable transition of the execution interval to the first execution SHALL be bounded. The interval from the arrival time of the explicit connection to runnable is measured separately in TIM-011. | ≤ 50 µs @ p99.99 (candidate) | BPF trace ($t_{act}$ runnable transition and $t_{start}$ first execution in the execution interval) → histogram in the verification framework |
| **REQ-008-TIM-002** | **Wake-up latency (TT occupation state)**: The $L_{wake}$ of an L2 execution interval that became runnable while a TT slot is active SHALL NOT exceed $B_{tt} + L_{wake}$. The interval from the arrival time to runnable is determined by TIM-011. | ≤ $B_{tt}$ + 50 µs @ p99.99 (candidate) | Same trace. $B_{tt}$ is computed from the table |
| **REQ-008-TIM-003** | **Replenishment latency**: In the throttled state, the delay from the arrival of `replenish_at_ns` until the task becomes runnable (promotion to the L2 queue + CPU rescheduling) SHALL be bounded. It must hold even under conditions with no event or dispatch activity at all. | ≤ 100 µs @ p99.99 (candidate) | Difference between the firing time of the Backup Replenisher and the restore time of BPF `remaining_us` |
| **REQ-008-TIM-004** | **Budget overrun allowance**: The amount by which the actual execution time within a server's replenishment period exceeds $C_s$ (overrun) SHALL be bounded. This value is used as $\delta_{ovr}$ in the admission formula (CBS-004). | P1: ≤ 1 scheduler tick (1 ms at HZ=1000), maximum (candidate; measurement required, including nohz_full CPU and tick restart latency); P2: ≤ 50 µs (if HRTICK is adopted, candidate) | Accumulated exec vs budget at BPF stopping; measured per kernel configuration (CONFIG_HZ, HRTICK) |
| **REQ-008-TIM-005** | **Delay from deadline miss detection to reception by TIMPANI-O**: The delay from the time the absolute deadline $t_{dl}$ of an Event has passed until `FAULT_DMISS` reaches TIMPANI-O SHALL be bounded. This delay is the sum of the detection latency (TIM-009) and the reporting latency (TIM-010). The earlier criterion of detecting at the same time as completion processing is retired. | Agreed upper bound of TIM-009 + TIM-010 (candidate values: refer to the two items: 2 ms + 200 ms) | Difference between $t_{detect} − t_{dl}$ of the detection record and the TIMPANI-O reception time |
| **REQ-008-TIM-006** | **Late binding (`T_bind`)**: The late thread identification delay of DET-006. | ≤ 1 s, maximum (candidate) | Thread creation time vs `task_meta_map` registration time |
| **REQ-008-TIM-007** | **L1 non-interference**: The effect of the existence and runaway of L2 workloads (P1), and of their addition and removal (P2), on L1 TT activation jitter SHALL be **statistically no different** within the bound of REQ-NFR-001. | p99.99 jitter difference when comparing with and without L2 ≤ 10 µs (candidate) | A/B comparison of Timer Master jitter histograms |
| **REQ-008-TIM-008** | **Observation overhead**: The CPU time that the explicit connection path (post, non-blocking part of Event wait, done) and the scanning of deadline monitoring add per Event SHALL be bounded. | ≤ 2 µs per event, mean (candidate) | Measurement of BPF ops execution time (`bpf_prog` statistics) + connection path microbenchmark |
| **REQ-008-TIM-009** | **Deadline detection latency**: The delay from the elapse of $t_{dl}$ to the setting of MISSED SHALL be bounded. It is determined by the sum of the scan period and the wake-up jitter of the detection thread, and it holds under the condition that higher-priority load does not occupy the CPU of the detection thread. | ≤ 2 ms, maximum (candidate; agreement and measurement on the Linux target needed, OI-18) | Histogram of $t_{detect} − t_{dl}$ in detection records; deadline injection experiments (waiting, throttled, and preempted states, each) |
| **REQ-008-TIM-010** | **Deadline miss reporting latency**: The delay from the setting of MISSED to the reception of FaultInfo by TIMPANI-O SHALL be bounded. This applies only to failures that are reported. Failures that are not reported under the threshold rule are counted only in statistics (PRT-002). | ≤ 200 ms, maximum (candidate; agreement and measurement needed, OI-18) | Difference between the detection time and the TIMPANI-O reception time |
| **REQ-008-TIM-011** | **Explicit connection delivery latency (arrival → runnable)**: The delay $L_{post} = t_{act} − t_{arr}$ from the arrival time $t_{arr}$ recorded in the Event (supplied value or post call time) until the worker becomes runnable SHALL be bounded when the poster thread is in the **reference poster configuration** (A-10). TIMPANI verifies, in this configuration, the `libttsched` post together with the worker's kernel runnable transition. Additional delay arising in a deployment outside the reference poster configuration is outside the guarantee of the admission formula (A-10). Together with TIM-001, it forms $L_{del}^{max}$ of CBS-004. Waiting during TT occupation is covered by the $B_{tt}$ term. $L_{del}^{max}$ is a target value for the time from the arrival time to the worker's first execution, and it is not a proven worst case. | $L_{post}$ ≤ 50 µs @ p99.99 (candidate, reference poster configuration); $L_{del}^{max}$ candidate 100 µs (agreement and measurement needed, OI-18) | Difference between $t_{arr}$ in the Event record and the runnable transition time (BPF trace). Only Events arriving while the worker is waiting are measured |

**P1 delivery latency verification candidate.** 50 µs is the initial target that splits the delivery-latency candidate of 100 µs for the idle, no-TT reference configuration between $L_{post}$ and $L_{wake}$. It is not a measured result or a guarantee of worst-case latency that supports the number. At a representative L2 deadline, the candidate is re-evaluated as the delivery allowance left after subtracting the CPU budget, TT and peer-server interference, and margin. In the reference poster configuration (A-10), the $t_{arr}$, $t_{act}$, and $t_{start}$ of the same Event that woke a worker that had actually slept are paired. $L_{post}$ includes the IRQ before the runnable transition and the remote wake-up wait. The total delay is measured directly as $t_{start}-t_{arr}$, without summing the percentiles of each interval. Record the board and kernel, CPU placement, IRQ and load conditions, number of samples, the p99.99 of both intervals and of the total, and the number of candidate exceedances for each.

### 4.7 Interface Requirements (IF)

| ID | Requirement | Priority | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-IF-001** | The Pullpiri → TIMPANI-O interface SHALL be able to carry the MIT of an L2 task in a **dedicated field separate from the period (`period`)**. Backward compatibility for the existing input that reuses `period` SHOULD be kept for the stated transition period. (DDR-001 §5, DDR-003 §3. verification: AC-008-08) | High | P1 |
| **REQ-008-IF-002** | The Pullpiri → TIMPANI-O interface SHALL be able to specify a time class (Periodic/Sporadic) on a **per-task** basis. One workload (container) must be able to have both TT threads and Event-Driven threads. In P1, the workload-level class (current) is sufficient, and the sample app registers TT and L2 as separate workloads. | Medium | P2 |
| **REQ-008-IF-003** | The TIMPANI-O → TIMPANI-N interface SHALL publish L2 entries as the `ExecutionModel.EVENT_DRIVEN` layer, and an entry SHALL include at least $C_s$, $T_s$, MIT, D, `current_limit`, and the assigned CPU. An entry must not carry a configuration flag that indicates whether the explicit connection is used (MOD-011). (verification: AC-008-08) | High | P1 |
| **REQ-008-IF-004** | The TIMPANI-O → Pullpiri fault interface SHALL be able to express all fault types of PRT-005. (verification: AC-008-11) | High | P1 |
| **REQ-008-IF-005** | The application API of the explicit Event connection path (DET-004) SHALL be provided as a C ABI. **This is a proposal and requires Project Leader approval.** The cost per call must be within TIM-008, and it SHALL operate without heap allocation or blocking I/O. The blocking wait itself of Event wait (wait) is excluded from this constraint. It SHALL NOT take middleware identifiers as arguments (MOD-006). A cancel API during Event wait is not in P1 (LLD L-8). Draft names and signatures are placed in HLD-008 §8.1 and LLD-008. (verification: AC-008-01, AC-008-12) | High | P1 |
| **REQ-008-IF-006** | When the admission rejects a workload, TIMPANI-O SHOULD return the rejection reason (utilization exceeded / deadline not satisfied / parameter violation / no CPU) to Pullpiri as a **structured code**. In P1, the log and `status` alone are sufficient. | Medium | P2 |
| **REQ-008-IF-007** | **Event queue contract** of the explicit Event connection: the connection path SHALL own an Event queue with a bound per source. The queue depth (candidate 8: the upper bound on ring occupancy, that is, the sum of QUEUED, RUNNING, and terminal items that the head has not yet handed over; a power of 2) and the payload upper bound (candidate 64 bytes, inline) are candidate build constants and are **subject to Project Leader approval**. The rules are as follows. (a) Acceptance: if the post succeeds, the Event exists, `(gen, seq)` and $t_{dl}$ are fixed, and it is a monitoring target. (b) Rejection: `E_INACTIVE` (source inactive, daemon stopped, during re-registration), `E_FULL` (queue full, overflow counted; it can also occur when the number of live Events is smaller than the queue depth), and `E_INVAL` (payload upper bound exceeded, future arrival time, arrival time earlier than the previous arrival) do not create an Event. (c) End: discard (DISCARDED), cancel (CANCELLED), abandon (ABANDONED), re-registration, and restart follow the rules of §2.3. (d) Consistency: the payload acquired by the worker, `(gen, seq)`, and $t_{arr}$ SHALL all belong to the same Event. The worker SHALL NOT process the data of another Event. **P1 limitation**: if the poster dies after advancing the tail and before step (f), the Event is processed and monitored, but one accepted count is missing, and later posts do not recover it. If the Event was made visible but the poster dies before advancing the tail, the next post recovers it and counts it. In both cases, until the next post or a TimerMaster wake-up, the worker does not wake for this Event, and the Event is under deadline monitoring. One Event being posted during re-registration may enter the arrival count of the new registration (at most one per registration). A failure count of the last Job of the previous binding may be reflected after the baseline of the new registration (at most one per binding). A process with write permission to the shared SHM is a trusted party (C-09). Large payloads and payload release are P2 (F-13). (verification: AC-008-15) | High | P1 |

### 4.8 Runtime Update Requirements (RTU)

| ID | Requirement | Priority | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-RTU-001** | L2 workloads SHALL be addable, removable, and modifiable at runtime without restarting the daemon. (SRS-000 REQ-FUN-012, DDR-011) | High | P1 |
| **REQ-008-RTU-002** | Addition and removal of L2 workloads SHALL NOT change the offset and epoch alignment of existing L1 TT slots, and SHALL NOT omit activations of existing L1/L2 tasks. (verified by TIM-007) | High | P2 |
| **REQ-008-RTU-003** | Because L2 entries do not have a hyperperiod phase, addition and removal of L2 alone SHOULD be applicable **immediately**, without waiting for a hyperperiod boundary. (DDR-006 §5 transition policy) | Medium | P2 |
| **REQ-008-RTU-004** | When an L2 workload is removed (STOP), TIMPANI-N SHALL remove the CBS state and metadata of that task, restore the scheduling class, and SHALL leave no remaining references. On removal, open Events are closed according to the rules of §2.3 (IF-007). | High | P1 |

### 4.9 Observability Requirements (OBS)

| ID | Requirement | Priority | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-OBS-001** | TIMPANI-N SHALL collect and expose, per L2 task (and its Event source), the following cumulative statistics: the numbers of arrival attempts, acceptances, overflows (`E_FULL`), and REVOKED; the number of acquire starts; the numbers of completions, discards, cancellations, and abandonments; the number of deadline misses (DMISS); the number of MIT violations; the number of BUDGET_EXCEED episodes; the maximum and histogram of response time $R$; the histogram of wake-up latency $L_{wake}$; and the budget use per Job (recent and maximum). Without an explicit Event connection, Event-related statistics cannot be obtained. The definition and baseline management of the statistics follow OBS-004 and FR-N-030. (verification: AC-008-01, AC-008-15) | High | P1 |
| **REQ-008-OBS-002** | The following records SHALL be provided so that the verification framework can extract lifecycle events. (a) **Job record** `{node, task, gen, seq, t_arr, t_dl, t_begin, t_end or close time, result, flag}`: includes the Jobs ended by the worker, the deadline-miss records, and the Jobs closed by TIMPANI-N. A Job closed before its start has no `t_begin`. Every Job record has its own `t_arr`, so the correspondence between Events and Jobs does not need to be matched offline. (b) **Execution-interval record** `{t_act, t_start, t_sleep, budget used, flag}` (BPF). All times must be based on CLOCK_MONOTONIC, and tracing activation must not exceed the overhead bound of TIM-008. Regardless of tracing activation, the statistics are maintained according to OBS-004. (verification: AC-008-01, AC-008-13) | High | P1 |
| **REQ-008-OBS-003** | The OBS-001 statistics SHOULD be reported periodically to TIMPANI-O (`NodeStatus`), so that comparison between nodes is possible. | Medium | P2 |
| **REQ-008-OBS-004** | The **mandatory statistics** of OBS-001 (connection path counters, deadline-miss counters, BPF budget counters) SHALL always be maintained regardless of tracing activation. Statistics that require tracing points (for example, execution on a CPU other than the assigned CPU) are outside the mandatory set, and the place where they are counted must be stated. Per-registration-generation values are computed as differences from the baseline at the activation time. The P1 limitation at the baseline boundary (delay of up to one failure count per binding) is recorded in IF-007. (verification: AC-008-01, AC-008-15) | High | P1 |

### 4.10 Non-Functional Requirements (NFR)

| ID | Requirement | Priority | Phase |
|:--|:--|:-:|:-:|
| **REQ-008-NFR-001** | The system SHALL support at least 32 L2 tasks per node and at least 8 L2 tasks per CPU. | Medium | P1 |
| **REQ-008-NFR-002** | The RT critical path of Event detection, replenishment, and fault detection (post, non-blocking part of Event wait, done, deadline monitoring, replenishment) SHALL contain no gRPC, heap allocation, or blocking I/O. (DDR-006 §4) | High | P1 |
| **REQ-008-NFR-003** | Adding L2 support SHALL NOT change the behavior or performance of L1-only configurations (no regression). Exception: because the SHM ABI of `libttsched` changes (the same type as C-03), L1 apps linked with `libttsched` must be rebuilt, and the behavior and performance criteria apply after the rebuild. | High | P1 |
| **REQ-008-NFR-004** | The design SHOULD place the Event detection point behind the OS abstraction boundary, considering porting to other RTOS such as QNX. (SRS-000 REQ-ENV-004) | Low | P2 |
| **REQ-008-NFR-005** | Protocol changes SHALL maintain **wire compatibility** by keeping existing field numbers and only adding new fields. New fields are added only after approval (C-03). | High | P1 |

---

## 5. Acceptance Criteria

> Numerical values for the pass/fail determination are not written directly in this table. Instead, the **agreed upper bound** of the corresponding TIM item is referred to. Candidate values are in the TIM items, and until they are finalized, the candidate values are used as the temporary criterion (OI-18).

| ID | Criterion | Pass condition | Verification means | Related requirements | Phase |
|:--|:--|:--|:--|:--|:-:|
| **AC-008-01** | Event acceptance, Job 1:1, and non-intrusiveness | (a) When the explicit Event connection is used: for 100,000 posts in a period in which the poster stays alive, acceptances + rejections = number of post attempts, all accepted Events reach exactly one terminal state (I1), and the number of Job records = number of acceptances (confirmed after stopping, on the same baseline). In a period in which the arrival intervals keep the MIT and all Jobs meet their deadlines, additionally the number of Jobs = number of arrivals. (b) An L2 workload that waits with standard blocking calls (socket/epoll/timerfd) without the explicit connection is registered as a `SCHED_EXT` CBS task, its kernel execution intervals are recorded, and it receives CBS budget enforcement (BPF) (no Event record; BPF execution intervals are not estimated as Events) | generic event generator (explicit connection path and standard path) + trace | MOD-006/007/009/010/011, DET-001/003/004/007, IF-005/007, OBS-004 | P1 |
| **AC-008-02** | TT + CBS coexistence and L1 protection | In a 1-hour observation of L1/L2 coexistence on the same isolated CPU, the p99.99 of the L1 TT activation jitter is within the agreed upper bound of TIM-007 relative to the case without L2, and there are 0 L1 DMISS (excluding the stabilization period right after startup) | Timer Master jitter histogram A/B | CBS-002, TIM-007, MOD-011 | P1 |
| **AC-008-03** | Budget charging and throttling | Exactly exhausted and then sleeps: no throttling or report. Remains runnable (including a sole worker on the assigned CPU), or wakes with 0 remaining in a state in which replenishment is not possible: throttled before further execution, no re-execution before replenishment, and one `BUDGET_EXCEED` emitted per entry. Wake-up with replenishment possible (including the `tol` boundary): full budget, and no throttling or report due to prior exhaustion. Overrun is within TIM-004, and throttle promotion delay is within TIM-003 (including the condition with no Event arrivals) | deterministic charge-amount test + comparison with the scheduling, queue, and emission records of the Linux target | CBS-003/006, PRT-001/003/006 | P1 |
| **AC-008-04** | Deadline miss count and duplicates | For each Event, the `missed` count increases by exactly 1 for each Job determined to be a deadline miss (number of Jobs subject to determination = sum of the counts). 0 duplicate reports of the same Job. Includes all causes: execution-time overrun, throttling, preemption, and waiting delay. When `max_dmiss` is exceeded, the agreed upper bound of TIM-005 is observed until reception by TIMPANI-O. For Jobs that do not block, consistent with the result of the admission (CBS-004) | generic event generator (D overrun, throttling induction, waiting injection) + comparison of Job records | PRT-002, PRT-007, MOD-003, DET-005, OBS-001 | P1 |
| **AC-008-05** | Wake-up latency bound | The p99.99 of $L_{wake}$ of execution intervals is within the agreed upper bound of TIM-001 under Gap conditions, and within the agreed upper bound of TIM-002 under TT occupation conditions. The p99.99 of $L_{post}$ from the arrival time to runnable is within the agreed upper bound of TIM-011 in the reference poster configuration (judged separately) | generic event generator + BPF trace | TIM-001/002/011, DET-001 | P1 |
| **AC-008-06** | CPU pinning | In a 1-hour observation, the number of times an L2 task runs outside its assigned CPU is 0 | `sched_switch` trace | CBS-005 | P1 |
| **AC-008-07** | MIT violation isolation | When a burst at intervals of MIT/3 is injected: the MIT violation count exactly matches the arrival-based count (error 0), zero loss of accepted Events (all accepted Events are in terminal states), violating arrivals are processed in the queue and run only within the CBS budget, L1 DMISS 0, the response time of other L2 tasks is within the admission assumption, and violation-related failures carry the `MIT_RELATED` flag (test after protocol approval) | generic event generator (burst injection) | PRT-004/007, ORD-002/003, MOD-007 | P1 |
| **AC-008-08** | Admission | A request is rejected if any of the conditions of CBS-004 — the formula $C_i + B_{tt}(k,D_i) + L_{del}^{max} + \sum_{j \ne i}(C_j + \delta_{ovr}) \le D_i$, $D_i \le \text{MIT}_i$, or $T_i \le \text{MIT}_i$ — is broken for any L2 task on the target CPU (including re-verification of existing tasks). After an accepted addition, all existing L2 tasks on the same CPU still satisfy the conditions. A rejected request does not change the existing table. The inputs $L_{del}^{max}$ and $\delta_{ovr}$ use the agreed values of TIM-011, TIM-001, and TIM-004 | GTest (timpani-o) | CBS-004, ORD-001, MOD-004/005, IF-001/003 | P1 |
| **AC-008-09** | Runtime addition and removal | During 10 repeated runtime additions and removals of L2 workloads while running: no daemon restart; after removal, residual BPF state is 0; open Events at removal are closed per the §2.3 rules (P1); zero omissions of existing L1/L2 activations (P2) | integration test | RTU-001/004 (P1), RTU-002/003 (P2) | P1 / P2 |
| **AC-008-10** | Late thread identification | A worker thread created after the table is applied has CBS applied within the agreed upper bound of TIM-006 | integration test | DET-006, TIM-006 | P1 |
| **AC-008-11** | Fault type preservation and rate limiting | `BUDGET_EXCEED`, `MIT_VIOLATION`, and `DMISS` arrive at Pullpiri with their types as-is. `BUDGET_EXCEED` is ≤ 1 per episode | mock_pullpiri reception log | PRT-005/006, IF-004 | P1 |
| **AC-008-12** | Transport neutrality and common model | Zero references to middleware identifiers in the TIMPANI core (O/N/BPF/libttsched). Even when the same L2 workload is driven through different delivery means (socket, pipe, timer, explicit Event connection), the AC-008-02~06 criteria hold without TIMPANI configuration or code changes (the arrival-based counting of AC-008-01(a), AC-008-04, and AC-008-07 is verified on the explicit connection path). The SOME/IP processing of the FCW sample exists only in the app adapter | code review + grep + generic event generator source switching | MOD-006/008, DET-003/004, IF-005 | P1 |
| **AC-008-13** | Independent Event and independent Job | While the worker processes Event A, if Event B is accepted: A's $t_{dl}$ remains $t_{arr,A} + D$ (not changed by the arrival of B), and B's $t_{dl}$ is $t_{arr,B} + D$. A consecutive Job in which, after A ends, the worker starts B without sleeping also remains as two Job records, each with its own $t_{begin}$, $t_{end}$, result, and deadline determination. Zero records merge the two Events | generic event generator (arrival time injection) + comparison of Job records | MOD-002/009, PRT-008, DET-002/005/007, OBS-002 | P1 |
| **AC-008-14** | Deadline detection timing and determination rules | (a) The MISSED flag of an Event whose deadline has passed while waiting (QUEUED) and while throttled (RUNNING) is set within the agreed upper bound of TIM-009, including the case in which the CPU is idle. (b) A Job that completed on time but whose collection was delayed (collection artificially delayed after completion is posted) is not counted as a failure. (c) In injection experiments in which detection and completion race, the recorded $t_{end}$ and the MISSED flag do not contradict each other, and the failure count per Job does not exceed 1. (d) Discard and cancel after the deadline are counted as failures, and TIMPANI close before the deadline is not counted as a failure. (e) The reporting latency is within the agreed upper bound of TIM-010 | generic event generator (waiting, throttling, completion delay, race injection) + detection records | PRT-002/009, PRT-007, DET-002, TIM-005/009/010 | P1 |
| **AC-008-15** | Identification, data, and generation consistency | (a) In an injection experiment that includes acceptance, rejection, overflow, discard, cancel, re-registration, and restart each, the payload acquired by the worker corresponds to the `(gen, seq)` and $t_{arr}$ of the same Event (checked for each Event processed). (b) `E_FULL` and `E_INACTIVE` rejections create no Event. An `E_FULL` rejection is reflected exactly once each in `arrivals` and `overflow`. A post that is rolled back after it was counted as an arrival (REVOKED, returning `E_INACTIVE`) is reflected exactly once each in `arrivals` and `revoked`. A post rejected at the first check because the source is inactive (`E_INACTIVE`) is not an arrival and changes no counter (§2.6). (c) After re-registration, timer, completion, and end records of the previous generation (`gen`) do not change the state of the new generation. Their effect on the counts of the new generation stays within the P1 limits of IF-007 (at most one arrival count per registration, and at most one failure count per binding). (d) After a daemon restart, Events of the previous object are cleaned up, and the new object uses a different `gen`. `(gen, seq)` is unique within one SHM object, and uniqueness after restart is not claimed as a guarantee (the `instance_id` is recorded in the trace records) | integration test (injection + comparison) | IF-007, MOD-010, RTU-004, OBS-004 | P1 |
| **AC-008-16** | Mixed-workload interference | FCW (explicit connection used) and a non-SOME/IP app (explicit connection used) use the same contract. (a) When placed on different isolated CPUs, over-execution or runaway of one app does not change the other app's results by more than the agreed criteria of AC-008-02 and AC-008-07. (b) When placed on the same CPU with parameters that pass the re-verified admission formula, the same results as in (a) are obtained. (c) Adding a second L2 task on the same CPU to the default FCW parameters is rejected by the admission | generic event generator (two workloads driven simultaneously) + observation comparison | CBS-004, ORD-003, MOD-011, IF-001 | P1 |
| **AC-008-17** | CBS execution on CPUs without TT | On a node with no TT slots at all, and on a non-TT CPU of a mixed node: (a) the first Event is accepted, processed, and ended, and one Job record is created. (b) After being idle for at least $T_s$ following the last L2 activity, an Event that arrives starts within the agreed upper bound of TIM-011 + TIM-001 (TIM-011 in the reference poster configuration), and the start interval is given budget $C_s$ (DDR-008 D-04 rule 1). (c) After the task is throttled because its remaining budget reached 0 while it remained runnable, replenishment completes within the agreed upper bound of TIM-003 even if there is no other task activity on that CPU (backup path. The kick mechanism follows the result of the OI-9 PoC). (d) A deadline that passes while QUEUED or throttled is detected within the agreed upper bound of TIM-009. Throughout, there are 0 TT_START/TT_END events on that CPU | generic event generator + BPF trace + Job records | CBS-007, CBS-004, PRT-003, PRT-009 | P1 |
| **AC-008-18** | Preservation of retained L2 when the first TT task is added or the last TT task is removed | While repeating 10 table updates that add the first TT task on that CPU or remove the last TT task, with the L2 entry kept as is (the table after the update passes the admission): the remaining budget and replenishment window of the retained L2 are not reset, the `(gen, seq)` and $t_{dl}$ of open Events do not change, zero CANCELLED or ABANDONED are caused by the update, and there is no daemon restart. Because the TT slot set changes, reconfiguration of the Timer Master thread and an epoch update may occur, and this is not a reason for resetting L2 state (DDR-008 D-17(a), (h)) | integration test | RTU-001, CBS-007 | P1 |
| **AC-008-19** | Explicit failure for incorrect cpuset placement | For a worker whose allowed CPU set does not include the assigned CPU: an explicit error is recorded at the time of the binding attempt, the task is not switched to SCHED_EXT, the BPF scheduler continues to operate, and other L1/L2 tasks on the same node are not affected (L1 DMISS 0). The task is bound within the late identification bound (TIM-006, T_bind) from the time the cpuset is corrected. In this case, the measurement start point is not the thread creation time but the time of the cpuset correction | integration test with a deliberately wrong cpuset | CBS-008, CBS-005, DET-006 | P1 |

> The verification means of the acceptance criteria are based on the **generic event generator** (`evttask`, HLD-008 §8.5), which is transport-neutral. The FCW sample reproduces the same criteria with its own anomaly-injection options, and the option mapping is placed in HLD-008 §8.2. The concrete parameters and arithmetic of AC-008-16 are placed in HLD-008 §8.

---

## 6. Traceability to SRS-000

### 6.1 REQ → SRS-000 / DDR

| ID in this document | SRS-000 requirement ID | Related DDR | Notes |
|:--|:--|:--|:--|
| `REQ-008-MOD-001`~`011` | `REQ-FUN-001`, `REQ-FUN-002` | DDR-001, DDR-008 | Sporadic + SafetyCritical (L2) model, exclusion of Aperiodic, transport neutrality, Event/Job model, division of responsibility between CBS budget management and Event/Job management |
| `REQ-008-DET-001`~`007` | `REQ-NFR-003`, `REQ-NFR-004`, `REQ-FUN-009`(injection point) | DDR-002, DDR-005, DDR-008 | Execution-interval observation, transparency, explicit Event connection, explicit Job boundary, late identification |
| `REQ-008-CBS-001`, `004` | `REQ-FUN-013` | DDR-004, DDR-007, DDR-008 | Offline CBS budget allocation and admission (including the sufficient condition for deadline) |
| `REQ-008-CBS-002`, `003`, `005`, `006`, `007`, `008` | `REQ-FUN-004`, `REQ-NFR-002` | DDR-002, DDR-005, DDR-007, DDR-008 | CBS runtime enforcement based on TT Gap, partition pinning, FFI |
| `REQ-008-PRT-001`~`009` | `REQ-FUN-008`, `REQ-FUN-014`, `REQ-FUN-015` | DDR-005, DDR-007, DDR-012, DDR-008 | Throttling and replenishment, Event-based deadline miss determination and detection, fault classification and reporting, MIT violation counting |
| `REQ-008-ORD-001`~`003` | `REQ-FUN-013`, `REQ-NFR-002` | DDR-004, DDR-007, DDR-008 | Placement, multi-server ordering, inter-server isolation |
| `REQ-008-TIM-001`~`011` | `REQ-NFR-001`, `REQ-NFR-002` | DDR-008 | Quantitative bounds (candidate values). The pass/fail criteria of the test strategy are finalized after agreement |
| `REQ-008-IF-001`~`007` | `REQ-FUN-011` | DDR-003, DDR-006 | proto extension (MIT field, EVENT_DRIVEN layer, fault types; P2: per-task class, reason code), explicit connection API and Event queue contract (proposal) |
| `REQ-008-RTU-001`~`004` | `REQ-FUN-012`, `REQ-FUN-015` | DDR-011 | L2 runtime update |
| `REQ-008-OBS-001`~`004` | `REQ-FUN-014` | DDR-006 | Statistics and traces; mandatory statistics independent of tracing |
| `REQ-008-NFR-001`~`005` | `REQ-NFR-005`, `REQ-ENV-004` | DDR-006 | Capacity, RT path purity, no regression, portability, wire compatibility |
| (Phase 2) | `REQ-FUN-006`, `REQ-FUN-009`, `REQ-ED-001` | DDR-008 (later revision) | Event silence monitoring, middleware-boundary observation |

### 6.2 SRS-008 Correspondence of New and Revised IDs (finalized together with the SRS-008 v4.2 traceability matrix)

| ID in this document | SRS-008 correspondence | Notes |
|:--|:--|:--|
| MOD-009 | FR-L-010, FR-L-011, FR-B-016, TMG-009 | One independent Job, one acquire |
| MOD-010 | FR-L-009, FR-L-011, FR-L-012, FR-N-028, IF-L-004 | Identification and state, 1:1 |
| MOD-011 | FR-G-003, FR-L-003, FR-O-013 | Division of responsibility between CBS budget management and Event/Job management of the same L2 worker |
| DET-001 | FR-B-002 | Execution-interval observation |
| DET-004 | FR-L-001, FR-L-002, FR-L-008, FR-L-009, IF-L-001, IF-L-002, IF-L-003 | Post, Event wait, done, handle acquisition |
| DET-005 | FR-B-002, FR-B-004 | Blocking is not a Job boundary |
| DET-007 | FR-L-010, FR-L-011, FR-B-003, FR-L-001 | Explicit Job boundary |
| IF-005 | FR-L-004, IF-L-003 | Connection API |
| IF-007 | FR-L-005, FR-L-009, FR-L-010, FR-L-011, FR-L-012, FR-N-029, IF-L-002, IF-L-004 | Event queue contract |
| PRT-007 | FR-B-017, FR-N-012, FR-N-031, IF-PO-005, IF-ON-004 | Flag |
| PRT-008 | FR-L-009, FR-N-027, FR-B-017 | Deadline fixed at acceptance |
| PRT-009 | FR-N-027, FR-N-028, FR-N-029, FR-L-011, FR-N-031, TMG-014, TMG-015 | Detection and once-per-Job result, race rule |
| TIM-009 | TMG-014, FR-N-027 | Detection latency |
| TIM-010 | TMG-015 | Reporting latency |
| TIM-011 | TMG-016 | Delay from arrival time to runnable |
| OBS-004 | FR-N-030, FR-N-026, IF-V-001, IF-V-004 | Mandatory statistics |
| CBS-004 | FR-O-012, FR-O-013, FR-O-014 | Admission formula inputs (budget window, $L_{del}^{max}$, $\delta_{ovr}$) |
| CBS-007 | FR-O-022, FR-N-032, FR-B-025 | CBS execution on CPUs without TT |
| CBS-008 | FR-N-033 | Check of the allowed set of the assigned CPU |
| AC-008-17 | FR-O-022, FR-N-032, FR-B-025 | Verification of CBS execution on CPUs without TT |
| AC-008-18 | FR-N-005, FR-N-032 | L2 retention on TT addition and removal |
| AC-008-19 | FR-N-033 | Explicit failure of incorrect cpuset placement |

The correspondence of the remaining IDs is maintained as in the existing SRS-008 correspondence. The final confirmation of the correspondence rows is made together with the revision of SRS-008.

---

## 7. Out-of-Scope Items

| Item | Reason / Handling |
|:--|:--|
| Aperiodic workloads (no MIT) | DDR-001 §3: CBS cannot be applied. Pullpiri deploys them directly to L3/L4 (CFS) |
| `deadline_us > min_inter_arrival_us` (arbitrary deadline) | Requires Job queuing and tracking of multiple unfinished Jobs. Phase 2+ candidate |
| Inter-node event chains and end-to-end deadlines (pipeline/DAG) | SRS-000 `REQ-DD-001`. This document covers only from Event acceptance to Job end within a single node |
| Middleware-specific semantics (SOME/IP, DDS, etc.) | Principle 1. Responsibility of the app or adapter. TIMPANI provides only the explicit Event connection path |
| Obtaining an Event-based deadline without an explicit connection | Without an Event record, an Event-based deadline cannot be monitored (MOD-011). Events are not estimated from kernel observation |
| Multi-poster posting, large payloads and payload release, per-source SHM isolation | P2 (F-13, F-14). P1 has one posting context per source, inline payloads, and trusted SHM writers |
| Event silence (watchdog), TT floor dual mode | The inconsistency between DDR-002 §7 and DDR-007 §0 is decided in DDR-008. Watchdog is not implemented in P1 |
| NPU/GPU resources of L2 tasks | `REQ-AI-001` |
| QNX porting | `REQ-ENV-004`; design consideration only under NFR-004 |
| Changes to the Pullpiri-side YAML schema | TIMPANI defines only the proto contract. Separate work in the Pullpiri repository |
| IRQ/ISR, middleware internal threads, the entire network path | This feature handles only registered userspace L2 workers. Budget enforcement and delay of these paths are not automatically captured by TIMPANI (Issue #53 scope). Selection and management of scheduling policies for input adapters and threads of other apps are also excluded from P1, but placement and environment verification are maintained (DDR-008 D-24). |
| Meaning of the completion time `t_end` | It is the time at which the processing declared by the worker and the posting of the result are finished. It does not automatically mean completion of network delivery or device reflection (P1-008 contract amendment §2). |

---

## 8. Assumptions & Constraints

| ID | Content | Source |
|:--|:--|:--|
| A-01 | Linux 6.12+, `sched_ext`, PREEMPT_RT, cgroup v2 isolated partition | SRS-000 REQ-ENV-001~003 |
| A-02 | The TID of a registered L2 worker runs only on an isolated CPU, and L3/L4 are on non-isolated CPUs. The same placement and policy are not required for the entire L2 process. The responsibility for cgroup and cpuset placement and its review status follow A-09. | DDR-002 §5, DDR-008 D-24 |
| A-03 | An L2 task thread has the same comm name as `task_id` (the P1 identification means). According to the Linux comm limit, `task_id` must be 15 bytes or less and must be unique within a node. Identification based on cgroup/Pullpiri metadata is P2 according to SRS-000 `REQ-NFR-004` | DDR-005 User App Interface |
| **A-04** | **Withdrawn (v5).** The assumption that the activation time is used as an approximation of the Event arrival time is retired. The arrival time is recorded at acceptance by the explicit connection path (MOD-010, DET-004), and the deadline is determined as $t_{arr} + D$ (PRT-008). The activation-based deadline and the activation-based MIT check are also not used. The former content is recorded in the §0 v5 entry | §2.5, §2.6 |
| A-05 | `U_bound`, `U_overhead`, and `CBS_MIN_EXEC_US` use the candidate values of DDR-004/007 (0.80 / 0.02 / 100 µs) as they are, and their confirmation follows the open items of the respective DDR | DDR-004 §5, DDR-007 §11 |
| A-06 | **Not a correctness premise (v5).** Blocking inside a Job (mutex contention, blocking I/O, sleep) is allowed, is observed, and is included in the response time $R$. Because Job boundaries are determined by explicit acquire and done, blocking does not split a Job. However, the guarantee of the admission formula (CBS-004) applies only to Jobs that do not block. The deadline of a blocking Job is monitored but is not guaranteed by the admission. The sample app guide advises avoiding blocking inside a Job (HLD-008 §8) | §2.5, DET-005, CBS-004 |
| A-08 | The adapter posts immediately after arrival. If an earlier time is specified (a `t_arr` earlier than the post time), that delay uses part of $D$, and it is a delay outside the guarantee of the admission. This delay is included in the response time and recorded | CBS-004 (5) |
| **A-09** | It is assumed that a registered L2 worker receives timpani-n kernel BPF CBS enforcement by running as `SCHED_EXT` (DDR-008 D-24). The cgroup and cpuset placement of the worker and of receiving and middleware threads is the responsibility of the system (Pullpiri, deployment configuration). `sched_setaffinity` cannot move a thread outside `cpuset.cpus.effective`, and the same applies to root. The current L2 binding path does not perform cgroup movement or affinity changes, and it does not move unmanaged threads in bulk or raise them to `SCHED_FIFO`. The Linux conditions and target PoC for the goal of isolating only the worker, and the responsibility of N for TID movement and placement verification, are unapproved deployment review items of D-24. | DDR-003 §2, DDR-008 D-24 |
| **A-10** | In the deployment, the poster thread that calls post satisfies the **reference poster configuration**: from the recording of the arrival time until the return of the post call, it is not preempted by another thread. The candidate is a real-time policy thread pinned to an idle or dedicated CPU (proposal; finalized by measurement, OI-18). Because TIMPANI does not manage the policy of poster threads in P1 (DDR-008 D-24), this configuration is the responsibility of the app and the deployment. In a deployment that does not satisfy it, the upper bound of TIM-011 and the guarantee of the admission formula (CBS-004) do not hold. The app and deployment confirm this by measuring $L_{post}$ in the target configuration. | DDR-008 D-24, TIM-011, CBS-004 (2) |
| C-01 | The sched_ext slice expiry depends on the scheduler tick or HRTICK. The P1 value of TIM-004 assumes HZ=1000 | §4.6 |
| C-02 | Because futex wake cannot be called directly from BPF, a userspace helper thread (Replenisher) is needed. The deadline monitoring thread (DeadlineMonitor) is a candidate for a separate thread, and whether to merge it with the Replenisher is an open item (F-12) | DDR-007 §4.5 |
| C-03 | Changes to the proto interface and to the public API (libttsched) require **explicit approval (Project Leader)**, and only keeping field numbers and additions are allowed. Changes to the SHM object layout are the same | `.github/copilot-instructions.md`, NFR-005, OI-19 |
| C-04 | The current CI builds only Rust, so verification of the C++ path depends on local GTest and integration tests. Supplemented in the test strategy | §10 |
| C-08 | One posting context per source at a time. The poster is either a thread of the same process as the worker thread, or another process that can write to the shared SHM. The library uses no locks and does not detect violations. A violation can corrupt only the queue of that source. Multi-poster posting is P2 (F-13) | IF-007 |
| C-09 | Every process that can write to a shared SHM object is a trusted party. A misbehaving writer can corrupt any slot. Per-source isolation is P2 (F-14) | IF-007 |

> C-05~C-07 are already used as constraint numbers in SRS-008 §7, so they are not used in REQ-008. C-08 and C-09 are also in SRS-008 §7 with the same text.

---

## 9. Future Extension Roadmap (Phase 2+)

After P1 verification, the following items are elaborated in later revisions.

1. ~~**Classification of Job boundaries for tasks without an explicit connection** (DET-005, OI-1)~~: Resolved. It is replaced by the explicit Job boundaries (DET-007), and Job boundaries are not estimated without the explicit Event connection (MOD-011). Classification of blocking system calls is not required.
2. **EDF server ordering and admission based on window demand** (ORD-002, CBS-004 P2).
3. **Per-task time class** (IF-002), **reason codes for admission rejection** (IF-006).
4. **TT-undisturbed runtime update** (RTU-002/003): diff-based table application, epoch maintained on L2-only changes, incremental `ScheduleTableUpdate`.
5. **Middleware dispatch-boundary observation** (`REQ-FUN-009`, `REQ-ED-001`): precise monitoring of $t_{arr}$ based on the middleware reception time. The TIMPANI core remains middleware-neutral.
6. **Event silence (watchdog) monitoring and TT floor decision** (`REQ-FUN-006`), **wake-up latency exceeded fault** (`WAKEUP_LATENCY_EXCEED`).
7. **HRTICK-based budget precision** (TIM-004 P2).
8. **Identification based on cgroup/Pullpiri metadata** (SRS-000 `REQ-NFR-004`), **NodeStatus statistics reporting** (OBS-003).
9. **Arbitrary deadline (D > MIT)**, **pipeline/DAG activation** (`REQ-DD-001`).
10. **Event-based deadline monitoring wake-up** (F-10): judge by PoC whether event-based wake-up, instead of periodic scanning, is needed for the TIM-009 bound.
11. **Specify queue depth and payload size per source in the workload description** (F-11, proto approval required).
12. **Review of merging the DeadlineMonitor and Replenisher threads** (F-12).
13. **Multi-poster posting and large payloads (including release)** (F-13).
14. **Per-source SHM isolation, daemon liveness signal (heartbeat)** (F-14).

---

## 10. Gap Analysis vs `timpani26` HEAD `4201573`

> This section is the basis for deriving the requirements. Design decisions are covered in DDR-008, and implementation changes in HLD/LLD-008. The table below describes the **current code** at HEAD `4201573`. The design goals follow §1~§9.

| # | Area | Current implementation (basis) | Requirement | Gap | Phase |
|:--|:--|:--|:--|:--|:-:|
| 1 | Activation record | `select_cpu()` overwrites `activation_ns` on **every** TT/CBS wake-up (`timpani-n/src/bpf/timpani.bpf.c:256`) | MOD-003, DET-001, DET-005 | A wake-up after blocking is mistaken for a new activation. There is no means of recording the Event acceptance time or the Job boundaries | P1 |
| 2 | Completion detection | CBS DMISS determination is performed at every `stopping()`, with no distinction of completion (`!runnable`) (`:387-401`) | DET-002, PRT-002, PRT-009 | Determination occurs at preemption or slice expiry; the response time based on completion is not defined | P1 |
| 3 | Replenishment | Only Lazy is implemented (`cbs_lazy_replenish`). The Backup Replenisher thread (DDR-007 §4.5.3) is not implemented | PRT-003, TIM-003 | No upper bound on replenishment delay on CPUs without activity | P1 |
| 4 | CPU pinning | `DSQ_CBS` (101) / `DSQ_THROTTLED` (102) are global DSQs; `dispatch()` can consume on any CPU (`:313-328`) | CBS-005 | L2 can run on an unassigned isolated CPU → per-CPU Gap analysis is invalid | P1 |
| 5 | Admission | Checks only utilization (`0.80 − U_tt − 0.02`); `min_gap < 100 µs` is a warning only; D ≥ C and D ≤ MIT are not verified; TT blocking and tick overrun are not reflected (`timpani-o/src/global_scheduler.cpp:403-479`) | MOD-004, CBS-004, ORD-001 | Deadline guarantee is not possible for D < MIT | P1 (simple formula) / P2 (precise formula) |
| 6 | Server ordering | `DSQ_CBS` consumed in FIFO; ordering policy not defined | ORD-002 | Possible mismatch between the admission assumption and runtime behavior | P1 (FIFO stated explicitly) / P2 (EDF) |
| 7 | MIT violation | No MIT comparison anywhere in BPF or N. `libtimpani_rt` records only in its own SHM (not consumed by TIMPANI-N) | PRT-004/007 | Counting and reporting are absent | P1 |
| 8 | Fault type | BPF `FaultEvent.fault_type` ∈ {DMISS, BUDGET_EXCEED}; O→Pullpiri `schedinfo.FaultType` ∈ {UNKNOWN, DMISS} → BUDGET_EXCEED/WATCHDOG are lost as UNKNOWN (`orchestrator_service.cpp:77-85`) | PRT-005, IF-004 | Loss of type | P1 |
| 9 | Fault flood | `BUDGET_EXCEED` is emitted at **every stopping** with `remaining_us == 0`; FaultMonitor delivers non-DMISS faults unconditionally (`timpani.bpf.c:422`, `fault_monitor.cpp:112-115`) | PRT-006 | Fault flood toward O/Pullpiri in the throttled state | P1 |
| 10 | Identification | Single scan of `/proc` comm when the table is applied (`main.cpp:305-310`), no rescan. `TaskRegistry` is unused | DET-006, TIM-006 | Workers created late and restarted threads are not registered | P1 (rescan) / P2 (cgroup) |
| 11 | proto (Pullpiri→O) | `TaskInfo.period` reused as MIT; `TemporalClass` is per `SchedInfo` (workload) (`schedinfo.proto:45-92`) | IF-001/002 | Absence of an MIT-specific field (P1) and of a per-task class (P2) | P1 / P2 |
| 12 | proto (O→N) | `ExecutionModel.EVENT_DRIVEN` only declared; O always includes `cbs_entries` in the `TIME_TRIGGERED` layer (`global_scheduler.cpp:516`); `CbsConfig.period_us` doubles as the MIT; assigned CPU not specified | IF-003 | Absence of layer distinction, MIT, and CPU delivery | P1 |
| 13 | Runtime update | Every change republishes the full table and updates the epoch (+500 ms); `ScheduleTableUpdate`/`update_callback` not connected; `TimerMaster::set_schedule_table` blocks gRPC threads through thread join | RTU-002/003 | Adding an L2 workload triggers re-synchronization of all nodes and restart of TT threads | P2 |
| 14 | Observability | Jitter statistics to stdout only; `NodeStatus` not sent; wake-up latency is limited to the `CONFIG_TRACE_BPF_EVENT` build option and the gpdata file | OBS-001~004 | No input for the verification framework | P1 (trace and counters) / P2 (NodeStatus) |
| 15 | Connection API | `libttsched` is TT-only (only TT slots allocated in SHM); `libtimpani_rt` includes SOME/IP identifiers in the API; the `cbs_key` hash does not match BPF (`FNV("wl:task")` vs `FNV(wl)^FNV(task)`) | DET-004, IF-005, IF-007, MOD-006 | Absence of an official, neutral connection API for L2 and of an Event queue | P1 |
| 16 | Preceding bug | Mismatch between per-CPU and global hyperperiod (a slot repeats once per table HP); table `hyperperiod_us` is 0 for a node with no TT and with tasks (`global_scheduler.cpp` lines 109, 140, 493; a node without tasks has 10000); TT utilization `U_bound` not checked (`global_scheduler.cpp`) | CBS-001/004 premises | Reduced reliability of the Gap analysis. SRS-008 FR-O-015 is not met. Whether to change the expression is OI-25. A preceding fix is required | P1 |
| 17 | Design document mismatch | DDR-005 says "ttsched_wait_next_period is mandatory for L1/L2" vs DET-003 transparency; DDR-002 §7 TT floor vs DDR-007 not adopted | DET-003, §9-6 | Alignment update of DDR-002/005 is needed after the decision in DDR-008 | P1 (document) |

---

## 11. Open Items

| # | Issue | Impact | Decision owner / document | Phase |
|:--|:--|:--|:--|:-:|
| OI-1 | **Resolved (v5)**: replaced by explicit Job boundaries (DET-007). Job boundaries are not estimated without an explicit Event connection, and CBS budget management is maintained (MOD-011). See the withdrawal of OI-1 and OI-10 in SRS-008 | — | DDR-008 D-18 | P2 (replaced) |
| OI-2 | Design of the admission formula and BPF vtime when introducing EDF order (ORD-002 P2) | Admission formula, dispatch | DDR-008 follow-up | P2 |
| OI-3 | Need for an alternative policy for MIT violation handling (delay/discard) | Isolation vs delay | DDR-008 follow-up | P2 |
| OI-4 | CBS replenishment semantics — **Resolved (P1)**: decided in DDR-008 D-04 as the **replenishment window based on kernel activation**. The current "periodic grid-aligned replenishment" is rejected because it can throttle tasks that keep the MIT and cause them to miss deadlines | Admission formula, scope of BPF changes, interference bound under bursts | DDR-008 D-04 | P1 (resolved) |
| OI-5 | Measurement environment and procedure for finalizing the target values of TIM-001~011 (RPi5 basis) | Pass/fail criteria | Test strategy | P1 |
| OI-6 | Whether to adopt the kernel requirement (HRTICK) to achieve TIM-004 P2 | Deployment requirement | DDR-008 follow-up / SRS-000 REQ-ENV | P2 |
| OI-7 | Asynchronization of the fault reporting path (currently synchronous `NotifyFault` in the NodeStream reader thread) | TIM-010, node stream stability | DDR-012 revision | P2 |
| OI-8 | Backward-compatibility transition period for `period` reuse (IF-001) | Pullpiri integration | Coordination with Pullpiri | P1 |
| OI-9 | Availability PoC of the CPU reschedule-triggering mechanism for backup replenishment (`scx_bpf_kick_cpu` of the BPF syscall program vs wake-up of an auxiliary thread) | PRT-003 implementation | DDR-008 | P1 |
| OI-16 | Explicit declaration of multiple Event sources per task (proto `EventSourceInfo`, MIT per source) and derivation of CBS server parameters | Workload model, admission formula | DDR-008 follow-up | P2 |
| OI-17 | **Resolved (v5)**: the approach of identifying the marker wait point as Completion was replaced by **explicit Job boundaries** (DET-007). Boundaries are not estimated for tasks that do not use the explicit connection (OI-1 resolved) | — | DDR-008 D-18 | P2 (replaced) |
| OI-18 | **Agreement on numeric values**: TIM-009 detection latency (candidate 2 ms), TIM-010 reporting latency (candidate 200 ms), TIM-011 arrival → runnable (candidate 50 µs) (upper bound in the reference poster configuration) and $L_{del}^{max}$ (candidate 100 µs), TIM-004 $\delta_{ovr}$ (candidate 1 tick), queue depth (candidate 8) and payload upper bound (candidate 64 bytes), and the reference poster configuration (A-10); to be agreed and measured on the Linux target | Pass/fail criteria, admission formula inputs, queue size | Project Leader, test strategy | P1 |
| OI-19 | **Approval of proposals**: public API (connection API, IF-005), SHM v2 object layout (DDR-008 D-19, LLD-008), proto extension (D-11: addition of `FaultInfo` field, addition of `job_gen`, exposure of IF-007 queue constants) | Whether implementation can start. Before approval, it is not a finalized design | Project Leader | P1 |
| OI-20 | **Interpretation check 1**: defines the completion time `t_end` as the time just before the worker posts completion, and retries if it races with the monitoring mark (§2.5). Confirm whether reading "determination based on the actual completion time" in the original agreement this way is correct | PRT-009, AC-008-14(c) | Author, agreement reviewer | P1 |
| OI-21 | **Interpretation check 2**: the rule that Events closed by TIMPANI before the deadline (CANCELLED, ABANDONED) are not counted as failures (§2.5). The agreement addresses items discarded or cancelled by the worker before the deadline, and applying the same rule to TIMPANI close is an interpretation | PRT-009, AC-008-14(d) | Author, agreement reviewer | P1 |
| OI-22 | **DeadlineMonitor PoC**: measure the cost and jitter of the baseline periodic scan (1 ms candidate) on the target HW (including TT load and the selected CPU). The single-timer alternative with the earliest absolute deadline (F-10) is compared against new post notification and reset races and TIM-009, and whether to adopt it is decided after design review (details: DDR-008 D-20). The timer does not resolve the completion race OI-20. The possibility of merging the monitoring thread and the Replenisher is also judged (F-12) | TIM-009, C-02, thread configuration | DDR-008 | P1 |
| OI-23 | Measurement procedure for L2 `runtime` (C) and the declared margin ratio | Basis of the admission formula input; underestimation of budget | HLD-008 §8.2 (H-19), DDR-008 §3.4 | P1 |
| OI-24 | Re-post at the rebinding boundary: the `t_arr` basis passed by the adapter (original arrival time or re-post time) and the responsibility for preventing duplicate processing | Deadline basis, duplicate processing | HLD-008 §8.1 (H-20), DDR-008 §3.4 | P1 |
| OI-25 | Expression of `hyperperiod_us` for a table without TT (keeping the non-zero value of FR-O-015, or allowing 0 or absence) and the update boundary for first TT addition and last TT removal | Table validation, Timer Master, update contract | DDR-008 D-22, DDR-011 §10 (Phase 1), Weekly 2026-11-12 item 1-f | P1 |

> OI-10~OI-15 are managed in SRS-008 §9 (the numbering scheme is shared).

---

## 12. Relationship to Preceding Sample Apps (PR #50~#52) and Guide Direction

### 12.1 Current Status
- [eclipse-timpani/timpani26#50](https://github.com/eclipse-timpani/timpani26/pull/50) (FCW L1/L2 example + anomaly injection), [#51](https://github.com/eclipse-timpani/timpani26/pull/51) (timpani-n thread name identification), [#52](https://github.com/eclipse-timpani/timpani26/pull/52) (budget overrun injection, frame log) were written on the basis of **a past DDR-008 review draft that does not exist in the repository** ("DDR-008 §2.3", "DDR-008 LLD §1.3", "FR-041").
- That draft directly mentioned SOME/IP in the core contract, and as a result `libtimpani_rt` came to have an API that takes service/instance/event IDs as arguments, its own SHM (`/timpani_rt`), and library-side MIT and E2E fault determination (3 ms hard-coded). TIMPANI-N does not consume these, so they currently remain at the "library log" level.
- The thread name identification of PR #51 is consistent with this requirement (the premise of DET-006 P1), and it is kept.

### 12.2 Guide Direction (details in HLD-008 §8, LLD-008 §7)
The Phase 1 design must handle several kinds of Events in the same way, not just FCW. Therefore, the guide is provided in two layers. (a) The **application methodology** that places any Event kind on the common model (HLD-008 §8.1 contract, §8.2 procedure, §8.3 application table per Event kind). (b) The **file-by-file modification guide** that applies the methodology to the FCW example, and the expected observation per run (HLD-008 §8.4; code sketches are in LLD-008 §7.2). The table below summarizes the direction of (b).

| Item | Current (PR #50/52) | Guide direction (P1) |
|:--|:--|:--|
| Event registration | `timpani_evt_register(wl, task, svc, inst, evt, mit)` | Remove. TIMPANI knows L2 only through the Pullpiri `SchedInfo` (MIT field). Middleware subscription is handled by the app |
| Arrival marking | `timpani_evt_mark_arrival(svc, inst, evt, seq)` → own SHM | **Post** on the explicit connection path (DET-004(a), IF-005): pass the arrival time and payload from the receiving thread (vsomeip callback). If rejected, check the reason and treat it as a case in which no Event was created (counting is done by the connection path). The handle is obtained at initialization through source handle acquisition (DET-004(d)). The poster thread follows the reference poster configuration (A-10), and in the target configuration, confirm by measurement that $L_{post}$ is within the TIM-011 upper bound. |
| Worker wait | `std::condition_variable` wait | In the **Event wait (wait)** of the explicit connection path (DET-004(b)), confirm and record the start of the Job, take over one Event, and after processing post the completion or discard through the **explicit done** (DET-004(c)). The app does not keep its own queue. Even if the standard blocking wait is kept without the explicit Event connection, CBS budget management is maintained, but Event-based deadlines are not monitored |
| CBS bind/unbind | `timpani_cbs_bind_thread/unbind` (no scheduling effect) | Remove. The Job boundary is set by the Job start inside wait and by the explicit done, and arrival is set by the post |
| MIT and E2E determination | Inside the library (3 ms hard-coded) | Remove. TIMPANI-N determines and reports `MIT_VIOLATION`/`DMISS` (PRT-004/005/009). The app may keep only its own log |
| Server key hash | `FNV("wl:task")` | Hash not needed in the app (removed) |
| Workload registration | `mock_pullpiri.yaml` SPORADIC, `period`=MIT | Use the MIT-specific field (IF-001). `period` allowed during the transition period |
| Anomaly injection options | `--burst-ms`, `--overrun-ms`, `-be`, `--wakeup-delay-ms`, `--tt-overrun-ms` | Keep. The means of reproducing AC-008-03/04/07/14 in FCW (the verification means of the criteria is the generic event generator). However, the delay of `--wakeup-delay-ms` is moved from inside the worker to the receiving side (before posting). Sleep inside the worker is blocking within a Job and is outside the guarantee of the admission (A-06) |
| App queue and log | Queue where the worker and receiving thread take the same mutex; `std::cout` inside a Job | Since the common Event queue keeps the payload, the app does not keep its own queue. Per-frame console output of the worker is removed. No log ring is kept, and the correspondence between frames and Jobs is obtained from Job records (OBS-002) |
| Verification | App stdout log | Judged on the basis of TIMPANI-N's Job records and execution-interval records (OBS-002). The app log is auxiliary |
| Thread placement and policy | FCW example does not set affinity or policy; follows the container cpuset | A registered worker does not set its own policy or affinity, and follows `SCHED_EXT`/BPF CBS. Selection and management of non-worker policies are outside the P1 scope. cgroup and cpuset placement is the responsibility of the system, and the goal of isolating only the worker is under review (A-09). If the assigned CPU is not in the allowed set, binding explicitly fails (CBS-008, AC-008-19) |

### 12.3 Principles of Role Division
- **TIMPANI repository**: requirements and design confirmed → implementation of `timpani-o`/`timpani-n`/`libttsched` → publication of the Event kind application methodology and the sample app change guide (HLD-008 §8).
- **Sample app (FCW)**: reduced, following the guide, to an adapter (vsomeip ↔ explicit Event connection API). SOME/IP-specific code exists only inside `examples/someip`.
- The two tasks proceed in parallel **after the design is confirmed**, but the interfaces (connection API signatures, proto fields, Event queue constants) use the LLD-008 finalized version as the single reference. These three items are finalized only after Project Leader approval (OI-19).
