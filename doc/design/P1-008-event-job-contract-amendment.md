<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# Event-Driven Workload Support — P1 Contract Amendment Agreement

**Date:** 2026-11-11

**Status:** The P1 revision direction agreed with the user. The detailed design, API, and implementation method are subject to follow-up review.

**Basis:** Records the change requirements for the 008 design of `9209bc5`. For the reflection status in the canonical REQ/SRS/DDR/HLD/LLD, see the section "Status of Reflection in Formal Documents (2026-11-11)" at the bottom.

**Traceability:** [Feature support Issue #53](https://github.com/eclipse-timpani/timpani26/issues/53). The design Draft PR is for review, and merging the design alone does not mean that feature implementation and verification are complete, and it does not close the feature Issue.

## 1. Agreed Goal

> Each Event that the app explicitly connects gets one independent Job, and the deadline of that Event is set based on its arrival time. If the Event is not completed by the deadline, a deadline miss is detected regardless of whether the Job is executed, throttled, or completed. Event payload and identification and time information must stay consistently connected through notification, consumption, and recovery.

| Agreement | Behavior required in P1 |
|:--|:--|
| **One Event–one Job** | Each accepted Event is processed as an independent Job. Pending Events are not merged into one Job. Discard or cancellation before processing is recorded as a separate result, not as completion |
| **Detection when the deadline elapses** | Does not wait for the Job completion callback. Detects the deadline being exceeded even during queue waiting, execution, preemption, or execution hold due to budget exhaustion |
| **Common contract for queue, notification, and recovery** | Defines the rules for acceptance, rejection, overflow, re-registration, and restart so that the payload and the Event identifier and arrival time do not diverge |

The public API uses the names post, Event wait, and done (REQ-008 §2). wait waits until there is an Event to process. Within the same call, it confirms and records the Job start, hands over the Event, and returns. If a QUEUED Event already exists, it can return without blocking, and there is no separate public acquisition API. The internal CAS(compare-and-swap) is an atomic conditional update that updates only when the state word read and the current value are the same. `QUEUED → RUNNING` is the **Job-start state transition (confirmation of processing start)**, and it is not a copy, an additional queue, an API, or a scheduler operation. QUEUED/RUNNING is the logical lifecycle of the SHM Event record, and it does not mean membership of a BPF DSQ or whether the record is currently executing on a CPU. A worker that is processing the previous Job on a CPU and the next QUEUED Event can coexist, and RUNNING is kept until termination, even during preemption, throttling, or blocking.

## 2. Meaning of Times and Deadlines

| Notation | Meaning |
|:--|:--|
| Arrival time | The time when the app's input adapter recognizes one Event at the declared boundary. Distinguished from remote transmission and physical sensor occurrence time |
| Activation time | The time when the Job of the Event becomes runnable. If the worker is already running, there may be no new kernel Runnable transition |
| First execution·processing start | Distinguishes and connects the kernel's worker execution and the app's Job start. The actual recording means is subject to detailed design |
| Completion time | The time when the declared processing and result publication are finished. It does not automatically mean that network delivery or device reflection is complete |
| D | Relative deadline — relative deadline time. The elapsed time allowed from arrival to completion |

**The absolute deadline is the arrival time of that Event + D.** Even if acceptance, activation, replenishment, or resumption is delayed, the deadline is not moved back. The activation time + D is not used as a substitute deadline.

- If the completion time is less than or equal to the absolute deadline, the deadline is met.
- If the completion time is later than the absolute deadline, or if the Job is not completed by the time the deadline has passed, it is a deadline miss.
- Even when completion and timer processing race, the determination is based on the actual completion time. An on-time completion is not determined to be a failure only because its collection was late.
- A deadline-miss detection is identified once per Job. A later completion is linked as a separate completion record, and the failure is not double counted.
- An item explicitly discarded or cancelled before the deadline is not a successful completion. The conditions for ending deadline monitoring of that item and the policy-based discard result must be stated explicitly. A failure that has already occurred is not erased by a cancellation.

## 3. Explicit Connection Contract and Responsibilities

Common terms follow [REQ-008 §2](REQ-008-event-driven-workload-support.md#2-conceptual-model-common-eventjob-model) as the canonical source. **CBS budget management** and **Event/Job management** are complementary responsibilities of the same L2 worker, and they are not two apps, classes, or modes, or two parts of an app. Explicit Event connection is a means of Event/Job management and does not change the L2 classification of Accepted DDR-001. This organizes the responsibility expression by the decision of the Project Leader (author), and it does not change the optional connection, compatibility, or the P1/P2 scope.

The app decides the meaning of the Event and its receive boundary. TIMPANI does not automatically infer message meaning from any `recv()`, `read()`, or kernel wake-up.

| Actor | Required responsibility |
|:--|:--|
| Input adapter | Delivers by connecting the source, Event identifier·arrival time, and the target worker and payload item |
| Common Event delivery path | Confirms whether the Event is accepted, and ensures that the payload and metadata of the accepted Event are published consistently before consumption |
| Worker | Starts a Job that consumes one Event at a time, and explicitly completes it. Also passes the boundary of Jobs that continue without sleep |
| TIMPANI | Connection of source–worker–actual thread, Job identification·deadline management, CPU budget enforcement, execution-state observation, deadline-miss detection·reporting |

Identification of each Event/Job needs a means of distinguishing old information from new information, such as the sequence number within the source and the registration generation. The data structure and the actual fields are decided in the follow-up design.

An accepted Event is tracked as **waiting → processing → completed / explicitly discarded or cancelled**. A deadline miss is a separate result from the processing state. Whether to continue or cancel a failed Job is a separate policy, and failure detection does not mean automatic termination.

Even without the explicit Event connection, the CBS budget management, CPU pinning, and execution-interval observation of the same L2 worker are kept. However, the Event arrival time, the Job record, the MIT count, and the information needed for Event-based deadline monitoring cannot be obtained, and they are not estimated from BPF execution intervals. Therefore, **it is not described as equivalent to Event-by-Event arrival-based deadline support.** No additional classification or configuration flag is added. If the source has no Event records, there is no monitoring object, and an app is not reclassified because of a temporary silence in posting. The same admission formula applies to all L2 tasks, and when there are no Event records, D is an admission parameter, not a monitored Job deadline.

## 4. Design Required for Detection Without Waiting for Completion

- The absolute deadline of each Event/Job must be put under monitoring from the time of the first acceptance. A method that starts monitoring only after the worker starts is insufficient.
- Completion, discard, cancellation, and deadline-elapse handling must use the same identifier and generation to resolve races and duplicates.
- Detection must work even when the CPU is idle or the worker is throttled.
- The **allowed delay from deadline elapse to detection** and the **delay from detection to external reporting** must each be defined and measured. "When the deadline elapses" does not mean a guarantee of zero delay. The numbers are not yet agreed.
- The detection timer, the means of sharing information, and the real-time path cost are to be verified on the Linux target. The existing Replenisher is not assumed to already provide this function.
- An upper bound is set on the payload and metadata storage space, and an acceptance-rejection or explicit discard policy is defined for when it is full. An unbounded queue is not assumed.

## 5. Change Targets in Existing Documents

| Document | Part to change |
|:--|:--|
| [REQ-008](REQ-008-event-driven-workload-support.md) | R3/R4 continuous and batch consumption rules, Event/Job one-to-one contract, deadline-elapse detection, input connection premise |
| [SRS-008](SRS-008-event-driven-workload-support.md) | Per-Event arrival information·Job boundary·completion contract, deadline monitoring while waiting, detection latency·duplicates·recovery acceptance criteria |
| [DDR-008](DDR-008-event-driven-workload-support.md) | Decision to distinguish Jobs by kernel sleep alone, DMISS approximation based on activation, review of the structure that stores only the most recent arrival time |
| [HLD-008](HLD-008-event-driven-workload-support.md) | Connection of Event delivery, workers, and deadline monitoring, app application procedure, FCW and non-SOME/IP examples |
| [LLD-008](LLD-008-event-driven-workload-support.md) | Acceptance·posting protocol, Event/Job data structures, explicit boundary delivery, timer and completion races, generation, recovery, and upper-bound design |

Existing per-CPU budget enforcement, FIFO (First In, First Out — priority by activation order), and BPF-centered budget ownership are kept as the basis, but the budget replenishment and overload behavior when combined with the new Job boundary, and the admission formula, are revalidated. **Budget is not newly granted merely because a new Job is created.** Public API and protocol changes remain subject to separate review and approval.

## 6. Required Verification Scenarios

- Each normal Event creates an independent Job and a fixed arrival-based deadline.
- Even if Event B arrives while Job A is being processed, the deadline of A does not change to the time of B.
- Even if B is processed after A without sleeping, they are recorded and determined as different Jobs.
- Even if the worker fails to start execution or waits due to budget exhaustion, the deadline miss is detected within the allowed delay.
- Even if the completion notification is collected late, an actual on-time completion is not misjudged. In a completion–timer race, the failure is not counted twice.
- Races between queue posting, notification, and acceptance or rejection, and the restart boundary, do not process the payload of another Event.
- Timers, completions, and termination information from a previous registration do not change the state of the new registration.
- The SOME/IP FCW and a non-SOME/IP app use the same contract, and when one app runs away or over-executes, its interference with other apps is checked against the supplemented admission criteria.

## Status of Reflection in Formal Documents (2026-11-11)

This section records where the change targets in §5 above are reflected in the formal documents. It is a record of the design stage, and it does not mean that implementation or verification is complete. All numbers are candidate values. The agreed content of §1 to §6 does not change in this section.

| Document | Version | Reflection status | Where reflected |
|:--|:--|:--|:--|
| [REQ-008](REQ-008-event-driven-workload-support.md) | v5 | Contract reflected | §2.3~2.5 lifecycle·invariants·deadline rules, §4.1 MOD-009·010·011, §4.2 DET-007, §4.4 PRT-008·009, §4.6 TIM-009~011, §4.7 IF-007, §4.9 OBS-004, §5 AC-008-13~16. A-04 Withdrawn, A-06 accuracy premise removed (Change History v5) |
| [SRS-008](SRS-008-event-driven-workload-support.md) | v4 | Contract reflected | §3.10 FR-L-009~012, §3.5 FR-N-027~031, §3.7 FR-B-024, §3.0 FR-G-003, §4 TMG-014~016, §5.3 IF-L-004. Correspondence of new REQ IDs is in the Traceability Matrix §8.1·§8.2. Constraint C-08 (one posting context) and C-09 (the SHM writer is trusted) are reflected in SRS-008 §7, the Traceability Matrix (§8.1), and REQ-008 §8 |
| [DDR-008](DDR-008-event-driven-workload-support.md) | v2 | Contract reflected | D-01·02·03·04·05·09·10·11·14·16·17 revised; D-07 and D-08 supplemented only for terminology and thread alignment (D-02 BPF execution-interval observation, D-03 API·SHM, D-04 budget and Job, D-05 MIT, D-09 admission formula, D-10 fault, D-11 proto and `job_gen`, D-16 record, D-17 generation). New D-18 one Event–one Job, D-19 common Event queue, D-20 deadline detection, D-21 budget-only L2 task class. F-3·F-8 carry a withdrawal mark and a replacement-item instruction. Retired alternatives are recorded in the alternatives tables of D-03·D-19 and others |
| [HLD-008](HLD-008-event-driven-workload-support.md) | v5 | Contract reflected | §3.2 TIMPANI-N (DeadlineMonitor), §3.4 libttsched v2, §3.6 admission formula, §4.2 normal Event→Job, §4.4 consecutive Jobs during arrival, §4.7~4.8 deadline-miss detection, §4.9 queue overflow, §4.10 inactive·re-registration, §4.11 daemon restart, §6.2~6.5 Data View and proto proposal, §8 application method (§8.4 FCW, §8.5 non-SOME/IP example), §10 Open Items |
| [LLD-008](LLD-008-event-driven-workload-support.md) | v5 | Contract reflected | §2 proto proposal, §3.3 pipeline and admission, §4 BPF (execution-interval observation), §5.4 collector, §5.5 counting path·detail path, §5.6 SHM v2·activation·close·startup cleanup, §5.9 DeadlineMonitor, §6 libttsched v2 (API·sketch·interleaving·limitations·tests), §7 sample app, §8 timing, §9 failure modes, §10 acceptance criteria mapping, §11 WBS, §12 Open Items |

> **Note (2026-11-16):** The current canonical versions are REQ-008 v5.1, SRS-008 v4.1, DDR-008 v3.2, HLD-008 v5.2, and LLD-008 v5.1. D-22 to D-24 were added after this agreement (Weekly 2026-11-12) and do not change the contract in this document. Later changes are recorded in the change history of each document.

> **Note (2026-11-18):** On 2026-11-17, the public call names, the confirmation of the Job start within wait, and the division of responsibility between CBS budget management and Event/Job management were reflected in the body. This is a decision of the Project Leader (author). The formal documents are REQ-008 v5.2, SRS-008 v4.2, DDR-008 v3.3, HLD-008 v5.3, and LLD-008 v5.2. On the same day, the $L_{post}$ upper bound (TIM-011) was kept as a requirement that TIMPANI enforces in the reference poster configuration, and the deployment satisfying that configuration was set as an assumption (REQ-008 A-10).

> **Note (2026-11-26):** The poster and producer terms were cleaned up (the side that calls post is the poster, and the side that creates the input to be posted to an Event source is the producer). This is a decision of the Project Leader (author) and does not change the contract of this document. The formal documents are REQ-008 v5.3, SRS-008 v4.3, DDR-008 v3.4, HLD-008 v5.4, and LLD-008 v5.3.

### Remaining Open Items

- **Numeric bounds (candidate, not agreed):** Detection latency 2 ms or less, report latency 200 ms or less, $L_{post}$ 50 µs, $L_{del}^{max}$ 100 µs, `tol` 50 µs, `tol_mit` 0, $\delta_{ovr}$ 1 tick, QDEPTH 8, payload 64 B, DeadlineMonitor priority 97 (Replenisher 98), period 1 ms, wait back-off 10 ms, SHM candidate 64 slots or less 256 KiB (REQ §11 OI-18~OI-19).
- **Approval required:** Public API (libttsched v2 and the v1 ABI change), SHM v2 layout, protobuf fields (addition of `count`·`flags`·`job_gen` in `FaultInfo`). Changes to TIMPANI-N internal interfaces (passing of the binding generation in TaskRegistry, and `bind_gen` in the BPF TaskMeta and the termination record) are also subject to approval (LLD-008 L-12). Without approval, reporting carries only the count (DDR D-11, LLD §2, HLD §6.4).
- **Two interpretations to confirm:** (1) The completion time is the time that the worker read just before the termination transition, and in a race it is determined by re-reading, and it is determined by the recorded `t_end`. (2) A Job closed by TIMPANI before `t_dl` (CANCELLED, ABANDONED) is not a failure. Both interpretations are raised together in the DDR open items and the REQ Open Items.
- **Example configuration and policy decisions:** The CBS period change of the FCW example (20 ms → 18 ms), the parameters of the concurrent-run configuration (HLD-008 §8.5, H-15), the policy on whether to continue or cancel a Job that missed its deadline (DDR-008 D-20), and the handling of failures that are not reported under the threshold rule (`max_dmiss`).
- **PoC and Linux target measurement:** DeadlineMonitor scan cost and 1 kHz jitter (including TT load), the need for event-based wake-up (F-10), BPF rescheduling based on syscall, budget replenishment delay of idle CPUs, measurement of $L_{post}$·`tol`·$\delta_{ovr}$ (HZ and nohz_full), cost of posting·Event wait·termination, and concurrent run of FCW and non-SOME/IP apps and admission parameters (AC-008-16).