// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLOT_NONE 0xFFFFFFFF

#define SCHED_TYPE_TT 0
#define SCHED_TYPE_CBS 1
#define SCHED_TYPE_BEST_EFFORT 2

/* L2 execution-interval state (TaskMeta.job_state). An execution interval is
 * kernel activation -> first run -> voluntary sleep. It is not a Job: Job
 * boundaries are reported by the worker, not observed by BPF.
 * JOB_WAITING = outside an interval (asleep), JOB_ACTIVE = inside one. */
#define JOB_WAITING 0
#define JOB_ACTIVE 1

/* Flags of the current execution interval (TaskMeta.job_flags).
 * Bits 0, 2 and 3 are retired and must not be reused. Bit 4 keeps its DMISS
 * meaning while the BPF deadline check exists and is retired with it. */
#define JOB_FLAG_THROTTLED (1u << 1) /* server was throttled in this interval */
#define JOB_FLAG_DMISS (1u << 4)     /* transitional, see job_fsm.h */
#define JOB_FLAG_CARRYOVER (1u << 6) /* interval started without a refill */

/* Tolerance of the activation refill rule only (see cbs_on_activation() in
 * job_fsm.h). Candidate value, to be confirmed by measurement. */
#define CBS_ACT_TOL_US 50u

struct PartitionInfo {
    __u64 task_id_hash;
    __u64 cgroup_id;
    __u8  layer;
    __u8  asil_level;
    __u64 cpu_mask;
};

struct TtSlotKey {
    __u32 cpu;
    __u32 slot_idx;
};

struct TtSlotBpf {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 offset_us;
    __u32 duration_us;
    __u32 deadline_us;
    __u32 cpu;
};

struct CbsState {
    __u64 task_id_hash;
    __u32 budget_us;
    __u32 period_us;          /* T_s: length of a replenishment window */
    __u32 remaining_us;
    __u32 deadline_us;
    __u64 exec_start_ns;
    __u64 replenish_at_ns;    /* end of the current window; 0 = none yet */
    __u32 exec_frac_ns;       /* sub-us carry between charges (0..999) */
    __u32 throttle_episodes;  /* entries into the throttled state */
    __u32 throttled;          /* 1 while the task waits for the window end */
    __u32 _pad;
};

struct TaskMeta {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 assigned_cpu;       /* CPU this TT/CBS task is scheduled on */
    __u32 slot_duration_us;   /* TT slot duration; 0 for non-TT tasks */
    __u8  scheduling_type;
    __u8  layer;
    __u8  tt_runnable;
    __u8  job_state;          /* L2 only: JOB_WAITING / JOB_ACTIVE */
    __u64 activation_ns;      /* L2: t_act of the current interval */
    __u64 cgroup_id;
    /* L2 execution-interval bookkeeping, written by job_fsm.h */
    __u32 job_flags;          /* JOB_FLAG_*, reset at interval start */
    __u32 run_seq;            /* interval counter */
    __u64 t_start_ns;         /* first run after activation; 0 until set */
    __u64 queue_key_ns;       /* t_act, or promotion time (FIFO queue today) */
    __u32 budget_used_us;     /* per interval */
    __u32 _pad;
};

#define FAULT_DMISS 0
#define FAULT_BUDGET_EXCEED 1

struct FaultEvent {
    __u64 workload_id_hash;
    __u64 task_id_hash;
    __u32 cpu;
    __u64 expected_deadline_ns;
    __u64 actual_completion_ns;
    __u8  fault_type;
};

#ifdef __cplusplus
}
#endif
