<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# DDR-008: Event-Driven Workload Support — Design Decision Record

**Date:** 2026-11-11
**Last modified:** 2026-11-26 (poster and producer term clean-up, final review corrections)
**Status:** Draft (Proposal, Phase 1 decisions pending confirmation)
**Version:** v3.4
**Author:** Human + AI
**Related requirements:** [REQ-008](REQ-008-event-driven-workload-support.md), [SRS-008](SRS-008-event-driven-workload-support.md), SRS-000 `REQ-FUN-004/009/013/014`, `REQ-NFR-003/004`, `REQ-ED-001`
**P1 contract amendment:** [P1-008](P1-008-event-job-contract-amendment.md), [Feature support Issue #53](https://github.com/eclipse-timpani/timpani26/issues/53)
**Related DDR:** [DDR-001](DDR-001-workload-model.md), [DDR-002](DDR-002-scheduling-architecture.md), [DDR-003](DDR-003-interface-protocol.md), [DDR-004](DDR-004-resource-allocation.md), [DDR-005](DDR-005-sched-ext-bpf-scheduler.md), [DDR-006](DDR-006-communication-architecture.md), [DDR-007](DDR-007-tt-cbs-integrated-scheduling.md), [DDR-011](DDR-011-runtime-table-update.md), [DDR-012](DDR-012-fault-management.md)
**Child documents:** HLD-008, LLD-008

> This document is the **canonical document** of `DDR-008`, which SRS-000 refers to. The earlier review draft (not in the repository, SOME/IP-specific) is retired. The "DDR-008 §2.3 / LLD §1.3 / FR-041" cited by PR #50 to #52 is superseded by this document.
> Each decision has a **D-NN** number and records its Phase (P1/P2), alternatives, and consequences. HLD and LLD reference D-NN and do not re-argue the decisions.
> Withdrawn items (F-3, F-8) keep their numbers and point to their replacements. Numbers are not reused.
> All designs in this document are proposals for review. They are not implemented or verified on a Linux target. The public API, SHM layout, protobuf fields, and numeric limits require Project Leader approval.

---

## Summary

- TIMPANI is designed to correspond to the **common Event/Job model** in REQ-008 §2 (Event source, Event, Job, lifecycle states and outcomes, rules R1 to R4, invariant I1). It is structured as three layers, **model / connection and observation / enforcement**. Model terms are defined independently of OS, scheduler, and library mechanisms (D-01).
- **One Event – one Job**: For each Event that the app's input adapter explicitly posts, one independent Job is created. The worker receives one Event with `ttsched_evt_wait()`, starts the Job, and always ends it with `ttsched_evt_done()`. Batch consumption (processing n pending items) and implicit completion at the next wait are not used (D-18).
- **Explicit Event connection** (libttsched v2 + SHM v2): It provides a fixed-size Event record ring and inline payloads (candidate: 8 entries, 64 B). On acceptance, it fixes the identifier `(gen, seq)` and the absolute deadline, and it defines the meanings of rejection, overflow, cancellation, re-registration, and restart. The poster is limited to one per source (D-03, D-19).
- **The deadline is the arrival time + D** (the absolute deadline). The new DeadlineMonitor of TIMPANI-N detects the passing of the deadline without waiting for completion, including during queue waits, execution, preemption, throttling, and CPU idle, and it counts each Job only once. It defines the completion time and the race rules with monitoring (D-20).
- BPF observes **execution intervals** (from kernel activation to voluntary sleep), and it is responsible only for CBS budget enforcement, CPU pinning, and TT protection. BPF does not define Jobs, and it does not make the MIT check or the L2 DMISS determination (D-02).
- CBS replenishment is an **activation-based replenishment window** (D-04). Property P-a states its preconditions, such as `T_s ≤ MIT`, `D ≤ MIT`, and no blocking. MIT violations are counted and reported on an arrival basis, and they do not reject the Event (D-05). The budget is granted per window, not per Job.
- The admission test is a utilization and **FIFO blocking sum sufficient condition**. It includes the delay `L_del_max` from arrival to the first dispatch and the tick overrun `δ_ovr`. One admission formula applies to all L2 tasks. CBS budget management and Event/Job management are complementary responsibilities of the same L2 worker. Without explicit Event connection, CBS budget management, CPU pinning, and execution-interval observation are still maintained, but they are not described as equivalent to per-Event deadline support (D-09, D-21).
- Faults: DMISS comes from the DeadlineMonitor, worker termination, and cleanup paths; BUDGET_EXCEED comes from BPF; and MIT_VIOLATION comes from slot counting. Reporting is divided into a lossless counting path and a best-effort detail path (D-10). Proto changes are limited to field additions, and every change is subject to explicit approval (D-11).
- P1 limitation: A Job that blocks has its deadline monitored, but the admission test does not guarantee it. If the poster dies, one `accepted` count may be lost. While the daemon is down, there is no deadline detection. The numbers are candidate values that require agreement and measurement.
- Status: Draft. Candidate numeric values, public API, SHM, and proto changes, and the DeadlineMonitor PoC are before approval and measurement, and they are neither implemented nor verified.

---

## 1. Context

### 1.1 Problem
As REQ-008 §1.3/§10 summarizes, the L2 (CBS) path exists end-to-end in `timpani26`, but the **behavior model** of Event-driven workloads (Event and Job definitions, completion and deadline criteria, replenishment determinism, CPU pinning, the sufficient condition for admission, fault classification) is undefined or does not satisfy the determinism requirements. In addition, the preceding PoC (`examples/someip/libtimpani_rt`) pulled SOME/IP into the core contract.

The 2026-11-10 draft (commit `9209bc5`) defined a Job by kernel sleep boundaries, and it used a batch consumption in which wait returns the number of pending items n, and a deadline based on activation time. In this structure, arrivals during processing and deadline overruns that are not completed cannot be distinguished at the Job level. The P1 contract amendment (P1-008, Issue #53) makes one Event one Job, fixes the deadline on the basis of the arrival time, and requires detection that does not wait for completion. This document reflects that agreement.

### 1.2 Forces

| Force | Description | Source |
|:--|:--|:--|
| **Determinism** | Every timing claim must have a measurable bound | SRS-000 REQ-NFR-001, REQ-008 TIM |
| **FFI** | Runaway or overrun in L2 must not propagate to L1 or to other L2 tasks | SRS-000 REQ-NFR-002 |
| **Transparency** | An app that does not use explicit connection must be able to use standard blocking as is (CBS budget management, CPU pinning, and execution-interval observation are maintained) | SRS-000 REQ-NFR-003, REQ-008 DET-003 |
| **Transport-layer neutrality** | No middleware semantics in the core | REQ-008 MOD-006 |
| **Event–Job 1:1 identification** | Each accepted Event must have its own independent Job and identifier | P1-008 §1 |
| **Deadline detection independent of completion** | The passing of the deadline must be detected even while queued, running, preempted, throttled, or while the CPU is idle | P1-008 §4 |
| **Data–identity consistency** | Data, identity, and time must not diverge across posting, notification, consumption, and restart | P1-008 §1 |
| **Simplicity (P1)** | Complete the FCW sample with realistic and simple basic functions | REQ-008 §0.1 |
| **Respect existing decisions** | Keep the models and mechanisms of DDR-001~007·011·012, with only the minimum necessary changes | This DDR §6 |
| **RT path purity** | No gRPC, allocation, or blocking on BPF/RT threads | DDR-006 §4 |
| **Kernel constraints** | futex wake is not possible in BPF, `bpf_timer` is unsuitable under PREEMPT_RT, and slice expiry depends on the tick | DDR-007 §4.5.1, REQ-008 C-01/02 |
| **proto governance** | Field numbers are kept and fields are only added, with explicit approval | `.github/copilot-instructions.md` |

---

## 2. Decisions

### D-01. Common Event/Job Model and Three-Layer Structure (Transport-Neutral) — P1

The common terms take [REQ-008 §2](REQ-008-event-driven-workload-support.md#2-conceptual-model-common-eventjob-model) as the canonical source. **CBS budget management** and **Event/Job management** are complementary responsibilities of the same L2 worker, and they are not app classifications or modes. Explicit Event connection is a means of Event/Job management, and it does not change the L2 classification of DDR-001.

**Decision**
1. **Fix the model first.** The common model in REQ-008 §2 is the only reference for TIMPANI. Model terms are independent of the OS, the scheduler, and libraries.
   - **Event source S**: The declared source of Events. In P1, there is one per L2 task, and it is identified by `task_id`. The Minimum Inter-arrival Time (MIT) is `TaskInfo.min_inter_arrival_us`. If it is absent, the period $T$ is used (D-11).
   - **Event e**: One arrival accepted at the app's input adapter boundary. It has the identifier `(gen, seq)`, the arrival time $t_{arr}$, the absolute deadline $t_{dl} = t_{arr} + D$ (computed once at acceptance and not changed afterward), and an opaque payload.
   - **Job j(e)**: Processing of one Event. There is no batching and no merging. A Job and an accepted Event are 1:1 (I1).
   - **Lifecycle states**: `QUEUED → RUNNING → COMPLETED | DISCARDED`, `QUEUED → CANCELLED`, `RUNNING → ABANDONED`. A terminal state is final. Only COMPLETED is a successful end. DISCARDED is a policy discard by the worker and is not a success. CANCELLED and ABANDONED are outcomes that TIMPANI closed.
   - **Deadline result** (`MISSED`) is a flag separate from the lifecycle. It is set at most once per Job and is never cleared (D-20).
   - **Times**: $t_{arr}$ (arrival), $t_{dl}$ (absolute deadline), $t_{begin}$ (the time at which the worker receives the Event and starts the Job), $t_{end}$ (the time read by the worker just before it posts the termination transition). A Job closed by TIMPANI has a closed time instead of $t_{end}$. The response time is $R = t_{end} - t_{arr}$, and the deadline is met when the state is COMPLETED and $t_{end} \le t_{dl}$.
   - **Rules**: R1: An arrival while waiting makes that Job immediately runnable. R2: An arrival while processing remains QUEUED and is not merged into the running Job. R3: When a Job ends and there is a QUEUED Event, the worker does not sleep and starts the next Job with the oldest Event (consecutive Jobs are separate Jobs). R4: A Job consumes only one Event, and the order is FIFO within the source.
   - **Invariant I1**: Each accepted Event reaches exactly one terminal state. A Job and an accepted Event are 1:1.
   - **A rejected post is not an Event.** E_INACTIVE, E_FULL, and E_INVAL create nothing. E_FULL is counted as overflow.
2. **The connection and observation layer is split into two paths.** (①) **Explicit Event connection**: the libttsched shared queue and SHM slots. It is the only path that provides Events, Jobs, and deadlines. (②) **Kernel execution-interval observation**: BPF. It observes the execution intervals and budgets of all L2 tasks but does not provide Events. Kernel observation is not a means of obtaining Events, so the two adapters are not regarded as alternatives of equal standing (P1-008 §1).
3. **Enforcement and determination layer.** BPF handles CBS budget, CPU pinning, and TT protection. TIMPANI-N (DeadlineMonitor, FaultMonitor) handles deadline monitoring and fault reporting. TIMPANI-O handles admission.
4. **Transport-neutral.** The proto, API, and data structures of the core (O/N/BPF/libttsched) contain no middleware identifiers or middleware headers. The input adapter that converts middleware reception into Event posting is placed on the app side. TIMPANI does not infer message semantics from arbitrary `recv()`, `read()`, or kernel wake-ups.
5. **P1 restriction.** There is one Event source per L2 task, and it is declared implicitly. No Event source message is added to the proto. `task_id` must be 15 bytes or fewer and must be unique within the same node, and TIMPANI-O verifies it (D-09 (4)). Multiple sources per task belong to P2 (F-7).

**Rationale**: The 9209bc5 draft defined an Event as a kernel wake-up and a Job boundary as a sleep. As a result, arrivals during processing, batch consumption, and per-Job deadline determination were not represented. If the model is separated from the implementation means, (a) arrivals and processing can be counted and identified separately, (b) the limits of kernel observation can be described as limits of the observation scope rather than model violations, and (c) the connection layer can be replaced without changing the enforcement code. Without explicit Event connection, CBS budget management, CPU pinning, and execution-interval observation are still maintained, but they are not described as equivalent to per-Event deadline support (D-21).

**Alternatives**
| Alternative | Advantages | Disadvantages | Verdict |
|:--|:--|:--|:--|
| A. The core holds a specific middleware contract (preceding PoC) | Immediate fit for FCW | Core changes for each middleware; TIMPANI learns message semantics | Rejected |
| B. A middleware plugin layer in TIMPANI-N | Precise observation per transport | Risk of middleware dependency and RT path contamination in N; exceeds the P1 scope | P2 review (observation-only plugin) |
| C. Common model + explicit connection + kernel execution-interval observation + app-side input adapter | Simple core; identification and deadlines are explicit; minimal certification scope | The app must implement an input adapter | **Adopted** |
| D. Event ≡ kernel wake-up, Job ≡ interval between sleeps (9209bc5) | Smallest implementation | Events and Jobs cannot be distinguished. Arrivals during processing and batch consumption cannot be represented. Deadline determination is tied to completion | Rejected |
| E. Treat a source obtained from kernel observation alone as an adapter equivalent to Events (retired 9209bc5 adapter K) | Supports standard blocking apps as is | No identifier or arrival time, and consecutive Jobs are merged | Rejected. CBS budget management is kept, but Event/Job management information is not inferred from kernel observation (D-21) |

**Consequences**: `libtimpani_rt`, `timpani_evt_register`, the library's own SHM `/timpani_rt`, and the library-side MIT and E2E determinations are to be retired (REQ-008 §12). The input adapter of the sample app shrinks to a thin layer that calls `ttsched_evt_post()` from the middleware callback. The workload declaration (proto) does not change except for the MIT field. The queue depth and payload limit are P1 build constants (D-19, F-11).

---

### D-02. Kernel Observation: BPF Execution-Interval Observer — P1

**Decision**: BPF maintains a state machine for each task's **execution interval**. An execution interval starts at kernel activation (waking from sleep and becoming runnable) and ends at voluntary sleep after the first execution. **An execution interval is not a Job.** Job boundaries are set only by the worker's explicit calls (D-03). Code identifiers can keep the `job_` prefix, but their meaning is an execution interval.

The states are {SLEEP, ACTIVE}. TaskMeta has `run_seq`, `t_act`, `t_start`, `bind_gen`, and flags. The transitions are as follows.

```text
SLEEP  --(ops.runnable: woken from sleep)--> ACTIVE
          · t_act = now, run_seq++, replenishment window check (D-04 rule 1), activation record once
ACTIVE --(ops.enqueue: reinsertion after preemption or slice expiry)--> ACTIVE (t_act unchanged)
          · If the remaining budget is 0, put it in the Throttled queue. This entry starts a throttle episode and raises BUDGET_EXCEED once (D-10)
ACTIVE --(ops.stopping: execution stops)--> ACTIVE
          · Budget charge. BUDGET_EXCEED is not raised here.
ACTIVE --(ops.quiescent: SCX_DEQ_SLEEP, voluntary sleep)--> SLEEP
          · Emit the execution-interval record (run_seq, t_act, t_start, t_sleep, budget_used_us, flags)
```

- The only trigger for activation is `ops.runnable` in the SLEEP state. `ops.enqueue` is also called on the reinsertion path, so it is not used for the activation decision. A dequeue that is not a sleep (such as an attribute change) does not close the execution interval.
- **Blocking and consecutive Jobs.** If a Job blocks, the kernel activation on wake-up is recorded as a new execution interval. That activation is handled inside the open replenishment window, so it does not receive new budget (D-04). The Job boundary does not change. Blocking is allowed and observed, and it is included in the response time. However, the guarantee of the admission test is limited to Jobs that do not block (D-09 assumption (4)). Consecutive Jobs that continue without sleep keep the execution interval going, and Job boundaries are set by the worker's done call and by the start of a Job inside the wait call.

The public calls are called post, Event wait (wait), and done (done) (REQ-008 §2). The wait call waits until there is an Event to process. Within the same call, it confirms and records the Job start, and then it hands over the Event and returns. If a QUEUED Event already exists, it can return without blocking. There is no separate public acquire API. The internal CAS (compare-and-swap) updates the state word only when it equals the value that was read, and it performs the **Job-start state transition (`QUEUED → RUNNING`)**, that is, the **confirmation of processing start**. It is not a data copy, an additional queue, or a scheduler operation. QUEUED and RUNNING are the logical lifecycle of the SHM Event record. They are not BPF DSQ membership or whether the Job is currently executing on a CPU. A worker processing a previous Job and the next QUEUED Event can coexist, and a started Job remains RUNNING until it ends, including during preemption, throttling, or blocking.
- **What P1 BPF does not do**: Job definition; the activation-based MIT check and emitting `MIT_VIOLATION`; L2 DMISS determination and early DMISS; system-call classification at sleep boundaries. Deadline monitoring is handled by TIMPANI-N (D-20). The DMISS path for TT slots (DDR-012) is not changed.

**Rationale**: The kernel cannot know Job boundaries. Consecutive Jobs, arrivals during processing, and blocking all appear in the kernel as the same sleep and wake-up pattern. However, budget enforcement needs only the execution interval. Therefore, BPF observes only execution intervals, and the worker explicitly defines Jobs.

**Alternatives**
| Alternative | Advantages | Disadvantages | Verdict |
|:--|:--|:--|:--|
| (i) Every wake-up = activation = Job (9209bc5) | Simple implementation | Consecutive Jobs and blocking are split incorrectly or merged | Rejected |
| (ii) A userspace monitor determines from shared memory only | No kernel change | Diverges from BPF enforcement; determination is not possible for apps without markers | Rejected |
| (iii) Hook library functions with uprobe/USDT | No kernel change | Operational burden of per-container attach; cost per call | Held as a P2 candidate |
| (iv) Identify sleep boundaries by system-call classification or a `sched_yield()` flag (9209bc5 P2 plan, earlier F-8) | Boundaries identified in the kernel | Cannot distinguish arrivals during processing from consecutive Jobs. The kernel ends up inferring app semantics | Withdrawn. Superseded by explicit Job boundaries (D-18) |

**Consequences**: The code that overwrites `activation_ns` on every wake-up in `select_cpu()` (`timpani.bpf.c:256`) is removed and replaced with the state machine. CBS budget is enforced per execution interval and is not newly granted for each Job (D-04). Execution-interval records are covered in D-16.

---

### D-03. Explicit Event Connection: libttsched v2 API and SHM v2 Slots — P1 (proposed change to the public API and SHM, requires Project Leader approval)

**Decision**: Extend the existing `libttsched` (SHM object, task slots, futex) to v2. Apps make Event-level connections with `ttsched_evt_open`, `ttsched_evt_post`, `ttsched_evt_wait`, and `ttsched_evt_done`. The signatures below are **a proposal, subject to approval**. LLD-008 defines the detailed structures and error codes.

```c
ttsched_evt_t ttsched_evt_open(const char *task_id);          /* initialization path, no heap */
int ttsched_evt_post(ttsched_evt_t src, uint64_t t_arr_ns,     /* called by the input adapter */
                     const void *payload, uint32_t len,
                     struct ttsched_evt_id *id_out);
int ttsched_evt_wait(struct ttsched_evt_event *ev);             /* worker: one Event, start Job */
int ttsched_evt_done(int outcome);                              /* worker: end Job, required */
```

- **open**: Holds a handle in a fixed table inside the process. The handle points to `task_id` and remains valid after `ttsched_init()`. The mapping is done on the initialization path.
- **post**: 0 means accepted. The Event exists, has the identifier `(gen, seq)` and the deadline $t_{arr} + D$, and becomes subject to monitoring. A negative value means nothing was created. If `t_arr_ns == 0`, the CLOCK_MONOTONIC at the time of the call is used. If a value is given, it must be CLOCK_MONOTONIC, and the call returns E_INVAL if the value is a future time or earlier than the previous arrival of the source. A value earlier than the current time is accepted as is; that delay is included in the response time but not in the guarantee of the admission test.
- **wait**: If an open Job exists, the result is E_STATE. Otherwise it receives one Event and starts a Job. If the result is E_INACTIVE, it calls `ttsched_init()` after a short back-off (candidate: 10 ms) and retries.
- **done**: Required for each Job. The outcome is COMPLETED or DISCARDED. **There is no implicit completion.** `done` first reads `word` and checks the `seq` and state of the open Job. If `seq` differs, or TIMPANI has already closed the Job (ABANDONED), it returns E_INACTIVE and clears the open flag (A6).
- **Worker loop**: `for (;;) { if (ttsched_evt_wait(&ev) < 0) { ttsched_init(); continue; } process(&ev); ttsched_evt_done(TTSCHED_EVT_COMPLETED); }`
- **Thread rule**: There is one thread per source that calls wait and done. The name of that thread is the `task_id` (1 to 15 bytes). If a second thread makes the call, it is a contract violation. The worker's open Job state and Event copy are kept in thread-local storage.
- **Poster**: For each source, there is one poster at a time (D-19). The poster can be a thread of the worker process, or another process on the same host that has SHM write permission (OI-15 decision retained).
- **Cost bound**: The number of atomic operations is bounded for post, non-blocking wait, and done (the counts are given in LLD-008 §6 and are candidates until measured). memcpy is at most the payload limit, and there is at most one futex system call. There is no heap, no lock, and no blocking. Mapping and remapping are done on the non-RT path by `ttsched_evt_open()` and `ttsched_init()`. If the mapping is not usable, post and wait return E_INACTIVE and do not remap by themselves.
- **Trust scope (P1)**: Every process that can write to SHM is trusted. An incorrect write can corrupt any slot. Permissions remain `fchmod` 0666 as decided in 9209bc5. Per-object isolation is P2 (F-14).

**SHM v2 slots (proposal, approval required)**
- The header keeps `{magic, version, n_tasks, generation, instance_id, reserved}`. It adds `version` and `instance_id` and updates the `magic` value, stating the ABI break explicitly. It increases `TIMPANI_MAX_TASKS` from 32 to 64 (candidate; LLD fixes the size, alignment, and object limit).
- The fixed part of a slot has `name`, `counter` (futex word for waiting and wake-up), `kind` (NONE, TT, EVT), configuration `mit_us` and `deadline_us`, `gen`, ring indices `head` and `tail`, and arrival records `last_arr_ns` and `last_viol_ns`. `gen` is a u64. Its upper 32 bits come from `instance_id`, and its lower 32 bits come from the binding generation number, which increases with each activation.
- **Cumulative counters (mandatory statistics, unrelated to traces)**: `arrivals`, `accepted`, `overflow`, `revoked`, `started`, `completed`, `discarded`, `cancelled`, `abandoned`, `missed`, `mit_viol`, and `resp_max_ns`, which only the worker writes. They use atomic addition and accumulate over the object's lifetime. TIMPANI-N sets the baseline value at activation time.
- `counter` is a word used to wake waiters. It is separated from Event counting, so that changing `counter` to wake waiters on deactivation or daemon exit does not change the Event counts.
- Slot convention: Once a `task_id` is assigned, it uses the same slot for the lifetime of the object. Deactivation sets `kind = NONE`, and the name and cumulative values are kept. Reactivation uses the same slot again. `kind`, `gen`, the record `word`, `head`, `tail`, and `counter` are read and written with sequentially consistent atomic operations. TT slots keep using the existing `magic`-last and `generation` handshake as is.
- Division of write responsibilities: The poster writes the record fields, the publication of the record `word`, `tail`, the arrival count, and the increment and wake-up of `counter`. The worker writes the QUEUED-to-RUNNING transition, `t_begin`, the termination transition and `t_end`, the termination counts, and the Job log. DeadlineMonitor writes the MISSED flag (CAS) and `missed`. TimerMaster writes `kind`, `name`, configuration, `gen`, the `last_arr_ns` initialization at activation, the transition that TIMPANI closes, and the corresponding counts. The detailed interleaving argument is covered by LLD-008.

**Alternatives**
| Alternative | Advantages | Disadvantages | Verdict |
|:--|:--|:--|:--|
| (i) App-owned queue + batch count n + invalidate/clear + `TTSCHED_EVT_RESET` + SpscRing (9209bc5) | Familiar data structure | The notification SHM and the app queue must be kept in step. Contract for processing n entries. A RESET protocol is needed on each restart | Rejected |
| (ii) Call only `ttsched_evt_notify(src, t_arr)` and the app manages the payload | Short call | The notification and payload can diverge. No identifier | Rejected |
| (iii) Implicit completion: the previous Job is treated as completed at the next wait | Simple app code | A failed Job can be recorded as completed | Rejected. `done` is required |
| (iv) The app updates the pinned BPF map directly (alternative in the DDR-008 draft) | Reuses the kernel path | Depends on permissions and mounting | Rejected (existing decision retained) |
| (v) A separate library `libttevent` | Separation of roles | Duplicated ABI and SHM | Rejected |
| (vi) Notification based on eventfd | Standard fd | Requires passing the fd; higher latency than futex | Rejected |
| (vii) Name-based signal and wait with no return value (earlier draft) | Simple calls | Cannot represent arrivals during processing. Name lookup on every call | Rejected |

**Consequences**: Consumers of `libttsched.h` (timpani-n, sample-apps, external TT apps) need to be recompiled (ABI break). `timpani_rt_shm.h` and `/timpani_rt` are retired. The OI-15 decision (scope of the poster) is kept, and reducing write permissions and inter-process policy are P2.

---

### D-04. CBS Replenishment Semantics: Activation-Based Replenishment Window + Mandatory Backup Replenishment — P1

**Decision**: Server $s$ has the remaining budget `remaining` and the end of the current replenishment window `replenish_at`. The current grid-aligned lazy replenishment (`replenish_at_ns += k·T_s`) is discarded, and the following rules are used. $T_s$ is the declared CBS period `period` of the server. It is not the same value as the Minimum Inter-arrival Time (MIT) (`min_inter_arrival_us`, or the period if absent). Property (P-a) assumes $T_s \le MIT$ (A15).

1. **On activation** (SLEEP→ACTIVE, D-02): If $t_{act} + tol \ge replenish\_at$, set `remaining = C_s` and `replenish_at = t_act + T_s`. Otherwise (the previous window has not yet ended), leave the budget and window unchanged, and leave a **budget carry-over flag** (`CARRYOVER`) in the execution interval.
2. **Enforcement after exhaustion**: The charge saturates the remaining budget at 0. If the remaining budget is 0 and the task remains runnable after the charge, or if the remaining budget is still 0 after rule 1 is applied on wake-up, the task is placed in the Throttled queue before further execution, and the current replenishment time is kept. If the budget is used up exactly and the task then sleeps, no throttle episode starts. `done()` and the logical Job state are not used for this determination.
3. **On promotion**: When $now \ge replenish\_at$, a Throttled server is updated to `remaining = C_s` and `replenish_at = now + T_s` on the dispatch path of the assigned CPU, and it enters the **back** of the L2 queue (D-08).
4. A server that is active and not Throttled does not refill its budget when the window ends. The next replenishment happens only under rule 1 or rule 3.
5. `tol` (`CBS_ACT_TOL_US`) means **only the tolerance of rule 1**. Because of variation in wake-up delay, the case where the interval between two consecutive activations is observed to be slightly short must be absorbed, so it must be at least the upper bound of activation jitter (candidate: 50 µs = `L_post`, measurement required). `tol` is not shared with the arrival-based MIT check, and it is not tied to $L_{wake}^{max}$ of the admission formula. The arrival-based MIT check uses its own tolerance `tol_mit` (candidate: 0; the arrival time does not receive wake-up jitter) (D-05).
6. While the server is running, only **BPF** writes `remaining`, `replenish_at`, and the Throttled state, under the run-queue lock of the assigned CPU. Userspace writes initial values only when it creates the server (before the task enters `SCHED_EXT`), and reads only after that (D-07, D-17).

**The budget is attached to the window, not to the Job.** A window opens only under rule 1 (activation after the window has ended) and rule 3 (promotion). A new Job does not by itself grant budget. A Job started inside an open window (a consecutive Job, or a resumption after blocking) shares the remaining budget.

The **Replenisher thread (Backup)** in DDR-007 §4.5.3 is **elevated to a mandatory component** (D-07). Its role is to ensure that rule 3 runs on time when the assigned CPU is idle.

**Properties**
- **(P-a) A server that keeps its declaration is not throttled.** If all of the following preconditions hold, rule 1 gives the full $C_s$ to every Job.
  (i) Every arrival respects the MIT, and $T_s \le MIT$. (ii) $D \le MIT$. (iii) The charged execution time from activation to the actual sleep (including the cost of entering the wait) is at most $C_s$. (iv) The Job does not block. (v) Each Job ends before the next arrival, so the worker is asleep.
  In this case, the previous window ends at $t_{prev} + T_s$, and the next activation satisfies $t_{act} \ge t_{prev} + T_s - tol$. By induction, the server is not throttled.
  **Boundary condition**: If a Job ends so late that the worker is not yet asleep before the next arrival (possible only when the response time is close to the MIT), the next Job starts without a kernel activation and runs with the remaining window budget. This window ends within the delay of the previous activation, and promotion then refills the budget. The additional delay is small and bounded, but `BUDGET_EXCEED` can occur. Therefore, leaving a margin between $D$ and the MIT is recommended.
- **(P-b) Per-window consumption bound.** Within one replenishment window, the server's consumption is at most $C_s$ (plus the overrun due to tick resolution, SRS-008 TMG-004). Because the start interval of windows is at least $T_s - tol$, the consumption over any interval of length $L$ is at most $(\lceil L/(T_s - tol) \rceil + 1) \cdot (C_s + \delta_{ovr})$ ($\delta_{ovr}$ is the bound of that overrun, D-09). This bound holds whether or not the server violates its assumptions.
- **(P-c) The amount that other servers run ahead is at most $C_j + \delta_{ovr}$ per server.** If server $j$ is ahead in the queue or is running at the activation time of Job $i$, the remaining budget of $j$ is at most $C_j$. The budget of $j$ is refilled only in two cases, activation under rule 1 and promotion under rule 3, and both send $j$ behind $i$ (D-08). Consecutive Jobs do not receive new budget unless they are throttled (rule 4). Therefore, the amount that $j$ uses ahead of $i$ does not exceed the budget that remained at the activation time of $i$ plus the tick overrun $\delta_{ovr}$. The $\sum_{j \ne i} (C_j + \delta_{ovr})$ term of D-09 is based on this property. This property holds even when $j$ violates the MIT or exceeds its budget.

**Behavior after a violation (the cost of a violation is borne by the violating task)**: If the task remains runnable after exhausting its budget at an MIT-violating activation, it is throttled until the end of the window and then runs with **the budget of the next window** (rule 3). A later activation that falls inside an open window does not receive new budget even if its interval is at least the MIT (rule 1, `CARRYOVER`), and it runs with the remaining budget. If the budget is insufficient, it waits until the end of the window. The following example is written on a window basis. Each Job is assumed to use the full budget of 2 ms.

```text
T_s = 18 ms, C_s = 2 ms, MIT = 18 ms (T_s <= MIT).
t= 0.0  J1 arrives, activation → window W1 = [0, 18), budget 2 ms. J1 runs 0~2
t=15.0  J2 arrives (interval from J1 arrival is 15 ms, MIT violation). At activation, W1 has not ended
        → rule 1 not met, budget stays at 0, throttled
t=18.0  Promotion (rule 3): budget 2 ms, window W2 = [18, 36). J2 runs 18~20 (J2 response time 5 ms)
t=33.0  J3 arrives (interval from J2 arrival is 18 ms, MIT respected). 33.0 is inside W2
        → rule 1 not met, CARRYOVER, remaining budget 0, throttled until 36.0
t=36.0  Promotion: window W3 = [36, 54). J3 runs 36~38 (J3 response time 5 ms, arrival-based delay 3 ms)
```

Inside a window, the budget is not refilled. The remaining budget decreases only by the amount that the Job uses. After the window ends, the server receives the full $C_s$ at its first activation (rule 1) or promotion (rule 3) and realigns with the arrival phase (returns to the state of P-a). The time to return depends on the arrival interval and the window phase, and the delay is a measurement item. $C_s$ is declared with margin over the actual execution time (HLD-008 §8). A Job that misses its deadline in this interval is reported by DeadlineMonitor together with `THROTTLED`, and `CARRYOVER` remains in the execution-interval record so that it is distinguished from an execution defect (D-10). In either case, the guarantees for other servers and for L1 ((P-b), (P-c)) are maintained.

**Reason for rejecting grid-aligned replenishment (counterexample)**: Suppose there is a server with $T_s$ = 18 ms and $C_s$ = 2 ms, and a TT block that occupies [16, 18) ms and [36, 38) ms on the same CPU. The grid replenishment times are 0, 18, and 36 ms. Event A arrives at 16 ms with deadline 21 ms, and Event B arrives at 34 ms with deadline 39 ms (D = 5 ms).

```text
t=16.0  Event A arrives → waits until 18.0 because of the TT block
t=18.0  Grid replenishment (remaining = 2 ms). A runs 18.0~20.0 → uses the budget of the [18, 36) period
t=34.0  Event B arrives (18 ms after A: MIT respected). remaining = 0 → Throttled
t=36.0  Grid replenishment. However, TT block [36, 38)
t=38.0  B runs 38.0~40.0 → completes at 40.0 > deadline 39.0 (DMISS)
```

This configuration passes the admission formula (D-09). That is, under grid alignment, a task that keeps its declaration can be throttled and miss a deadline that the admission formula guaranteed. In the activation-based window, the window of A is [16, 34), so B receives the full budget at 34.0 and finishes before 36.0.

**Alternatives**
| Alternative | Verdict |
|:--|:--|
| Periodic grid-aligned lazy replenishment (current `cbs_lazy_replenish`) | Rejected by the counterexample above. The BPF change is the smallest but not sound |
| A full sporadic server that records a replenishment time for each consumption fragment | A per-server replenishment list is needed, which makes BPF complex. Excessive for a single-threaded server. Rejected |
| Deadline-based CBS (defers the server deadline when the budget is exhausted, EDF order) | Reviewed in P2 together with EDF in D-08 |
| Align the promoted window to the Job's activation time instead of the promotion time (`replenish_at = t_act + T_s`) | Starting from the Job right after the violation, the phase realigns to the arrival, so the return is faster. However, a server that repeats violations gets $2C_s$ every $T_s$, which breaks the per-window bound (P-b). The guarantee to other servers (P-c) is maintained, so it is re-examined in P2 after measurement (F-9) |
| Reject or discard the violating Job | An accepted Event has a Job (I1). Rejection loses an actual arrival, and discard is possible only as a worker policy result. Rejected (D-05) |

**Consequences**: `replenish_at` moves according to the activation of each server, not to a grid. The Replenisher reads the per-server time from BPF and follows it (D-07). The "periodic grid" description in DDR-007 §4.5 is a target for alignment update (F-2). REQ-008 OI-4 is resolved by this decision. The design assumptions now include $T_s \le MIT$ and no blocking, and the return time after a violation is a measurement item.

---

### D-05. MIT: Arrival-Based Counting and Reporting, No Rejection — P1

**Decision**: The MIT is an **arrival** constraint of the Event source (REQ-008 §2.6). Counting is done at the arrival boundary, that is, on the posting path. No activation-based MIT check is kept. The MIT check and the emission of `MIT_VIOLATION` are removed from BPF.

- **Check**: In the arrival step of the posting protocol, if $t_{arr} - last\_arr\_ns < MIT - tol_{mit}$, the slot's `mit_viol` is incremented and `last_viol_ns = t_arr` is recorded. Only the poster writes this value (D-19).
- **What is counted**: Every post that reaches the MIT check is counted as an arrival. Accepted posts, posts rejected for overflow, and posts that were returned are all included here. A post rejected at the first step because the source is inactive is not an arrival.
- **Violations are also accepted**: An arrival that violates the MIT is also **accepted, queued, and given a Job.** Violations are not rejected. Violations are counted and reported.
- **Report**: The EventSourceMonitor of TIMPANI-N compares the increase in `mit_viol` with the baseline value at activation time and creates `MIT_VIOLATION`. It is one entry per $T_s$ window per source, and it includes the number of violations (D-10).
- **Enforcement**: Enforcement looks only at the window state, regardless of whether there was an MIT violation (D-04). After a violation, a Job can be throttled or receive `CARRYOVER` according to the window rules, and this is reported.
- **MIT-related flag**: If the last violation time `last_viol_ns` is after $t_{arr} - T_s$ of a Job that missed its deadline, the `MIT_RELATED` flag is attached. FaultMonitor computes the flag by reading `last_viol_ns` (D-10).
- **Tolerance**: `tol_mit` (candidate: 0) is used only for the arrival-based check. Because the arrival time does not receive wake-up jitter, it is separated from `tol` (D-04 rule 5).

**Alternatives**
| Alternative | Verdict |
|:--|:--|
| (i) Activation-based MIT check performed in BPF (9209bc5 adapter K) | Possible with the kernel alone, but it counts arrivals during processing and blocking fragments as false violations. Wake-up jitter is observed as violations. Rejected |
| (ii) Run arrival-based and activation-based checks in parallel and switch the source (marker-activated switch, earlier D-05) | Source switching and duplicate-counting prevention logic is complex. Rejected |
| (iii) Reject arrivals that violate the MIT | Loses arrivals that the app actually generated. Rejected |
| (iv) Discard (drop) violating arrivals | An accepted Event has a Job (I1). Discard is possible only as a worker policy result. Rejected |
| (v) Delay until the replenishment time (force enqueue to THROTTLED) | Prevents use of normal remaining budget, so it only increases delay. Rejected |

**Consequences**: The BPF `mit_viol_count`, the MIT use of `TaskMeta.last_act_ns`, and the emission of `FAULT_MIT_VIOLATION` are removed. MIT counting is unified into a single SHM cumulative counter. The cost of the arrival-based check is a few atomic operations on the posting path.

---

### D-06. Assigned-CPU Pinning: Per-CPU L2 DSQ — P1

**Decision**: Replace the global `DSQ_CBS(101)`/`DSQ_THROTTLED(102)` with **per-CPU DSQs**: `DSQ_CBS_BASE + cpu` and `DSQ_THROTTLED_BASE + cpu` (for example, `0x100 + cpu` and `0x200 + cpu`; created for each CPU in the isolated mask in `init()`). `ops.enqueue` inserts only into the L2 DSQ of `meta->assigned_cpu`, and `ops.dispatch(cpu)` consumes only the L2 DSQ of its own CPU. `ops.select_cpu` always returns `assigned_cpu`. `sched_setaffinity` is not used (the current principle is retained; the CPU must be within the CPUs allowed by cpuset).

**Alternatives**: (i) Derive the placement from `select_cpu` alone (current) — a global DSQ consume allows execution on another CPU; rejected. (ii) Hard pinning with `sched_setaffinity` — impairs app transparency and may conflict with the cgroup cpuset; rejected.

**Consequences**: `promote_throttled_if_replenished` also iterates per CPU. The scan limit `PROMOTE_BUDGET` (16) equals the per-CPU upper limit of L2 tasks, `MAX_L2_PER_CPU` (16, D-09), so a server whose window has ended is not excluded from the scan. `CbsConfig` needs an assigned-CPU field (D-11).

---

### D-07. Backup Replenishment (Replenisher) Thread and Means of Triggering Rescheduling — P1

**Decision**: Add a `Replenisher` thread (SCHED_FIFO, non-isolated CPU) to timpani-n. Its priority is set to TimerMaster − 1 (candidate: 98) and is fixed in the PoC. The Replenisher **does not write server state.** Its only job is to make the assigned CPU pass through the dispatch path again when the window of a Throttled server has ended. Budget refill and promotion are done by BPF under the run-queue lock of that CPU (D-04 rules 3 and 6). In this way, userspace writes do not cause lost updates that overwrite a budget charge in progress on another CPU.

**Operation**: For each server, the "next check time" is managed in a min-heap, and the thread waits until the absolute time on `CLOCK_MONOTONIC`. When it wakes up:
1. Run the BPF `BPF_PROG_TYPE_SYSCALL` program `timpani_replenish_kick` with `bpf_prog_test_run_opts()` (arguments: the `cbs_map` key of the server and the assigned CPU). The program **reads** `cbs_map`, and it calls `scx_bpf_kick_cpu(cpu, 0)` only when the server is Throttled, `now ≥ replenish_at`, and the TT slot on that CPU is not active. It returns the server's `(throttled, remaining, replenish_at)`. If the TT slot is active, it does not kick. Here, "active" is the determination of the current `tt_active_on_cpu`, that is, there is a runnable TT task on that CPU (not the slot interval in the schedule table). During that time, dispatch does not dispatch L2 tasks. When the TT task sleeps, the CPU picks the next task and dispatch is called, and promotion happens at that time.
2. Next check time: If the kick program could not be run (a transient error), or if the server is still Throttled and its window has already ended (promotion has not yet happened), `now + 1 ms` (`REPLENISH_RETRY`). If the returned `replenish_at` is in the future, that time. Otherwise, `now + T_s`.

Because `replenish_at` changes only to "update time + $T_s$" (D-04 rules 1 and 3), the thread never checks later than the end of an open window. A server whose window has ended but which has not been promoted is checked again every 1 ms until it is promoted. Unnecessary execution is harmless (the program kicks only when the conditions match, and it does not change state).

**Means of triggering rescheduling (in priority order)**
1. The `timpani_replenish_kick` above. It uses flag 0, not `SCX_KICK_IDLE`. If the target CPU is in the middle of entering idle, `SCX_KICK_IDLE` can skip the kick. Flag 0 does not preempt a running task and only requests rescheduling. The premise that the sched_ext kfunc set can be called from the SYSCALL program type is to be **confirmed by a P1 PoC** (OI-9). The PoC confirms (a) whether it can be called, (b) kick on an idle CPU → promotion delay (TMG-003), (c) the cost of a kick on a CPU that is running L2, and the TT jitter effect under the boundary condition in which a TT slot starts right after the kick, and (d) that `remaining` is not lost when it runs at the same time as budget charging.
2. Alternative: a per-CPU auxiliary "kicker" thread (SCHED_FIFO, pinned to the corresponding isolated CPU) is woken by a futex and immediately waits again, which causes `schedule()` on that CPU, re-entry of `ops.dispatch`, and then promotion. A side effect is that a non-SCX RT thread runs briefly on an isolated CPU (a few µs).

**Alternatives**: `bpf_timer` — the decision in DDR-007 §4.5.1 to reject it for PREEMPT_RT reasons is retained. The option of merging the Replenisher and DeadlineMonitor into one thread is rejected in P1 (D-20 alternative, F-12).

**Consequences**: The DeadlineMonitor (D-20) is a separate thread from the Replenisher. Its priority candidate is set in the PoC so that the two threads' priority candidates are not the same.

---

### D-08. Order of Multiple L2 Servers on the Same CPU: P1 FIFO, P2 EDF

**Decision**: In P1, the per-CPU L2 DSQ is consumed in the **activation-time order of execution intervals (FIFO)**. The implementation uses the DSQ as a priority queue and gives keys as follows (`scx_bpf_dsq_insert_vtime`). The kernel names are checked against the target vmlinux.h.
- A new execution interval, and an execution interval reinserted after preemption or slice expiry: `vtime = t_act` (keeps its own activation rank).
- A server promoted after throttling: `vtime = promotion time` (it goes to the back of the queue. A server that exceeded its budget cannot again overtake other servers because it was activated earlier).

In P2, the key changes to the absolute deadline to switch to **EDF**, and the admission formula is replaced with demand and supply bound function formulas. The key used by P2 EDF is the absolute deadline $t_{dl}$ of the Job (D-20). That switch includes passing the Job deadline to BPF as a kernel-space key, so it is reviewed separately.

**Rationale**: The simple insertion order of the DSQ differs from the activation order (an execution interval reinserted after TT preemption is pushed behind one that was activated later). The premise of D-09 that "only other servers activated earlier run ahead" holds only if the activation time is used as the key. Together with property (P-c) of D-04, this order makes the interference upper bound from other servers $C_j$ per server. EDF raises the admission rate, but it requires large changes to the admission formula and to BPF.

---

### D-09. Admission Sufficient Condition (P1): Utilization + FIFO Blocking Sum — One Formula, All L2 Tasks

**Decision**: TIMPANI-O verifies all of the following for CPU $k$.

```text
(1) Utilization:     U_tt(k) ≤ U_bound  AND  U_tt(k) + Σ_j C_j/T_j + U_overhead ≤ U_bound
(2) Minimum Gap:   min_gap(k) ≥ CBS_MIN_EXEC_US            (warning → promoted to rejection)
(3) Deadline (FIFO): ∀ L2 task i on k:
                C_i + B_tt(k, D_i) + L_del_max + Σ_{j≠i} (C_j + δ_ovr) ≤ D_i
(4) Parameters:   0 < C_i ≤ D_i ≤ MIT_i,  T_i ≤ MIT_i,
                task_id is 15 bytes or fewer and unique within the node
(5) Count:       number of L2 tasks on CPU k ≤ MAX_L2_PER_CPU (16)
```

Condition (3) is checked again not only for the newly added task, but for **all** L2 tasks already placed on CPU $k$, because the $C$ of the new task is added to the interference terms of existing tasks. Conditions (3) and (4) **are the same for all L2 tasks, regardless of whether they post Events** (D-21).

- $B_{tt}(k, L)$: The maximum TT occupancy time within any window of length $L$ on CPU $k$ (after merging adjacent and consecutive slots, including the wrap at the hyperperiod boundary). If the window covers several TT blocks, it is their sum. The implementation (`TtBlocking::occupancy_within_us`) also computes the same value.
- $L_{del\_max}$ (the upper bound of the delay from arrival to the first dispatch on an idle CPU without TT) $= L_{post} + L_{wake}^{max}$. $L_{post}$ is the delay from the arrival time $t_{arr}$ recorded in the Event until the task becomes runnable, and it is TIM-011 (a new timing item, candidate: 50 µs, measurement required). This $L_{post}$ bound is a value that TIMPANI must respect when the poster thread is in the **reference poster configuration** (a configuration in which the poster thread is not preempted by another thread from recording the arrival time until the post call returns, REQ-008 A-10) (D-24). $L_{wake}^{max}$ is the delay from the runnable state to the first execution, and it is TIM-001 (candidate: 50 µs). TIM-002 (wake-up while TT occupies the CPU) is not included in this term. TT occupancy is covered by the $B_{tt}$ term (A24). Candidate $L_{del\_max} = 100$ µs. These values are measurement targets and are not proven worst-case values. TIM-001/002 keep their kernel-side definitions.
- $\delta_{ovr}$: The maximum amount by which one server can run beyond its budget (overrun due to tick resolution) (TIM-004, TMG-004). In P1, it is set to 1 ms based on HZ=1000. On nohz_full CPUs, the tick restart delay must be added and measured. This term reflects the tick overrun that the previous formula ignored.
- $MIT_i$: `min_inter_arrival_us`. If it is absent, the period $T_i$ is used (D-11 rule).

**The side condition $D_i \le MIT_i$ of (4)** guarantees that, for conforming arrivals, there is at most one live Event per source (when the Job meets its deadline). Therefore, a self-queuing term is not needed in the formula. When the MIT is violated, the queue can accumulate up to QDEPTH, and later Jobs can miss their deadlines. This is reported, but it is not prevented.

**Assumptions (items that must be documented with the result)**: This formula is a sufficient condition under the following assumptions.
1. TT tasks run within their declared runtime. A TT overrun is a TT DMISS and is outside the L2 guarantee.
2. $\delta_{ovr}$ and $L_{del\_max}$ hold as measured. The $L_{post}$ bound holds in the reference poster configuration (A-10).
3. Arrivals from the source respect the MIT.
4. The Job does not block. A source that blocks has its deadline monitored (D-20), but the admission test does not guarantee that deadline.
5. The adapter posts at the time of arrival. A supplied past $t_{arr}$ consumes part of D.
6. The charged execution time from activation to the actual sleep (including the cost of entering the wait) is at most the declared budget $C_i$. A budget margin against variation in execution time is recommended.
7. Each Job ends fast enough for the worker to sleep before the next arrival (D-04 (P-a) boundary condition).
8. $tol$ covers the measured activation jitter.

**Rationale**: The response time of Job $i$ (from arrival to completion) does not exceed the sum of (a) the amount that other servers run ahead of $i$, which is at most $C_j + \delta_{ovr}$ per server (D-04 (P-c), D-08, tick overrun), (b) the TT occupancy within the response window, at most $B_{tt}(k, D_i)$, (c) the delay from arrival to the first dispatch, at most $L_{del\_max}$, and (d) its own execution $C_i$ or less (not throttled under the conditions of D-04 (P-a)). For concrete parameter examples and arithmetic, see HLD-008 §8 and AC-008-16.

**Alternatives**
| Alternative | Verdict |
|:--|:--|
| A separate admission formula for each explicit Event connection status | TIMPANI-O does not distinguish connection status. Rejected (D-21) |
| Use only the existing `L_wake_max` and ignore the arrival-based delay and tick overrun | The posting path drops out of the arrival-based response time, and tick overrun breaks the guarantee. Rejected |
| Add a self-queuing term at the formula level | Unnecessary for conforming arrivals ($D_i \le MIT_i$). Reporting of MIT violation situations is handled by D-05. Rejected |

**Preceding fixes**: The mismatch between per-CPU and global hyperperiod, the CBS-only node `hyperperiod_us = 0`, and the unchecked TT $U_{bound}$ (REQ-008 §10-16) are fixed together (see FR-O-015, D-22).

**P2 transition**: In P2, the FIFO blocking sum of condition (3) is replaced by the precise $sbf_k(L)$ and $dbf$ expressions (EDF). This item is tracked in this section and in the P2 item of §3.3.

**CPU selection**: Among the candidates that satisfy conditions (1) to (3), **worst-fit** (maximum remaining utilization) is retained (DDR-007 §3.3).

---

### D-10. Fault Sources, Classification, and Reporting Paths — P1

**Decision**

- **Sources and paths**

| Fault | Source | Reporting path |
|:--|:--|:--|
| DMISS (L2) | DeadlineMonitor (detection of deadline passing, D-20), the worker's termination transition (`t_end > t_dl`, `LATE_COMPLETION`), the cleanup sweep, and the stale-generation cleanup while the worker waits (`DETECTED_LATE`) | The **counting path** (increase of the slot `missed`, lossless) drives the existing threshold rule. The **detail path** (miss record and the `MISSED` entry in the Job log, best-effort) removes duplicates by `(task, gen, seq)` and attaches them to the report |
| BUDGET_EXCEED | BPF (once per episode on actual entry into throttling, D-04 rule 2) | BPF `fault_ringbuf` → FaultMonitor |
| MIT_VIOLATION | Increase of the slot `mit_viol` (relative to the baseline at activation time), arrival-based (D-05) | Counting path. One entry per $T_s$ window per source, including the number of violations |
| DMISS (TT) | DDR-012 path | No change |

`BUDGET_EXCEED` does not mean a positive amount of overrun execution. Whether a separate fault report is made when the task sleeps right after a positive overrun execution and is not blocked from further execution is an open item. This case is also included in the overrun measurement of TIM-004. Unlike the existing TT/CBS design's description of the exhaustion point, the L2 reporting criteria of 008 follow this section and D-04.

- **FaultInfo.flags** (one table, common to all documents. The canonical source of the values is LLD-008)

| Bit | Name | Meaning | Set by |
|:--|:--|:--|:--|
| 0x1 | MIT_RELATED | The last MIT violation time `last_viol_ns` is after $t_{arr} - T_s$ | FaultMonitor (computed by reading `last_viol_ns`) |
| 0x2 | THROTTLED | The value of `CbsState.throttled` at the time of detection. It is filled only by DeadlineMonitor detection. It is not written for misses set by worker termination or the cleanup sweep | DeadlineMonitor |
| 0x4 | NOT_STARTED | The record was QUEUED at the time of detection (replaces `EARLY` in 9209bc5) | DeadlineMonitor |
| 0x8 | LATE_COMPLETION | The worker's termination transition was recorded after the deadline (replaces `MARKER` in 9209bc5) | Worker (left as `miss_src` in the Job log) |
| 0x10 | DETECTED_LATE | The passing of the deadline was found in the closing procedure. It is a cleanup sweep at daemon restart or stale-generation cleanup during worker waiting, and there is no real-time detection while the daemon is down | Cleanup sweep, worker stale-generation cleanup |

- The Job record uses the same bits with the same meanings, and adds its own outcome field and the `missed` indication.
- **BPF execution-interval flags** (owned by LLD-008): `CARRYOVER`, `THROTTLED`, `TASK_EXIT`. `CARRYOVER` is an execution-interval flag, and in P1 it is not a DMISS reporting flag.
- **Retired BPF bits (do not reuse)**: the emission of `FAULT_MIT_VIOLATION`, the `mit_violation` flag of the execution-interval record, `JOB_FLAG_EARLY_DMISS` (early DMISS, `early` flag), and the DMISS determination for L2. These bits are explicitly retired so that the two tables are not confused.
- **BPF `FaultEvent.fault_type` numbers**: `DMISS=0` (used only on the TT path), `BUDGET_EXCEED=1`, `MIT_VIOLATION=2` (not emitted by BPF, and the number is not reused), `WAKEUP_LATENCY_EXCEED=3` and `WATCHDOG=4` are reserved. `BUDGET_EXCEED` carries `run_seq` and `t_act`.
- **Duplicate absorption**: The BUDGET_EXCEED episode key is `replenish_at`. An episode is independent of Job boundaries, so no Job identifier is put in the key.
- **DMISS threshold**: The DDR-012 rule (report when count exceeds `current_limit`) is retained. The input of the counting path is the increase in `missed` relative to the baseline at registration activation time. Every miss is counted in the mandatory statistics, and a report is sent only when this rule says to send it. The reporting latency bound applies to the reports that are sent (A4).
- **Generation rule**: FaultInfo is sent only to the task of the current registration. A miss from a previous generation (found during closing) remains only in the records and statistics.
- **MIT_VIOLATION period**: One FaultInfo per $T_s$ window per source, carrying the number of violations.
- **Type mapping**: TIMPANI-O `handle_fault_info` maps to Pullpiri `FaultType` using types with the same meaning (the shortcut to UNKNOWN is removed). The types used in P1 (DMISS, BUDGET_EXCEED, MIT_VIOLATION) have the same numbers in both protos.
- The BPF `FaultEvent.fault_type` numbers and the proto `FaultType` numbers are **different.** TIMPANI-N `NodeClient` maps them with an explicit conversion table, and integer casting is prohibited (LLD-008 §5.5).
- Fill `task_id`, `node_id`, and `timestamp_ns` in FaultInfo. The `timestamp_ns` of node_control FaultInfo already exists but is not currently set. The `timestamp_ns` of schedinfo FaultInfo is newly added (D-11, A19).

**Alternatives**
| Alternative | Verdict |
|:--|:--|
| BPF determines DMISS at completion time (9209bc5) | Cannot catch misses of Jobs that never complete. Rejected |
| DMISS reporting by the detail path alone | Counts are lost on overflow. Rejected. The counting path is required |
| Block ring reuse until reporting, using an unreported bit | If the daemon goes down, the data path stops. Rejected (result of the review of P1-008 §10) |
| BPF emits early DMISS right after the deadline passes (9209bc5 `EARLY`) | Monitoring moves to DeadlineMonitor, so it is duplicated. Rejected |

**Consequences**: TIMPANI-O `handle_fault_info` preserves the type and flags. The `count` and `flags` fields require protobuf changes (D-11). Without approval, the report carries only `count` (no `flags`, `job_seq`, or `job_gen`, A18). Before approval, flags are not checked.

---

### D-11. proto Change Scope (P1, field additions only, all subject to explicit approval)

**Decision**: protobuf changes only add fields, and existing numbers are not changed. **All protobuf changes require explicit approval.** If an item is not approved, the corresponding function reports only counts or is implemented without that function. Numbers are finalized in LLD-008.

| File | Change | Phase | Status |
|:--|:--|:-:|:--|
| `schedinfo.proto` `TaskInfo` | Add `uint32 min_inter_arrival_us = 11`. For Sporadic, if the value is 0, `period` is interpreted as the MIT during the transition period (warning) | P1 | Approval required |
| `schedinfo.proto` `FaultType` | Add `BUDGET_EXCEED = 2` and `MIT_VIOLATION = 4`. `WATCHDOG = 3` is reserved (use in P2). The same types have the same numbers as in `node_control`. `WAKEUP_LATENCY_EXCEED` is added in P2 | P1 | Approval required |
| `schedinfo.proto` `FaultInfo` | Add `uint64 timestamp_ns`, `uint32 count`, and `uint32 flags`. `flags` uses the bit table of D-10. `count` is the number of occurrences in this report. It has a different meaning from the cumulative field `cumulative_dmiss`, so it is not reused (A19) | P1 | Approval required |
| `node_control.proto` `CbsConfig` | Add `uint32 min_inter_arrival_us = 9` and `optional uint32 cpu = 10`. `optional` distinguishes CPU 0 from unspecified. TIMPANI-N treats an entry without the field, or one that is not an isolated CPU, as an apply error, and does not substitute the partition cpuset | P1 | Approval required |
| `node_control.proto` `LayerConfig` | Publish L2 entries separately as a layer with `model = EVENT_DRIVEN`, `layer_index = 2` (no message change; publication rule change) | P1 | No message change |
| `node_control.proto` `FaultType` | Add `MIT_VIOLATION = 4`. Reserve `WAKEUP_LATENCY_EXCEED = 5`. The existing `WATCHDOG = 3` is not used until P2 | P1 | Approval required |
| `node_control.proto` `FaultInfo` | Add `uint32 count`, `uint32 flags` (MIT_RELATED 0x1, THROTTLED 0x2, NOT_STARTED 0x4, LATE_COMPLETION 0x8, DETECTED_LATE 0x10), `uint64 job_gen`, and `uint32 job_seq`. `(job_gen, job_seq)` is 1:1 with the Event identifier `(gen, seq)`, and it makes `(task, gen, seq)` unique within one object. Uniqueness across restarts depends on `instance_id`, so it is not a guarantee (A14). `count` is the number of occurrences in this report. It has a different meaning from the cumulative field `dmiss_count`, so it is not reused (A19). `timestamp_ns` (field 5) already exists | P1 | Approval required |
| `node_control.proto` `HierarchicalScheduleTable.table_id` | A monotonically increasing string for each publication (`"table_<gen>"`). No message change | P1 | No message change |
| `schedinfo.proto` `TaskInfo.temporal_class` (per task), `Response.reason/message` | — | P2 | — |
| `node_control.proto` `NodeStatus` L2 statistics | — | P2 | — |

- **The queue depth and payload size have no proto change.** In P1, they are build constants (D-19). Placing them in the workload declaration is F-11 and requires separate approval.
- The name `CbsConfig` is retained (OI-13 resolved: a new message would widen the approval scope).

**Rationale**: The current `FaultInfo` has no `count`, `flags`, or Job identifier. Without `job_gen`, the same `seq` can reappear after a daemon restart.

---

### D-12. Late Binding: Revival of TaskRegistry + Periodic Rescan — P1

**Decision**: Restore the abandoned `TaskRegistry` as the identification module. When a table is applied, it keeps the names of unresolved L2/TT tasks. It rescans `/proc/*/comm` and `/proc/*/task/*/comm` at a 500 ms period (configurable), and as soon as a task is found, it performs BPF registration, `SCHED_EXT` application, and SHM slot binding (`T_bind` ≤ 1 s). When the disappearance of a TID is detected (`/proc/<pid>/task/<tid>` absent, or the BPF `exit_task` callback), it cleans up the state and returns the task to the unresolved list. The `bind_gen` of the termination record prevents a late termination of a previous binding from releasing a new binding (D-17).

**Alternatives**: (i) Immediate binding through the `tp_btf/task_newtask` and `sched_process_comm` (comm change) tracepoints — the delay is ≪ 1 s, but it adds a BPF/ringbuf path, P2. (ii) cgroup events (`inotify` on `cgroup.procs`) + Pullpiri metadata — the final form of REQ-NFR-004, P2.

---

### D-13. TT Floor (Dual Mode) and Watchdog: Not Adopted in P1

**Decision**: Retain the decision in DDR-007 §0 (dual mode not adopted). Event silence monitoring (`WATCHDOG`) is implemented in P2 as a **userspace EventMonitor** (an extension of FaultMonitor, with new fields `FaultPolicy.watchdog_period_us` or `max_inter_arrival_us`). DDR-002 §7 is updated for alignment to "superseded by DDR-007/008". The deadline monitoring of P1 (D-20) is a different function from silence monitoring.

---

### D-14. Alignment of the DDR-005 User App Interface

**Decision**: Revise the DDR-005 statement "ttsched_wait_next_period() is required for both L1 and L2" as follows. **L1 uses `ttsched_wait_next_period()` as required. A worker in L2 that uses explicit Event connection uses the pair `ttsched_evt_wait()` and `ttsched_evt_done()` (required when Event connection is used, D-18).** An L2 task that uses only standard blocking is registered without any call and keeps CBS budget management, CPU pinning, and execution-interval observation (D-21, FR-L-003). The L2 identification constraint (`pthread_setname_np` = `task_id`) is retained.

---

### D-15. Implementation Target: C++ Path (timpani-o/timpani-n C++ + BPF) — P1

**Decision**: P1 is implemented for `timpani-o/` (C++), `timpani-n/` (C++ + BPF), and `sample-apps/src/libttsched`. `timpani_rust/` is not a P1 target, because CBS, TemporalClass, and NodeStream are absent and the timpani-n Rust side has no runtime (based on the analysis in REQ-008 §10). However, only new fields are synchronized into the Rust `schedinfo.proto` definition file, and there are no Rust code changes. A follow-up task is left for a Rust port to carry over the model of this DDR (state machine, admission formula, proto) as is.

**Consequences**: The statement "Rust is the only actively developed" in `doc/docs/structure.md` conflicts with this decision, so that document needs an update. The C++ path is not included in CI, so it is verified with local GTest and integration scripts (SRS-008 NFR-007).

---

### D-16. Observation Output: Job Records, Execution-Interval Records, Mandatory Statistics — P1

**Decision**: Observation output is divided into three kinds. Mandatory statistics are unrelated to the trace settings.

1. **Job record (worker Job log)**: An SPSC log that the worker writes at `done` (in SHM, loss is possible, drop count kept). The only producer is the worker. TIMPANI-N's `JobTraceCollector` collects it. A record is `{gen, seq, t_arr, t_dl, t_begin, t_end, outcome, miss_src}` and is built as a non-shared copy by the worker (the shared record is not read after termination). `miss_src` tells whether the MISSED flag was set before the termination transition or was set by the termination transition. The `flags` and `missed` of the output record are not written by the worker; FaultMonitor computes them from `miss_src` and the state and attaches them. Jobs closed by TIMPANI and miss records also use the same output format in the trace output of TIMPANI-N. Fields for which the time does not exist are left blank. The trace output also records the 64-bit `instance_id`, so that offline analysis can separate daemon runs (A14).
2. **Execution-interval record (BPF `job_trace_rb`)**: `BPF_MAP_TYPE_RINGBUF` (256 KiB). The record is `{…, run_seq, t_act, t_start, t_sleep, budget_used_us, flags}`, and flags use `CARRYOVER` and `THROTTLED`. It is gated by the global `trace_enabled` variable (not rodata, toggled at runtime). Exception: the task termination record (`ops.exit_task`) is always emitted without the gate, for TaskRegistry cleanup and `bind_gen` confirmation. The channel name `job_trace_rb` is retained, and it is stated explicitly that its content is execution intervals.
3. **Mandatory statistics (unrelated to traces)**: The cumulative counters of the SHM slot (D-03) and the BPF counters. `EventSourceMonitor` reads them as periodic snapshots and outputs them. The values within the registration are obtained as the difference from the baseline at activation time. Statistics that require a tracepoint (for example, execution on an unassigned CPU) are outside the mandatory set, and where they are counted is stated explicitly.

- Response-time statistics are computed as `t_end - t_arr`. The offline matching rule based on `t_rel` in 9209bc5 and the Job–Event pairing rule are retired. Because every Job record has its own `t_arr`, pairing is not needed. The `t_act` of an execution interval is a value for observing budget enforcement.
- The trace output format (gpdata extension) is to be agreed with the verification framework. Timer Master jitter samples are also output in the same channel format.

**Alternatives**
| Alternative | Verdict |
|:--|:--|
| Reuse `fault_ringbuf` | Fault and trace bandwidth and priority are mixed. Rejected |
| Kernel tracepoint (`bpf_trace_printk`, `trace_pipe`) | Non-deterministic, and parsing costs are incurred. Rejected |
| Record `n_events` of 9209bc5 in the Job record and match Jobs and Events in real time | Races arise in asynchronous collection. Rejected |
| Offline matching based on `t_rel` of 9209bc5 | Intervals where the conditions are broken must be handled separately. Rejected. Each record has its own `t_arr` |

---

### D-17. Runtime Update (P1): Full Table Republication Retained, Registration and Binding Generations

**Decision**: P1 retains the current full-table publication and full re-registration. The following rules are kept together.

- (a) If the L2 entry is identical and the task is already bound, do **not write** to `cbs_map` and `task_meta_map`. If userspace rewrites the values as a whole, it overwrites BPF updates made in the meantime (budget, execution-interval state, counters).
- (b) An entry whose parameters have changed is **removed and then re-registered.** The task is returned to `SCHED_OTHER` and removed from the queue, the map entry is deleted, and the new entry is written before the task is put back into `SCHED_EXT`. When newly registering, the map entry is also written first, and the switch to `SCHED_EXT` is done later (so that a wake-up right after the switch is not handled without a server).
- (c) **Activation (at binding)**: Activation is performed after the worker thread is bound to `SCHED_EXT`. It is not at the time of table application. Therefore, if acceptance succeeds, there is a managed worker. The order is as follows. ① Close the unterminated records of the previous registration (QUEUED becomes CANCELLED, RUNNING becomes ABANDONED, and if the deadline has passed, MISSED is also set). ② Write the configuration (`mit_us`, `deadline_us`). ③ `last_arr_ns = 0`. ④ Create a new `gen` (upper 32 bits from `instance_id`, lower 32 bits by incrementing the binding generation number). ⑤ `kind = EVT`. TIMPANI-N sets the counter baselines (`missed`, `mit_viol`, etc.) at this point.
- (d) **Deactivation** (worker removal, STOP, worker termination detection, daemon termination): First write `kind = NONE`, then perform a closing sweep that scans all entries of the slot. QUEUED is closed as CANCELLED. RUNNING is closed as ABANDONED only when the worker has terminated; otherwise it is left as RUNNING until the worker finishes, and deadline monitoring continues. If the party that closes the record confirms the passing of the deadline, MISSED is also set. A waiting worker receives E_INACTIVE.
- (e) The SHM slot of the same `task_id` and its cumulative counters are retained (D-03). For each binding, the BPF execution-interval state (`run_seq`, last activation time, window state) restarts from 0. `run_seq` is unique only within a registration interval, and the order of records is determined by `t_act`.
- (f) `TaskMeta` holds `bind_gen`, so that a task termination record from a previous binding does not release the new binding (FR-N-005 update).
- (g) A Job that is in progress during re-registration is either closed (ABANDONED) or recorded after its worker finishes. Observation gaps are allowed in P1. An Event posted at this boundary does not have the same `gen`, so it does not change the state of the new registration (D-19).
- (h) If the TT slot set is the same, the Timer Master restart SHOULD be omitted.

**Alternatives**
| Alternative | Verdict |
|:--|:--|
| Activate at table application time | Acceptance could occur with no worker present. Rejected |
| Reset the ring indices to 0 at activation | Touches the indices that the poster writes (the indices run freely). Rejected |
| Reset the cumulative counters at each re-registration | The baseline is sufficient. Cumulative values are kept for the lifetime of the object. Rejected |

---

### D-18. One Event – One Job, Explicit Job Boundaries — P1

**Decision**: One accepted Event is one Job. The worker receives one Event with `ttsched_evt_wait()`, starts the Job, and always ends it after processing with `ttsched_evt_done(outcome)`. There is no batch consumption and no implicit completion. A consecutive Job (R3) is a separate Job, and each has its own `t_begin`, `t_end`, and MISSED determination. A kernel execution interval is not a Job boundary.

- A Job record in the output is `{node, task, gen, seq, t_arr, t_dl, t_begin, t_end, outcome, flags}`. Jobs terminated by the worker, miss records, and records closed by TIMPANI all have the same format. `flags` is computed by FaultMonitor (D-16).
- If the worker omits `done`, the next `wait` returns E_STATE. If the worker thread terminates, TIMPANI closes the RUNNING record as ABANDONED (D-17).
- Blocking is allowed during a Job. Blocking time is included in the response time, and a Job that blocks is outside the guarantee of the admission test (D-09 assumption 4).

**Rationale**: I1 requires a 1:1 relation between a Job and an accepted Event. Batch consumption makes one Job represent several Events, so the deadline and response time per Event cannot be defined. Kernel sleep boundaries cannot distinguish consecutive Jobs from blocking. Because the explicit boundary is owned by the worker, it separates both cases exactly.

**Alternatives**
| Alternative | Advantages | Disadvantages | Verdict |
|:--|:--|:--|:--|
| (i) Batch consumption: wait returns the number of pending items n, and one Job processes n items (9209bc5) | Fewer calls | One Job represents several Events. Individual deadlines and responses cannot be defined. Arrivals during processing cannot be represented | Rejected |
| (ii) Merge all waiting Events into one Job | Simple implementation | Same as (i) | Rejected |
| (iii) Use the kernel sleep boundary as the Job (9209bc5 wait point) | No app change | Consecutive Jobs are merged, and blocking splits Jobs | Rejected |
| (iv) Identify sleep boundaries by system-call classification or a `sched_yield()` flag (9209bc5 P2 plan, earlier F-8) | Boundaries identified in the kernel | Cannot distinguish arrivals during processing from consecutive Jobs. The kernel infers app semantics | Withdrawn |
| (v) Treat the previous Job as implicitly completed at the next wait (9209bc5) | Simple app code | A failed Job is recorded as completed | Rejected |

**Consequences**: Worker code must include the `done` call. The sample app and the FCW application procedure are covered in HLD-008 §8 and LLD-008.

---

### D-19. Common Event Queue: Fixed Record Ring, Inline Payload, Acceptance, Overflow, Closing, Generation — P1 (constants and structure require approval)

**Decision**: The Event queue is a **fixed-size record ring** per source (QDEPTH entries), and the payload is carried inline inside the record.

- **Build constants (candidate)**: `TTSCHED_EVT_QDEPTH` = 8 (a power of 2; ring occupancy limit: the sum of QUEUED, RUNNING, and terminated entries that `head` has not yet advanced past), `TTSCHED_EVT_PAYLOAD_MAX` = 64 B. Both values require agreement and measurement, and since they are part of the public API, they are subject to approval. E_FULL can occur even when the number of live Events is less than QDEPTH, because when a RUNNING Job is at `head`, the closed entries after it block `head` (A13).
- **Record (128 B)**: `word` (u64 atomic value; the upper 32 bits are `seq`, and the lower bits are the state and the MISSED flag), `gen` (u64), `t_arr_ns`, `t_dl_ns`, `len`, `payload`.
- **States**: FREE (initial state, an object filled with zeros), QUEUED, RUNNING, COMPLETED, DISCARDED, CANCELLED, ABANDONED, REVOKED. FREE is neither a posted state nor a terminal state. A state change after posting is a CAS on the whole `word`. Because `word` includes `seq`, a CAS that observes a previous occupant of the same ring entry fails.
- **Acceptance (summary of the post protocol)**: ① Check the slot's `kind` (the header `magic` is not checked; the restart cleanup blocks posting by setting `kind = NONE`, A2). ② Read `gen`. ③ Count the MIT with the arrival check (D-05). ④ Check whether the ring is full. If it is full, advance `head` past finished entries; if it is still full, raise `overflow` and return E_FULL. ⑤ Fill the record, publish `word` as QUEUED, and then advance `tail`. ⑥ Check `kind` and `gen` again. If this check fails, roll back according to the current `word`. If it is not QUEUED (another party has already taken or closed it), count it as accepted. If the current `word` is QUEUED and MISSED is already set, close it as CANCELLED, keep MISSED, and count the acceptance and the cancellation. If it is QUEUED and MISSED is not set, roll it back to REVOKED regardless of whether the deadline has passed, and return E_INACTIVE. It is not an Event. The rollback does not newly set MISSED (A11).
- **Overflow**: An Event that is already accepted is not discarded from a full queue (I1). Only the new post is rejected.
- **Consumption and termination**: When the worker receives a Job, it CASes from QUEUED to RUNNING. The worker copies the record while it is RUNNING. Termination is the CAS of `done`. `done` first reads `word` (W0) and checks the seq of the open Job and whether it is ABANDONED (A6). `head` is advanced by CAS by the party that sees an entry in a terminated state (the worker, TimerMaster, or the poster).
- **Closing**: At activation, unterminated entries of the previous registration are closed (D-17 (c)). At deactivation, QUEUED becomes CANCELLED, and RUNNING follows the rule of D-17 (d). When closing confirms the passing of the deadline, it also sets MISSED (D-20).
- **Generation**: Each record has a `gen`. A monitor skips records whose `gen` differs from the current `gen`, so it does not change their state or counts (D-20 rule 3, A9). A closing sweep closes all unterminated records that it is instructed to close, without comparing generations. At activation, the records that are closed are those of the previous registration; at deactivation, the records that are closed are those of the current registration (A9).
- **Restart**: At daemon start, the previous object is changed to `magic = 0` and `kind = NONE` for all slots. The closing rule is applied to all slots (MISSED for entries whose deadline has passed), waiters are woken, the final counts are recorded, the object is unlinked, and a new object is created with a new `instance_id`. Clients attached to the old object receive E_INACTIVE and attach to the new object with `ttsched_init()`. `seq` restarts in the new object. `(gen, seq)` is unique within one object until `seq` wraps around (within 2^32 acceptances). Uniqueness across restarts depends on the upper 32 bits of `gen` (obtained from `instance_id`), so it is not a guarantee. The trace output also records the 64-bit `instance_id` (A14).
- **Poster**: For each source, there is one poster at a time. Concurrent posting by several posters (a multi-poster ring) is not supported in P1 (F-13). Damage caused by two overlapping posts affects only the queue of that source.
- **Payload**: TIMPANI does not interpret the payload. It is posted in the same record together with the identifier and times, so the consumer receives the identifier and data of its own Event together. Putting a pointer into the app's storage in the payload is outside the contract (F-13). A large payload is P2, together with an explicit release notification.
- **P1 limitation**: If a post is in progress at the re-registration boundary, its arrival can be counted in the arrival count of the new registration (at most one per re-registration). While the daemon is down, posts are accepted and the worker keeps processing, but there is no deadline detection (D-20). If the poster dies after the entry is made visible (after `tail` advances) and before (f), the Event is processed and monitored, but one `accepted` count is lost, and the next post does not recover it. If it dies after the post and before `tail` advances, the next post recovers that record, counts it, and wakes the worker. In both cases, the worker does not wake up for that Event until the next post or a wake-up by TimerMaster (A8). Therefore, 'accepted count + rejected count = number of post attempts' holds only in intervals in which the poster does not die. If the worker takes a QUEUED record before the closing sweep reaches it after deactivation has written `kind = NONE`, that Job is allowed in P1, and it is recorded normally and monitored (A12).

**Alternatives**
| Alternative | Advantages | Disadvantages | Verdict |
|:--|:--|:--|:--|
| (i) App-owned payload queue + batch count n + invalidate/clear + `TTSCHED_EVT_RESET` + SpscRing (9209bc5) | Familiar data structure | Two queues must be kept in step. Invalidation race. RESET is needed on each restart | Rejected |
| (ii) Multi-poster ring with reserved entries (first review proposal) | Posting by several posters | A poster that stops after reserving blocks the entry. Posts out of order occur. P1 has one poster | Rejected. P2 F-13 |
| (iii) Payload handle (pointer into app storage) | Large data | Lifetime and release notification are outside the contract. Risk of invalid pointers | Rejected. P2 F-13 |
| (iv) Unbounded queue | No overflow | Breaks the real-time and memory limits | Rejected |
| (v) Reset the ring indices at activation (first review proposal) | Simple | Touches the indices that the poster writes. The indices run freely | Rejected |
| (vi) An unreported bit that blocks ring reuse until reporting (second review proposal) | No missing reports | If the daemon goes down, the data path stops. Counts are the mandatory path, and details are best-effort | Rejected |
| (vii) Mark the post state as PENDING (second review proposal) | Distinguishes unconfirmed posts | If the poster dies, the state remains. The determination path grows | Rejected. Handled by the rollback loop |

**Consequences**: The queue constants and the SHM layout require approval (D-03). Deadlines are not detected while the daemon is down (D-20, F-14).

---

### D-20. Deadline Detection Without Waiting for Completion: Arrival-Based Absolute Deadline, DeadlineMonitor — P1

**Decision**
1. **Deadline**: The absolute deadline is $t_{dl} = t_{arr} + D$ and is fixed at acceptance. It is not moved later because of activation, replenishment, resumption, or re-registration. **The activation time + D is not used as a substitute deadline.**
2. **Start of monitoring**: Monitoring starts at the first acceptance. It does not wait for the worker to start. Acceptance happens only when a managed worker exists (D-17 (c)).
3. **Detecting thread**: The new **DeadlineMonitor** thread of TIMPANI-N. It is SCHED_FIFO with a candidate priority of 97, below TimerMaster (99) (Replenisher is 98, A1). It is placed on a non-isolated CPU. It does not use heap allocation, blocking I/O, or gRPC. The clock is CLOCK_MONOTONIC (the use of CLOCK_REALTIME in the TT path is not changed). The candidate value of the period `DL_SCAN_PERIOD_US` is 1000 µs. In each period, it reads the clock once and checks all ring entries of all slots, selecting them by state regardless of `kind`. Entries whose `gen` differs from the current `gen` are skipped (A9). For an entry that is QUEUED or RUNNING, has no MISSED, and satisfies $now > t_{dl}$, it sets MISSED by CAS and increments `missed`, and then passes a miss record `{slot, gen, seq, t_arr, t_dl, t_detect, state_at_detect, throttled}` to FaultMonitor through a fixed-size SPSC queue. If the queue overflows, the overflow is counted. Only at detection, it reads the server's `CbsState` once (a non-blocking BPF map lookup) and fills `throttled`.
4. **Detection latency**: The delay from $t_{dl}$ to the setting of MISSED is at most the sum of the period and the wake-up jitter. The premise is that there is no higher-priority load on that CPU. The candidate upper bound is 2 ms. The reporting latency (from the setting of MISSED to the FaultInfo of TIMPANI-O, applying only to reports that are sent) is a separate bound, and its candidate is 200 ms. **Both values require agreement and measurement.** This does not mean that zero delay is guaranteed.
5. **Once per Job**: MISSED is set with a single CAS, and the party that sets it increments `missed`. The setting party is one of the following: DeadlineMonitor, the worker's termination transition, the closing sweep (closing by TimerMaster and cleanup at daemon start), or the stale-generation cleanup performed by the worker while waiting. A post rollback keeps an already set MISSED and does not set it anew (A11). Because the `missed` increment is executed after the CAS, it can fall after the activation baseline (at most one per binding, A9). A later completion is linked to the completion record, and the failure is not counted again. MISSED is not cleared by any later transition.
6. **Completion time and race rule (interpretation, to be confirmed)**
   - $t_{end}$ is the clock value that the worker reads in `done()` immediately before it posts the termination transition. The posting is the completion. Completion is the time at which the worker finishes the declared processing and posts the result. It does not mean the completion of network delivery or actuation. TIMPANI does not count the time of IRQ/ISR, middleware-internal threads, or the network path in the budget (out of scope of REQ-008 §7, A21).
   - If the worker's transition CAS races with the MISSED setting by monitoring, the transition fails. The worker reads the clock again (this value is after $t_{dl}$) and retries. Therefore, the recorded $t_{end}$ and the MISSED flag do not disagree.
   - A completion posted on time (before the deadline) is not a miss even if its collection is late. The determination is made with the recorded $t_{end}$.
   - Permitted error: A Job recorded as on time can be posted just after $t_{dl}$ because of preemption between the clock read and the transition. That posting completes before the first monitoring period after $t_{dl}$ (a posting never happens after a monitoring period).
   - A guard band and a provisional determination are not adopted. Defining completion as the posting and re-reading the clock achieves the same purpose without a determination reversal.

7. **Termination rule (interpretation, to be confirmed)**: Terminations before the deadline (COMPLETED, DISCARDED) and closings by TIMPANI before the deadline (CANCELLED, ABANDONED) are not misses. They are counted as separate outcomes and are not counted as successes. Any termination after the deadline (a late completion, a late discard, cancellation, or abandonment) is a miss. A miss that is already set is not cleared by a later termination.
8. **Follow-up for a failure**: A deadline miss is a result separate from the processing state. Detection of a failure does not mean automatic termination of the Job. In P1, the worker processes a failed Job to the end and records the result. Whether a failed Job continues or is cancelled is a separate policy and an open item.
9. **While the daemon is down (P1 limitation)**: While the daemon is not running, the existing object remains `kind = EVT`. Posts are accepted and the worker keeps processing. The worker's termination transition determines a late completion. However, Events that expire in the queue or during execution are not detected in real time. The cleanup sweep at the next daemon start finds them as DETECTED_LATE and records them in the final statistics. P1 has no daemon heartbeat (F-14).
10. **Separation of detection and reporting**: Detection (DeadlineMonitor) and reporting (FaultMonitor, counting path and detail path) are separated according to D-10.

**Alternatives**
| Alternative | Verdict |
|:--|:--|
| (i) Activation-based deadline $t_{act} + D$ (9209bc5) | The deadline moves later by the queue wait. Queue delay is not visible. Rejected |
| (ii) Determination only at completion time (BPF `stopping`/`quiescent`) | Detection does not happen during queue wait, throttling, or CPU idle. Without completion, there is no determination. Rejected |
| (iii) The worker sets a timer when the Job starts | Does not work if the worker is not scheduled. There is no monitoring while waiting. Rejected |
| (iv) Handle the race between completion and monitoring with a guard band or a provisional determination | A provisional determination requires revision. The guard width depends on measurement. Rejected (rule 6) |
| (v) Mark unconfirmed posts as PENDING | If the poster dies, the state remains. The determination path grows. Rejected (handled by the rollback loop) |
| (vi) Merge DeadlineMonitor with the Replenisher thread | Roles and periods differ, so priorities conflict, and the replenishment path is tied to monitoring delay. Rejected in P1. F-12 after measurement |
| (vii) Set an hrtimer or timerfd for each Event | Timers must be managed for each record, and allocation and cancellation costs are incurred. TimerMaster load increases. Rejected. Event-based wake-up is F-10 |
| (viii) Deadline determination in BPF | BPF cannot wake a Job waiting in the queue, and it does not run on an idle CPU. Rejected |
| (ix) Detection during daemon downtime by a daemon heartbeat | Outside the P1 scope. P2 F-14 |
| (x) Set a single timer for the earliest absolute deadline | A review alternative different from (vii), which uses per-Event timers. Its conditions are compared in F-10/OI-22, and it is not adopted |

**Absolute deadline timer alternative (for review, not approved).** It is also possible to manage the earliest unterminated deadline among the $t_{dl} = t_{arr} + D$ values fixed at acceptance with a min-heap or similar, and to align a single absolute timer of DeadlineMonitor to that time. As with the periodic scan, it does not wait for the worker to start. However, when a new post creates an earlier deadline, the monitor must be notified and the timer reset, and the loss race between the notification and sleep or reset must be designed. The existing SHM `counter`/futex wake-up is for workers and is not the DeadlineMonitor notification path. A new notification path or API is not approved here.

A timer expiry and the actual execution of detection are different. Because of scheduling, the execution can be observed later. After waking, the same `gen`/`seq` and state are checked again. Because on-time completion is allowed until $t_{end} \le t_{dl}$, MISSED must be set once by an atomic CAS only when $now > t_{dl}$. If the timer wakes exactly at the deadline time, it is not immediately treated as a miss, and the re-check method is also subject to review. The determination of the termination path for late completion and the existing rules for keeping MISSED and preventing duplicate counting are still required as they are. The timer does not resolve the race between the physical completion and the recorded completion of rule 6 (OI-20), and it does not automatically guarantee a jitter bound. **The current baseline is the periodic scan (1 ms candidate)**, and any change goes through design review and a target PoC (OI-22).

**Consequences**: The detection latency and the reporting latency are candidate values until measured. The DL_SCAN cost and jitter are PoC items (F-6, F-10). BPF loses its deadline determination code and becomes simpler. The two interpretations (rules 6 and 7) are open items.

---

### D-21. Responsibility Split Between CBS Budget Management and Event/Job Management — P1

**Decision**: The two responsibilities of REQ-008 §2 apply to the same L2 worker and are not an app's subclassification or mode. Even without explicit Event connection, CBS budget management, CPU pinning, `BUDGET_EXCEED`, and execution-interval recording are maintained. Without connection, the information needed for Event identification and arrival time, Job records, MIT counting, and Event-based DMISS monitoring cannot be obtained.

- **No additional flag**: P1 does not have additional classification or configuration flags for this responsibility split. If a source has no Event records, there is no object for DeadlineMonitor to determine. A temporary silence in posting does not reclassify the app.
- **Guarantee wording**: Without explicit Event connection, the design does not estimate Event or Job deadlines from BPF execution intervals, and it does not describe itself as equivalent to per-Event arrival-based deadline support. Compatibility for existing apps that use standard blocking calls as they are is retained (FR-L-003).
- **Admission**: The same admission formula of D-09 applies to all L2 tasks. When there are no Event records, the declared $D$ is a parameter of the admission test based on kernel activation, not a monitored Job deadline. The $L_{post}$ part remains as additional margin.

**Rationale**: Without explicit connection, the kernel observes only execution intervals. Because there is no identifier or arrival time, per-Event deadline monitoring cannot be provided. CBS budget management and CPU pinning can be performed on execution intervals. This is a split between observation information and responsibility, and it does not change the L2 semantics of DDR-001 or the existing FFI requirements.

**Alternatives**
| Alternative | Verdict |
|:--|:--|
| (i) Separate admission formulas by whether explicit Event connection is used | Rejected. The existing single admission formula applies to all L2 tasks |
| (ii) Add a classification field for the responsibility split to the proto | Rejected. Event records are monitored without additional classification or configuration flags |
| (iii) Treat a kernel wake-up as an Event (9209bc5 adapter K) | There is no identifier or arrival time. Consecutive Jobs are merged. Rejected |
| (iv) Reject L2 registration when there is no explicit Event connection | Breaks compatibility of CBS budget management for existing L2 tasks. Rejected |

---

### D-22. CBS-only Execution: Independence from TT Firing and the TT Hyperperiod — P1

**Rationale:** [2026-11-12 Weekly discussion and follow-up answers](https://github.com/eclipse-timpani/timpani26/discussions/4). CBS is enforced inside `SCHED_EXT`, not Linux `SCHED_DEADLINE`. Even in CBS-only operation, Event acceptance, per-CPU dispatch, budget replenishment, and DeadlineMonitor monitoring must work.

- On a CPU with no TT slots, the TT occupancy is 0, and the available Gap is the entire time. D-09 checks it with `B_tt(k, D) = 0`. The per-CPU L2 count limit and the checks of interference from other servers and of utilization are retained.
- CBS replenishment works with each server's `T_s` and with the window and rescheduling trigger of D-04/D-07. It does not have a dependency under which replenishment or dispatch happens only after TT_START/TT_END occur.
- **The CBS execution is designed not to read the hyperperiod value.** Replenishment uses each server's `T_s`, and dispatch and deadline monitoring use absolute times. The MIT is an arrival constraint, and `T_s` is the replenishment period. A TT hyperperiod is not created from the LCM of MITs or from an arbitrary default. This is the result of design analysis, and it is subject to confirmation by P1 verification (AC-008-17, AC-008-18). The follow-up answer to Weekly item 1-f was to first check whether a hyperperiod is needed, and it was not a meeting decision.
- The table field `hyperperiod_us` is based on [SRS-008 FR-O-015](SRS-008-event-driven-workload-support.md). A node without TT also uses a defined value that is not 0 (LLD candidate `DEFAULT_HP_US`, 10 ms). This value is a table-format value, and it is not a CBS execution parameter. The current code writes 0 to the table of a node that has no TT and has tasks (`global_scheduler.cpp` lines 109, 140, 493). This does not meet FR-O-015, and it is a preceding bug of REQ-008 §10-16. Whether to change the representation (for example, allowing 0), the time at which an update is applied, and the boundary of the first addition and final removal of TT are open (OI-25). DDR-011 Phase 0 handles only L1 TT updates, so this item is to be decided together in [DDR-011 Phase 1](DDR-011-runtime-table-update.md#10-deferrals-and-review-gate) review. This decision is not an approval of a field change.
- **P1 verification (CBS-007, AC-008-17, AC-008-18):** On a node without TT and on CPUs without TT in a mixed node, confirm first Event processing, arrival again after idle, and backup replenishment after budget exhaustion. Check that adding or removing TT does not reset the existing CBS budget or incomplete Jobs, or change deadlines.

### D-23. Separation of the Deadline-Failure Fact from Cause and Recovery Policy — P2

**Decision direction:** The deadline-failure fact of an L2 task affected by TT overrun is not erased, but the fact alone does not conclude that the L2 task itself is defective, and it does not impose automatic penalties. The cause analysis mentioned in the meeting is a separate design and verification target, not an intelligent determination that is already implemented.

1. **Basic observation retained in P1:** Provides per-Job `MISSED` detection, counting, and basic reporting, together with the existing observation information of D-10/D-16/D-20. This is the fact of a deadline failure, not a confirmed cause. A new TT correlation analysis or cause-specific recovery policy is not a P1 completion condition.
2. **P2 policy design:** When the threshold-exceeded report of D-10 is combined with the recovery policy of [DDR-012 §4.4](DDR-012-fault-management.md#44-2026-11-12-weekly-follow-up--separating-affected-task-and-causing-task-for-review), the causing and affected tasks and their responses are defined so that a CBS affected by TT interference is not automatically attributed as its own defect. The existing reporting and recovery paths of P1 are not changed by this stage's separation, and they are not described as providing cause-aware recovery. The counting of all misses and the scope and latency of external reporting are kept as separate open items of the P1 basic contract.
3. **P2 analysis extension:** Review cause classification that links TT execution intervals, slot end delay, Timer Master jitter, CBS execution, and Job records; multi-cause analysis; and automatic recovery selection. A causal link is not confirmed by TT overrun, `THROTTLED`, and MIT violation that occur only at the same time. The concrete algorithm and schedule are not fixed, and this does not approve new AI dependencies or proto fields.
4. **Scope of guarantee:** TT boundary protection is the responsibility of DDR-007 §4.3.1 in P2. Before that verification, D-09 is a conditional guarantee that TT is within its declared execution range. TT termination enforcement and cause analysis do not block completion of the P1 basic environment, but the design does not claim that time isolation including TT overrun is guaranteed in P1.

### D-24. Application Abstraction and Execution Environment Assumptions — P1

- **P1:** The TIMPANI dependency can be hidden behind the app's input adapter or library wrapper. The core does not know SOME/IP semantics, and the worker uses the common Event/Job contract. However, at some layer, arrival, Event identification, and Job start/end must be stated explicitly. The same information is not inferred automatically from a kernel wake-up of an arbitrary app alone (D-01 to D-03, D-18).
- **Follow-up investigation:** A fully transparent connection with no app change at all is not a P1 guarantee. Even if a wrapper or middleware hook is chosen, its observation accuracy, cost, and failure handling are to be verified separately.
- **P1 environment check:** Distinguish the PID/TID, name, actual CPU, policy, allowed mask, and cgroup of L1/L2 threads and of the receiving and vsomeip threads. `sched_setaffinity` cannot take a thread outside `cpuset.cpus.effective`, and root cannot bypass this constraint. Do not confirm the cause from `EINVAL` alone; check the online CPUs, the mask, and the target cgroup together.
- Placement in cgroup/cpuset is the responsibility of the system and Pullpiri in [DDR-003 §2](DDR-003-interface-protocol.md#2-context). To separate the workers and unmanaged threads of the same process, first check the cgroup configuration and whether each thread can be moved. BPF CPU assignment or a priority change does not substitute for this configuration.
- Moving an unmanaged thread to a non-isolated CPU or raising it to `SCHED_FIFO` is not a blanket remedy. Distinguish an affinity change from a cgroup move, and verify not only the receive delay but also the interference with and starvation risk to Timer Master, Replenisher, and DeadlineMonitor. The existing candidate delay values are not automatically guaranteed in this environment either.
- **Requirement:** The check of the allowed CPU set and explicit failure are traced to REQ-008 CBS-008, A-09, AC-008-19, and SRS-008 FR-N-033. The current code does not have this check.

**P1 policy boundary and call context.** Under the scope decision of the Project Leader (author), the selection and management of scheduling policy for the input adapter, middleware, and other app threads are excluded from P1. By contrast, a registered **L2 worker TID is always placed under `SCHED_EXT` and receives the kernel BPF CBS enforcement of timpani-n**. It is not a requirement that the same policy apply to the whole process. The `libttsched` API runs in the context of the calling thread, and SHM is a passive shared storage, not an execution entity. The CPU cost actually consumed in the worker's wait and done is included in the worker's CBS (the blocking wait time itself is not a CPU cost), and the post cost belongs to the policy and budget of the calling poster thread. The $L_{post}$ of the D-09 admission formula (TIM-011) is a delay term, not a CPU cost. Since the posting path is `libttsched` code, its delay bound is a TIMPANI requirement. However, because the posting call runs on the poster thread, the bound holds when the poster thread is in the **reference poster configuration**. The reference poster configuration is a configuration in which the poster thread is not preempted by another thread from recording the arrival time until the posting call returns (candidate: a real-time policy thread pinned to an idle or dedicated CPU. Proposal, to be fixed by measurement). Whether the deployment satisfies this configuration is an assumption that is the responsibility of the app and deployment (REQ-008 A-10). In a deployment that does not satisfy it, the guarantee of the admission formula does not hold (Project Leader (author) decision, 2026-11-18). The fixing of the candidate values and of the reference poster configuration follows F-6. Excluding the input-thread policy does not exclude the P1 placement and environment checks above, or the explicit failure of incorrect placement.

**Review of a deployment that isolates only workers (not approved, not implemented).** The review goal is a configuration in which, even within the same process, only the registered workers are placed on an isolated cpuset, and non-workers run on non-isolated CPUs with fair scheduling (`SCHED_OTHER`, etc.) or `SCHED_FIFO/RR`. This is not an approval of a policy choice for non-workers or of a new TIMPANI responsibility. The existing placement responsibility remains with the system and Pullpiri in DDR-003 §2. Whether TIMPANI-N moves worker TIDs, or only verifies the deployed result, is open, and the current binding code has no cgroup move.

- **Conditional possibility on Linux:** In a threaded cpuset of the [cgroup v2 documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html#threads), threads can be separated by writing TIDs to `cgroup.threads` inside a **common threaded resource domain**. Writing a TID to `cgroup.procs` moves all threads of the process. It is not a method of arbitrarily moving one thread between different ordinary domain cgroups. A thread cannot go outside the allowed CPUs of its ancestor, and if an ancestor allows only isolated CPUs, a non-worker cannot be sent to a non-isolated CPU by the lower configuration alone. The possibility of threaded separation and the establishment of partition isolation with `cpuset.cpus.partition=isolated` are separate properties.
- **Checks before approval:** The threaded cpuset/partition support and effective topology of the target kernel, the delegation and move permissions of the system and container runtime, and the policy and cpuset inheritance at thread creation and the relocation at restart and rebinding must be confirmed. Currently, BPF's `SCX_OPS_SWITCH_PARTIAL` allows fair non-workers that are not `SCHED_EXT`, but this fact alone does not verify the deployment above. A PoC of the exact target configuration is required. For the non-worker FIFO/RR candidate, also check `CONFIG_RT_GROUP_SCHED`, the activation of the cgroup v2 `cpu` controller, and the RT thread placement constraint. Do not regard `cpu.max` as a FIFO/RR budget or as a TIMPANI CBS budget.

  The Linux documentation states the constraint that, when `CONFIG_RT_GROUP_SCHED=y`, all RT threads must be in the root cgroup in order to activate the `cpu` controller. Therefore, whether FIFO/RR placement in a non-root threaded cpuset and use of the `cpu` controller can coexist must be confirmed on the target, and cpuset separation itself is not equated with activation of the `cpu` controller.

---

## 3. Consequences

### 3.1 Positive
- The definitions of Event, Job, deadline, and result are clear, and they are identified 1:1. The verification framework can determine the pass/fail of records.
- Consecutive Jobs, arrivals during processing, and blocking are distinguished as different records.
- Deadline detection that does not wait for completion is designed to work during queue wait, throttling, and CPU idle (pending measurement).
- A task that keeps its declaration is not throttled (when the premises of P-a hold). Because the replenishment delay is bounded, "starvation on a CPU without events" is eliminated.
- L2 tasks are pinned to their assigned CPUs, so the per-CPU Gap analysis of DDR-007 becomes valid.
- The core is middleware-neutral. Apps use the same contract through an input adapter.
- The proto keeps compatibility by field additions only.
- Mandatory statistics are unrelated to trace settings.

### 3.2 Negative / Cost
- There is an ABI break in `libttsched` v2 and SHM (recompilation required). The public API, SHM layout, and queue constants are subject to approval (D-03, D-19).
- The worker must call `done`. The app must implement the input adapter.
- BPF changes (the execution-interval state machine, per-CPU DSQs, fault transitions) and new threads (DeadlineMonitor, Replenisher) widen the verification scope of timpani-n. PoCs (OI-9, monitor PoC) are required.
- The admission rate of the admission formula becomes lower, because $\delta_{ovr}$ and $L_{del\_max}$ are included, and this is especially so in multi-L2 configurations (P2 EDF, HRTICK review).
- A Job that blocks has its deadline monitored, but the admission test does not guarantee it.
- While the daemon is down, Event deadlines are not detected in real time. There is no heartbeat (F-14).
- Arrivals beyond the queue limit QDEPTH are rejected as overflow. The limit value requires measurement.
- A server that violates the MIT or exceeds its budget has its window pushed behind the arrival phase, and the response time of that task itself increases until the first activation or promotion after the window ends (D-04 "Behavior after a violation"). If arrivals keep falling inside the window, the return is delayed, and that delay is a measurement item.
- Each L2 `task_id` is assigned a fixed SHM slot for the lifetime of the daemon, so a workload whose names change often can exhaust the slots.
- This is a design stage, and it is not implemented. The design assumptions and numbers are candidates until measured.

### 3.3 Follow-up Decisions Needed (P2 or OI)

| # | Item | Document |
|:--|:--|:--|
| F-1 | OI-9: PoC of the rescheduling trigger for backup replenishment (BPF SYSCALL + `scx_bpf_kick_cpu` availability) | Accepted condition of this DDR |
| F-2 | Update DDR-007 §4.5 (periodic grid replenishment description, Replenisher map updates) for alignment with D-04 and D-07 | DDR-007 |
| F-3 | **Withdrawn (revised 2026-11-11).** System-call classification at sleep boundaries is superseded by explicit Job boundaries (D-18), so the item is removed. The remaining P2 items are tracked in the EDF transition (D-08) and the precise admission formula (D-09) | D-18, D-08, D-09 |
| F-4 | Alignment updates for DDR-002 §7, DDR-005 User App Interface (D-14), DDR-011 incremental update, and DDR-012 asynchronous fault delivery | Each DDR |
| F-5 | Update the Rust-only development description in `doc/docs/structure.md` | docs |
| F-6 | Fix the TIM candidate values (RPi5 measurement): TIM-001/002 (L_wake), $L_{post}$ (reference poster configuration, A-10), $\delta_{ovr}$, $DL\_SCAN\_PERIOD\_US$, detection and reporting latency, and fix the reference poster configuration | Test strategy |
| F-7 | Explicit declaration of multiple Event sources per task (REQ-008 OI-16), common guidance for the app-side input adapter | DDR-008 revision |
| F-8 | **Withdrawn (revised 2026-11-11).** Kernel identification of the marker wait point (the `sched_yield()` flag and BPF `ops.yield`) is superseded by explicit Job boundaries (D-18). Identification itself is not needed in P1 | D-18, D-02 |
| F-9 | Post-violation window return policy: measurement and review of the alternative of aligning the promoted window to the Job's activation time (D-04 alternatives table) | DDR-008 revision |
| F-10 | DeadlineMonitor wake-up alternatives: compare a single timer for the earliest absolute deadline against the periodic scan as the baseline, including the race between new-post notification and reset. TIM-009; cost and jitter are to be confirmed by the OI-22 PoC before design review, and it is not introduced automatically | DDR-008 revision (D-20) |
| F-11 | Place the queue depth and payload size per source in the workload declaration (proto change, approval required) | DDR-008, proto |
| F-12 | Possibility of merging the DeadlineMonitor and Replenisher threads (after measuring priorities and periods) | D-07, D-20 |
| F-13 | Multiple posters, payload handles and release notification, large payloads | DDR-008 revision (D-19) |
| F-14 | Per-source isolation in SHM, daemon heartbeat (detection during downtime) | DDR-008 revision (D-20) |

### 3.4 Open Items (approval, measurement, or confirmation required)

- **Numeric candidate values (agreement and measurement required)**: `TTSCHED_EVT_QDEPTH` 8, `TTSCHED_EVT_PAYLOAD_MAX` 64 B, `DL_SCAN_PERIOD_US` 1000, detection latency upper bound 2 ms, reporting latency upper bound 200 ms, $L_{post}$ 50 µs, $L_{del\_max}$ 100 µs, $\delta_{ovr}$ 1 ms (HZ=1000), `tol` 50 µs, `tol_mit` 0, SHM slot count 64, object size limit 256 KiB, DeadlineMonitor priority 97 (Replenisher 98), back-off 10 ms.
- **Approval**: The public API (`ttsched_evt_*`), SHM v2, queue constants, and all protobuf changes (D-03, D-11, D-19).
- **PoC**: The cost and jitter of the 1 kHz DeadlineMonitor scan (TT load and selected CPU conditions), and the need for event-based wake-up (F-10, F-6).
- **Interpretation check 1**: The definition of the completion time $t_{end}$ and the race rule with monitoring (D-20 rule 6). Confirm whether readers interpret "determination based on the actual completion time" in this way.
- **Interpretation check 2**: Entries closed by TIMPANI before the deadline (CANCELLED, ABANDONED) are not misses (D-20 rule 7). This is an interpretation that extends the sentence "explicit discard or cancellation before the deadline" in P1-008 §2 to closing by TIMPANI.
- **Policy for continuing or cancelling a failed Job** (D-20 rule 8).
- **OI-9 PoC** (F-1) and **DDR-007 alignment** (F-2).
- **L2 `runtime` (C) measurement and margin** (OI-23): Agree on the measurement procedure for CPU demand per Job, the measurement conditions such as input coverage and the first Job after idle and interference, and the declared margin ratio (HLD-008 §8.2, H-19). `budget_used_us` is in units of execution intervals, so a Job that blocks can be split into several intervals, or consecutive Jobs can be merged into one interval. For this reason it is not directly regarded as C per Job. The measured maximum is not a proof of worst-case execution time. Budget enforcement limits CPU interference caused by underestimation of C, but the deadline protection of other tasks is limited to the range in which the admission assumptions of D-09 and the overrun upper bound hold, and it does not mean isolation of shared-resource or data effects.
- **Re-posted `t_arr` at the rebinding boundary and responsibility for duplicate handling** (OI-24): If the same input is posted again, it becomes a separate Event with a new `gen` (D-17, D-19). Which basis applies, keeping the original arrival time or using the re-posting time, and where the responsibility for preventing duplicate handling lies, must be fixed by contract (HLD-008 §8.1, H-20). With the same D, the original time keeps a deadline that includes the reconnection delay, but the re-posting time moves the deadline later. If D changes, the deadline is computed with the new configuration even when the original time is used, so it is not the same deadline as the previous Event.
- **Representation of `hyperperiod_us` in a table without TT and its update boundary** (D-22, OI-25): Agree before implementation on the representation (keeping the non-zero value of FR-O-015, or allowing 0 or absence) and the update boundary at the first addition and final removal of TT. This item affects table validation, Timer Master, and the update contract. DDR-011 Phase 0 handles only L1 TT updates at fixed E/H and passes CBS updates to Phase 1, so this item is an input to the review of [DDR-011 Phase 1](DDR-011-runtime-table-update.md#10-deferrals-and-review-gate).
- **Where the A-number references are defined**: The A1~A25 notations in parentheses in DDR-008, SRS-008, LLD-008, and GUIDE-008 (for example, A8 and A14) have no table that defines the numbers within the 008 document set. The content of each number is to be listed in a table, or the notations are to be replaced with section references. These notations are different from the assumptions A-01~A-10 of REQ-008 and SRS-008.

---

### 3.5 Reflection of the 2026-11-12 Weekly and Phase Determination

**P1/P2 scope decided by the Project Leader (author) during the design review:** P1 defines the common Event/Job and implements the basic environment of an Event-Driven Workload in which TIMPANI can accept, deliver, process, terminate, and monitor deadlines of Events. The common queue, identification, and recovery contract, CBS budget enforcement, and execution without TT are also included in this environment. The concretization of the TT slot boundary contract and termination enforcement, and the cause analysis of CBS deadline failures and cause-specific recovery, are separated into P2. The arrival-based deadline detection, counting, and basic reporting of the existing P1 are not reduced.

**Basis for the decision:** In the Copilot design review conversation, the Project Leader (author) decided to distinguish P1 as the common Event/Job basic execution environment from P2, which covers the concretization of the TT slot range and the cause analysis of CBS deadline failures. The detail items such as the common queue, recovery, and CBS-only are explanations that connect the existing design with this scope decision. The decision date has not been confirmed, so it is distinguished from the document edit date. The record of the review conversation is not in the repository, and this section is the record of the decision.

**Additional scope decision (Project Leader (author), in this design review conversation):** The selection and management of scheduling policy for the input adapter and other app threads are excluded from P1, but the `SCHED_EXT`/BPF CBS enforcement of registered L2 workers and the placement and environment verification are retained (D-24). The deployment goal of isolating only workers and the responsibility for TID moves are subject to review, and they are not approved implementation decisions.

**Background material:** [Weekly 2026-11-12 and seokheehan follow-up answer in Discussion #4](https://github.com/eclipse-timpani/timpani26/discussions/4). This thread is background for the scope review, and it is not itself the recorded source of the decision above. The questions on the agenda are not treated as approved solutions. P1/P2 below are **stages of Event-Job support**, and they differ from the past development Phase numbers of other DDRs. This table reflects the Project Leader (author)'s scope decision, and the approval of the concrete schedule and implementation method and the target verification are performed separately.

| Meeting item | P1 basic environment / guarantee premise | P2 or out of scope | Owner |
|:--|:--|:--|:--|
| 1-a Forced termination of TT slots | Verify the basic Event/CBS environment under the assumption of normal TT execution. Isolation up to TT overrun is not guaranteed | **P2:** Verify slot range, termination enforcement, permitted delay, incomplete-TT policy, and mixed time isolation | DDR-007 §4.3.1 (lead), DDR-005 |
| 1-b CBS misses a deadline because of TT overrun | Keep deadline-failure detection, counting, and basic reporting, and do not claim cause confirmation | **P2:** Distinguish TT interference from CBS's own defect, protect affected tasks, and cause-specific recovery policy | D-23, DDR-012 §4.4 |
| 1-c CBS policy and Timer Master preemption | CBS is SCHED_EXT, Timer Master is SCHED_FIFO. Confirm DEADLINE layer and fair server interference as premises for P1 measurement | No conclusion to change Timer Master to SCHED_DEADLINE. Root-cause countermeasures are a kernel and platform investigation | Supplement of the execution environment in DDR-005, DDR-006 §4 |
| 1-d DMISS cause information | Keep the existing basic observation and statistics | **P2:** TT/CBS record correlation analysis, cause classification, intelligent analysis, and automatic cause-specific recovery | D-23, D-10/D-16, DDR-012 |
| 1-e App dependency on TIMPANI | Localize the dependency with an adapter or wrapper, and keep explicit Event/Job connection | Fully unmodified apps and automatic inference are subjects of investigation | D-24 |
| 1-f CBS execution without TT | Required in P1. Replenishment, dispatch, and monitoring are independent of TT and do not use the hyperperiod value (design property, to be confirmed by AC-008-17) | Table field is based on FR-O-015. Representation change (for example, allowing 0) and the update boundary are open (OI-25) | D-22, DDR-011 §10 (Phase 1) |
| 1-g L1→L2 data pipeline | Radar L1→Event→FCW L2 can be verified as a P1 connection example. The existing L1/L2 classification is retained | End-to-end deadline, observation, and inter-node guarantees for the whole pipeline are a DDR-009 topic and outside the 008 P1 guarantee. There is no DDR-009 canonical document in the current checkout | HLD-008 §8, future DDR-009 |
| 2 vsomeip policy and CPU move failure | Collect evidence on PID/TID, cgroup, effective cpuset, policy, online CPUs, and container configuration. Environment verification is P1 | Unconditional promotion to FIFO and bypassing constraints with root are prohibited. Responsibility of the system deployment configuration | D-24, DDR-003 §2 |

**Additional verification scenarios (not run):** P1 verifies replenishment after CBS-only idle (AC-008-17), the basic Event/Job lifecycle and arrival-based deadline monitoring, explicit failure of incorrect cpuset placement (AC-008-19), and the L1→L2 connection example. Control thread jitter is measured as a premise of the execution environment. P2 verifies the next TT and CBS protection, affected-task records, and cause classification when TT exceeds its boundary. Performing the L1→L2 example does not mean that the end-to-end deadline guarantee has been verified.

**Document alignment (v3.2):** The P1 requirements and acceptance criteria of D-22 and D-24 were added to the subordinate chain (REQ-008 v5.1: CBS-007, CBS-008, A-09, AC-008-17 to 19, OI-23 to 25. SRS-008 v4.1: FR-O-022, FR-N-032, FR-N-033, FR-B-025. HLD-008 v5.2, LLD-008 v5.1). D-23 is P2, so it has no new ID. The API, proto, SHM, and the interpretation of the D-20 completion race were not changed. The added requirements are also design proposals and are before implementation and verification.

---

## 4. Requirement Mapping (Decision → Requirement)

| Decision | Satisfies REQ-008 | Satisfies SRS-008 |
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
| D-13 | (out-of-scope stated explicitly) | — |
| D-14 | DET-003 | FR-L-003 |
| D-15 | NFR-003 | NFR-006/007 |
| D-16 | OBS-001/002, OBS-004, TIM-008 | FR-N-020~024/026, FR-N-030, IF-V-001~004 |
| D-17 | RTU-001/004 (AC-008-15) | FR-O-005, FR-N-005/006, FR-N-029 |
| D-18 | MOD-009, DET-007 (AC-008-13) | FR-L-010, FR-L-011, FR-B-024 |
| D-19 | MOD-010, IF-007 (AC-008-15) | FR-L-009, FR-L-012, FR-N-029 |
| D-20 | PRT-008, PRT-009, TIM-009, TIM-010, TIM-011 (AC-008-14) | FR-N-027, FR-N-028, FR-N-031, TMG-014, TMG-015, TMG-016 |
| D-21 | MOD-011 | FR-G-003, FR-L-003 |
| D-22 | CBS-007, RTU-001 (AC-008-17, AC-008-18), §10-16, OI-25 | FR-O-015, FR-O-022, FR-N-032, FR-B-025 |
| D-23 | No new ID (P2). The flag wording of UC-06 step 3 and PRT-007 is corrected (the flag is not a cause determination). Added to the P2 scope of REQ-008 §0.1 | P2 cause-analysis and recovery acceptance criteria are defined in DDR-012 and the P2 revision |
| D-24 | MOD-006, MOD-011, IF-005, CBS-008, A-09, A-10, TIM-011 (AC-008-19) | FR-N-033 |

---

## 5. Affected Components

| Component | Change summary (details: LLD-008) |
|:--|:--|
| `timpani-o/proto/schedinfo.proto`, `node_control.proto` | Add the D-11 fields (all require approval) |
| `timpani-o/src/schedinfo_service.cpp` | Read the MIT field and interpret the transition, parameter validation (`D ≤ MIT`, `T ≤ MIT`), restore on rejection |
| `timpani-o/src/global_scheduler.{h,cpp}` | Separate `mit_us`, compute $B_{tt}$, D-09 admission formula (`L_del_max`, `δ_ovr`), reject minimum Gap, fix the hyperperiod mismatch, publish the EVENT_DRIVEN layer, make the CPU explicit |
| `timpani-o/src/orchestrator_service.cpp`, `fault_client.cpp` | Preserve mapping of fault types and flags, pass the new fields |
| `timpani-n/src/bpf/maps.h`, `timpani.bpf.c` | Extend TaskMeta (execution-interval state, `bind_gen`), per-CPU DSQ, execution-interval state machine, BUDGET_EXCEED transition emission, `job_trace_rb`, `trace_enabled`. Activation-based MIT check, L2 DMISS, and `MIT_VIOLATION` emission are removed |
| `timpani-n/src/main.cpp`, `bpf_loader.{h,cpp}` | EVENT_DRIVEN layer handling, MIT and CPU in CBS registration, re-registration with state preservation, activation and deactivation order |
| `timpani-n/src/task_registry.{h,cpp}` | Revive D-12 (unresolved list, rescan, disappearance detection), `bind_gen` check |
| `timpani-n/src/replenisher.{h,cpp}` (new) | D-07. Rescheduling trigger that does not write server state |
| `timpani-n/src/deadline_monitor.{h,cpp}` (new) | D-20. SCHED_FIFO thread, periodic scan, MISSED CAS, SPSC delivery of miss records, `CbsState` lookup at detection |
| `timpani-n/src/job_trace.{h,cpp}` (new) | D-16. Job log collection, execution-interval record collection, Job record output |
| `timpani-n/src/event_source_monitor.{h,cpp}` (new) | D-03, D-05, D-16. SHM cumulative counter polling, activation baseline, mandatory statistics output, creation of `MIT_VIOLATION` from the MIT increment |
| `timpani-n/src/fault_monitor.{h,cpp}` | Threshold rule of the counting path (`missed`, `mit_viol` increments), duplicate removal by `(task, gen, seq)` in the detail path, flag filling |
| `timpani-n/src/timer_master.{h,cpp}` | SHM v2, slot allocation, activation, deactivation, and startup cleanup (closing sweep), `gen` creation, jitter sample trace output |
| `sample-apps/src/libttsched.{h,c}` | SHM v2, `ttsched_evt_open/post/wait/done`, `ttsched_init()` reattachment, back-off |
| `examples/someip/*` | Shrink and retire `libtimpani_rt`. Shrink the input adapter to calls to `ttsched_evt_post()` (REQ-008 §12) |
| `scripts/mock_pullpiri.yaml`, `timpani-o/scripts/grpcurl/*.json` | Use the MIT field (existing fields retained) |
| Documents | Alignment of DDR-002 §7, DDR-005, DDR-006 (Replenisher and DeadlineMonitor in the RT thread table), DDR-007 §4.5 and §7, DDR-011, DDR-012, `structure.md` |

---

## 6. Alignment with Existing DDRs

| DDR | Impact |
|:--|:--|
| DDR-001 | None (L2 = Sporadic+Safety → CBS unchanged). The `TaskSpec.min_inter_arrival_us` design is realized in the proto |
| DDR-002 | §7 dual mode → updated to "not adopted (DDR-007/008)" |
| DDR-003 | Reflects the `TaskSpec` MIT field and the FaultType and FaultInfo extensions |
| DDR-004 | Add the D-09 admission formula (`L_del_max`, `δ_ovr`) to the §8.C CBS budget calculation |
| DDR-005 | User App Interface: Revise the L2 requirement for `ttsched_wait_next_period` as in D-14. The worker's `ttsched_evt_wait/done` pair (D-18). TaskMeta extension and `bind_gen`. Update the DSQ layout (per CPU) |
| DDR-006 | Add Replenisher and DeadlineMonitor to the RT thread table (realizes the item announced in DDR-007 §10). Both threads prohibit gRPC and allocation |
| DDR-007 | §4.5 replenishment semantics changed to the activation-based window (D-04), §4.5.3 Backup made mandatory (only BPF updates state, D-07), §4.2 DSQ layout per CPU, §7 Fault table gets MIT_VIOLATION (arrival-based source, D-10) |
| DDR-011 | Phase 0 (L1 TT update, fixed E/H) has no effect on 008 P1. CBS updates and tables without TT (OI-25) are reflected in DDR-011 Phase 1 (§10) |
| DDR-012 | Separation of the fault counting path and detail path (D-10). DMISS threshold rule retained. TT DMISS path unchanged |

---

## 7. Revision History

| Version | Date | Change |
|:--|:--|:--|
| v1 | 2026-11-10 | Initial Proposal. Design decisions D-01 to D-17 based on commit `9209bc5`. Written without a version label |
| v2 | 2026-11-11 | Aligned with the P1 Event-Job contract amendment (Issue #53, [P1-008](P1-008-event-job-contract-amendment.md)). Revised D-01, D-02, D-03, D-04, D-05, D-09, D-10, D-11, D-14, D-16, and D-17. D-07 and D-08 were supplemented only for terminology and thread alignment. D-18 to D-21 are new. F-3 and F-8 withdrawn; F-10 to F-14 are new. Updated the summary, consequences, mapping table, affected components, and alignment table. Design stage; there is no implementation or verification. Review reflection (2026-11-11): corrected the meaning of the queue occupancy limit, the counting limit on poster death, the party that sets MISSED, generation comparison, the seq check in done, the description of window budget return, the L_del_max term (TIM-011, TIM-001), and the distinction between the proto count field and existing cumulative fields |
| v3 | 2026-11-13 | Reflected Weekly 2026-11-12 (Discussion #4). Added to D-22 to D-24 and §3.5: CBS-only independent execution, separation of failure fact from cause and recovery, app abstraction and cpuset environment premises, and P1/follow-up/other-DDR responsibilities. TT boundary enforcement is reviewed in DDR-007/005, table boundaries in DDR-011, attribution and recovery in DDR-012. Not an approval of mechanisms, numbers, or public interfaces |
| v3.1 | 2026-11-16 | Reflected the review discussion of the overview (OVERVIEW-008). Added two items to the §3.4 open items: the L2 `runtime` (C) measurement procedure and margin, and the re-posted `t_arr` basis at the rebinding boundary. No change to design decisions (D-01 to D-24) |
| v3.2 | 2026-11-16 | Review reflection (correction of the D-22 hyperperiod description and alignment with FR-O-015, a note on where the §3.5 agreement record is kept, traceability IDs of the subordinate chain, list rendering of D-20 rule 6). Change in design decision content: the D-22 hyperperiod description (CBS execution rule, based on the table field) and the addition of the requirement item in D-24 |
| v3.3 | 2026-11-18 | Reflected the 2026-11-17 commit (3e307da): unified approval terms to admission and admission formula. Changed the budget-only L2 class to the responsibility split of CBS budget management and Event/Job management, and did not add additional flags (D-21). Within the worker's wait, the Job start is confirmed, and there is no separate public acquire API. Added the P1 policy boundary and the review of worker-only isolation, and compared the timer alternative in F-10 and OI-22. This decision was made by the Project Leader (author). The $L_{post}$ bound (TIM-011) is kept as a requirement that TIMPANI respects in the reference poster configuration, and the deployment's satisfaction of that configuration is added as assumption A-10 (Project Leader (author) decision). The starting point of $L_{post}$ was unified to $t_{arr}$. List rendering and the posting terminology were corrected. The formal documents are REQ-008 v5.2, SRS-008 v4.2, DDR-008 v3.3, HLD-008 v5.3, and LLD-008 v5.2 |
| v3.4 | 2026-11-26 | Term clean-up (Project Leader (author) decision): the side that calls post is the poster, and the side that creates the input to be posted to an Event source is the producer. "Posting party", and "producer" where it meant the thread that calls post, were changed to "poster" (reference producer configuration → reference poster configuration). In D-03, the subject of the sentence about where the poster runs was corrected to the poster. Final review corrections: $\delta_{ovr}$ was added to the consumption bounds of D-04 (P-b) and (P-c). The worker Job log record of D-16 was corrected to `miss_src`, and D-16 now states that the `flags` and `missed` of the output record are computed by FaultMonitor. D-18 also states that `flags` is computed by FaultMonitor. The LATE_COMPLETION row of the D-10 flag table now notes that it is left as `miss_src` in the Job log. The place where the A-number references are defined was added to §3.4 as an open issue. Other decision content and numbers are unchanged. The terms in the earlier history rows were also changed to the new terms. The formal documents are REQ-008 v5.3, SRS-008 v4.3, DDR-008 v3.4, HLD-008 v5.4, and LLD-008 v5.3 |
