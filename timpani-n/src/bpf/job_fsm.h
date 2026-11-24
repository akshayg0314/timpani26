// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

/* L2 execution-interval state machine and CBS replenishment-window rules.
 *
 * Pure functions: no BPF helper, no map access, no event emission. The same
 * header is compiled into the BPF scheduler and into the host unit tests
 * (timpani-n/tests/test_job_fsm.cpp), so the rules are tested off target.
 *
 * Terms:
 *   execution interval  kernel activation -> first run -> voluntary sleep.
 *                       Not a Job; BPF neither counts Jobs nor judges L2
 *                       deadlines, and it does no inter-arrival check.
 *   window              [t, replenish_at_ns): the server may consume at most
 *                       budget_us of CPU time inside it.
 *
 * Budget rules (the only places remaining_us is refilled are 1 and 3):
 *   1. Activation: refill and open a window from t_act if the previous window
 *      has ended (within CBS_ACT_TOL_US), otherwise keep both.
 *   2. Charging saturates remaining_us at 0. A server with no budget that is
 *      still runnable is throttled at enqueue; exhausting the budget and then
 *      sleeping starts no throttle episode.
 *   3. Promotion: a server without budget is refilled once
 *      now >= replenish_at_ns and its new window starts at now. That happens
 *      in the promotion scan, or at enqueue if the window is already over.
 *   4. An active, not throttled server is never refilled, even after its
 *      window end has passed.
 */

#pragma once

#include "maps.h"

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

#define CBS_NS_PER_US 1000ULL
#define CBS_US_MAX 0xffffffffULL

/* Result of cbs_on_enqueue() */
#define CBS_ENQ_READY 0          /* has budget: goes to the ready queue */
#define CBS_ENQ_THROTTLE_ENTER 1 /* new throttle episode: report it once */
#define CBS_ENQ_THROTTLE_STAY 2  /* already throttled: no new report */

/* Wakeup of an L2 task. Returns 1 if it starts an execution interval
 * (WAITING -> ACTIVE), 0 for a requeue inside the current interval. */
static __always_inline int job_fsm_on_runnable(struct TaskMeta* m, __u64 now)
{
    if (m->job_state != JOB_WAITING)
        return 0;

    m->job_state = JOB_ACTIVE;
    m->activation_ns = now;
    m->queue_key_ns = now;
    m->t_start_ns = 0;
    m->budget_used_us = 0;
    m->job_flags = 0;
    m->run_seq++;
    return 1;
}

/* Rule 1. Call at activation only. Returns 1 if the budget was refilled, 0 if
 * the previous window has not ended (budget and window are kept). */
static __always_inline int cbs_on_activation(struct CbsState* c, __u64 t_act)
{
    if (t_act + (__u64)CBS_ACT_TOL_US * CBS_NS_PER_US < c->replenish_at_ns)
        return 0;

    c->remaining_us = c->budget_us;
    c->exec_frac_ns = 0;
    c->replenish_at_ns = t_act + (__u64)c->period_us * CBS_NS_PER_US;
    return 1;
}

/* Start of an execution interval with its server: job_fsm_on_runnable() plus
 * rule 1. An interval that starts without a refill is marked CARRYOVER.
 * `c` may be NULL (server not registered yet). Returns 1 if an interval
 * started. */
static __always_inline int job_fsm_start_interval(struct TaskMeta* m,
                                                  struct CbsState* c,
                                                  __u64 now)
{
    if (!job_fsm_on_runnable(m, now))
        return 0;

    if (c && !cbs_on_activation(c, now))
        m->job_flags |= JOB_FLAG_CARRYOVER;
    return 1;
}

/* First run of the current interval. */
static __always_inline void job_fsm_on_running(struct TaskMeta* m, __u64 now)
{
    if (m->job_state == JOB_ACTIVE && m->t_start_ns == 0)
        m->t_start_ns = now;
}

/* Rule 2, charging part. Charges the CPU time since exec_start_ns and returns
 * it in us (saturated at CBS_US_MAX). The sub-us remainder is carried to the
 * next charge inside the same window; a refill drops it, so less than 1 us
 * per window stays uncharged. Exhaustion does not start a throttle episode. */
static __always_inline __u32 cbs_charge(struct CbsState* c, __u64 now)
{
    __u64 total_ns;
    __u64 used_us;

    if (c->exec_start_ns == 0)
        return 0;

    total_ns = (now - c->exec_start_ns) + c->exec_frac_ns;
    used_us = total_ns / CBS_NS_PER_US;
    c->exec_frac_ns = (__u32)(total_ns % CBS_NS_PER_US);
    c->exec_start_ns = 0;

    if (used_us >= c->remaining_us)
        c->remaining_us = 0;
    else
        c->remaining_us -= (__u32)used_us;
    return used_us > CBS_US_MAX ? (__u32)CBS_US_MAX : (__u32)used_us;
}

/* Rule 3. Returns 1 if the server may run again. No tolerance is applied. */
static __always_inline int cbs_try_promote(struct CbsState* c, __u64 now)
{
    if (c->remaining_us != 0) {
        c->throttled = 0;
        return 1;
    }
    if (now < c->replenish_at_ns)
        return 0;

    c->remaining_us = c->budget_us;
    c->exec_frac_ns = 0;
    c->throttled = 0;
    c->replenish_at_ns = now + (__u64)c->period_us * CBS_NS_PER_US;
    return 1;
}

/* Records the promotion time as the task's queue key. A promoted task goes
 * behind the tasks already waiting. The ready queue is FIFO today, so the
 * caller gets that order by appending; the key is for a ranked queue. */
static __always_inline void job_fsm_on_promote(struct TaskMeta* m, __u64 now)
{
    m->queue_key_ns = now;
}

/* Rule 2, enforcement part. Call when the task is about to be queued.
 * Returns CBS_ENQ_*; CBS_ENQ_THROTTLE_ENTER is returned once per throttle
 * episode. A server that is out of budget when its window has already ended
 * has nothing to wait for: rule 3 is applied on the spot and the task is
 * ready. That is not a throttle episode. */
static __always_inline int cbs_on_enqueue(struct CbsState* c,
                                          struct TaskMeta* m, __u64 now)
{
    if (c->remaining_us == 0 && now >= c->replenish_at_ns &&
        cbs_try_promote(c, now))
        job_fsm_on_promote(m, now);

    if (c->remaining_us != 0) {
        c->throttled = 0;
        return CBS_ENQ_READY;
    }

    m->job_flags |= JOB_FLAG_THROTTLED;
    if (c->throttled)
        return CBS_ENQ_THROTTLE_STAY;

    c->throttled = 1;
    c->throttle_episodes++;
    return CBS_ENQ_THROTTLE_ENTER;
}

/* Transitional L2 deadline check, kept from the scheduler before the
 * event-job work. Call when the task stops running: the interval misses if
 * that stop comes after t_act + deadline_us. It is judged from the kernel
 * activation, not from the Event arrival, and goes away when the node-side
 * deadline monitor takes over (DDR-008 D-20). For a task that was already
 * runnable when it was bound, t_act is the bind time. Returns 1 at most once
 * per execution interval (JOB_FLAG_DMISS). */
static __always_inline int job_fsm_legacy_deadline_missed(struct TaskMeta* m,
                                                          __u32 deadline_us,
                                                          __u64 now)
{
    if (m->job_state != JOB_ACTIVE || deadline_us == 0)
        return 0;
    if (m->job_flags & JOB_FLAG_DMISS)
        return 0;
    if (now <= m->activation_ns + (__u64)deadline_us * CBS_NS_PER_US)
        return 0;

    m->job_flags |= JOB_FLAG_DMISS;
    return 1;
}

/* Voluntary sleep. Returns 1 when an execution interval ended. */
static __always_inline int job_fsm_on_quiescent(struct TaskMeta* m)
{
    if (m->job_state != JOB_ACTIVE)
        return 0;

    m->job_state = JOB_WAITING;
    return 1;
}
