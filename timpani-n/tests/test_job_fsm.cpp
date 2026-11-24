// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

// Host tests for timpani-n/src/bpf/job_fsm.h: the L2 execution-interval state
// machine and the CBS replenishment-window rules shared with the BPF
// scheduler. Times are in ns unless a name says otherwise.

#include <cstddef>

#include <gtest/gtest.h>

#include "job_fsm.h"

namespace
{

constexpr __u64 NS_PER_US = 1000ULL;
constexpr __u64 NS_PER_MS = 1000000ULL;
constexpr __u32 BUDGET_US = 2000;   // C_s = 2 ms
constexpr __u32 PERIOD_US = 18000;  // T_s = 18 ms
constexpr __u64 PERIOD_NS = PERIOD_US * NS_PER_US;
constexpr __u64 TOL_NS = CBS_ACT_TOL_US * NS_PER_US;

// A point in time, `value` ms after an arbitrary non-zero origin. The origin
// is not 0 because 0 means "unset" in exec_start_ns and replenish_at_ns.
constexpr __u64 TIME_ORIGIN_NS = 100 * NS_PER_MS;

constexpr __u64 at_us(__u64 us)
{
    return TIME_ORIGIN_NS + us * NS_PER_US;
}

constexpr __u64 at(__u64 ms)
{
    return TIME_ORIGIN_NS + ms * NS_PER_MS;
}

TaskMeta make_waiting_task()
{
    TaskMeta meta = {};
    meta.scheduling_type = SCHED_TYPE_CBS;
    meta.job_state = JOB_WAITING;
    return meta;
}

CbsState make_server()
{
    CbsState server = {};
    server.budget_us = BUDGET_US;
    server.period_us = PERIOD_US;
    return server;
}

// Runs the server on CPU for `run_ns` starting at `start_ns` and charges it.
__u32 run_and_charge(CbsState* server, __u64 start_ns, __u64 run_ns)
{
    server->exec_start_ns = start_ns;
    return cbs_charge(server, start_ns + run_ns);
}

}  // namespace

// ---- Map value layout shared by BPF and user space ------------------------

// Both sides build from maps.h. A size change here must be deliberate: it
// changes the BPF map value size.
TEST(MapLayoutTest, CbsStateHasNoImplicitPadding)
{
    EXPECT_EQ(sizeof(CbsState), 56u);
    EXPECT_EQ(offsetof(CbsState, deadline_us), 20u);
    EXPECT_EQ(offsetof(CbsState, exec_start_ns), 24u);
    EXPECT_EQ(offsetof(CbsState, replenish_at_ns), 32u);
    EXPECT_EQ(offsetof(CbsState, exec_frac_ns), 40u);
    EXPECT_EQ(offsetof(CbsState, throttle_episodes), 44u);
    EXPECT_EQ(offsetof(CbsState, throttled), 48u);
}

TEST(MapLayoutTest, TaskMetaKeepsExistingFieldOffsets)
{
    EXPECT_EQ(sizeof(TaskMeta), 80u);
    EXPECT_EQ(offsetof(TaskMeta, activation_ns), 32u);
    EXPECT_EQ(offsetof(TaskMeta, cgroup_id), 40u);
    EXPECT_EQ(offsetof(TaskMeta, job_flags), 48u);
    EXPECT_EQ(offsetof(TaskMeta, run_seq), 52u);
    EXPECT_EQ(offsetof(TaskMeta, t_start_ns), 56u);
    EXPECT_EQ(offsetof(TaskMeta, queue_key_ns), 64u);
    EXPECT_EQ(offsetof(TaskMeta, budget_used_us), 72u);
}

// ---- Execution interval (FR-B-001..003) -----------------------------------

TEST(JobFsmTest, RunnableStartsIntervalOnlyFromWaiting)
{
    TaskMeta meta = make_waiting_task();
    meta.job_flags = JOB_FLAG_THROTTLED;
    meta.budget_used_us = 123;
    meta.t_start_ns = 77;

    const int started = job_fsm_on_runnable(&meta, at(5));

    EXPECT_EQ(started, 1);
    EXPECT_EQ(meta.job_state, JOB_ACTIVE);
    EXPECT_EQ(meta.activation_ns, at(5));
    EXPECT_EQ(meta.queue_key_ns, at(5));
    EXPECT_EQ(meta.run_seq, 1u);
    EXPECT_EQ(meta.job_flags, 0u);
    EXPECT_EQ(meta.budget_used_us, 0u);
    EXPECT_EQ(meta.t_start_ns, 0u);
}

TEST(JobFsmTest, PreemptionInsideIntervalDoesNotRestart)
{
    TaskMeta meta = make_waiting_task();
    job_fsm_on_runnable(&meta, at(5));
    meta.budget_used_us = 400;
    meta.job_flags = JOB_FLAG_CARRYOVER;

    const int started = job_fsm_on_runnable(&meta, at(6));

    EXPECT_EQ(started, 0);
    EXPECT_EQ(meta.activation_ns, at(5));
    EXPECT_EQ(meta.run_seq, 1u);
    EXPECT_EQ(meta.budget_used_us, 400u);
    EXPECT_EQ(meta.job_flags, JOB_FLAG_CARRYOVER);
}

TEST(JobFsmTest, SleepEndsInterval)
{
    TaskMeta meta = make_waiting_task();
    job_fsm_on_runnable(&meta, at(5));

    const int ended = job_fsm_on_quiescent(&meta);
    const int ended_again = job_fsm_on_quiescent(&meta);

    EXPECT_EQ(ended, 1);
    EXPECT_EQ(meta.job_state, JOB_WAITING);
    EXPECT_EQ(ended_again, 0);
}

TEST(JobFsmTest, BlockingResumeStartsNewInterval)
{
    TaskMeta meta = make_waiting_task();
    job_fsm_on_runnable(&meta, at(5));
    job_fsm_on_quiescent(&meta);

    const int started = job_fsm_on_runnable(&meta, at(7));

    EXPECT_EQ(started, 1);
    EXPECT_EQ(meta.run_seq, 2u);
    EXPECT_EQ(meta.activation_ns, at(7));
}

TEST(JobFsmTest, RunningRecordsOnlyTheFirstStartOfAnInterval)
{
    TaskMeta meta = make_waiting_task();
    job_fsm_on_runnable(&meta, at(5));

    job_fsm_on_running(&meta, at(6));
    job_fsm_on_running(&meta, at(8));

    EXPECT_EQ(meta.t_start_ns, at(6));
}

TEST(JobFsmTest, RunningOutsideAnIntervalRecordsNothing)
{
    TaskMeta meta = make_waiting_task();

    job_fsm_on_running(&meta, at(6));

    EXPECT_EQ(meta.t_start_ns, 0u);
}

// ---- No inter-arrival check in BPF (FR-B-024, DDR-008 D-05) ---------------

// Activations 1 ms apart are far below any MIT. Only the window rule applies:
// the second interval shares the open window and is neither flagged as a
// violation nor throttled while budget remains.
TEST(JobFsmTest, NoMitCheckOnShortSpacing)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    job_fsm_on_quiescent(&meta);

    job_fsm_start_interval(&meta, &server, at(1));
    const int verdict = cbs_on_enqueue(&server, &meta, at(1));

    EXPECT_EQ(meta.job_state, JOB_ACTIVE);
    EXPECT_EQ(meta.job_flags, JOB_FLAG_CARRYOVER);
    EXPECT_EQ(verdict, CBS_ENQ_READY);
    EXPECT_EQ(server.throttle_episodes, 0u);
}

// ---- Charging (FR-B-012) --------------------------------------------------

TEST(CbsChargeTest, NanosecondCarry)
{
    CbsState server = make_server();
    server.remaining_us = BUDGET_US;

    const __u32 first_us = run_and_charge(&server, at(1), 1500);
    const __u32 second_us = run_and_charge(&server, at(2), 600);

    EXPECT_EQ(first_us, 1u);
    EXPECT_EQ(second_us, 1u);  // 500 ns carried + 600 ns = 1 us, 100 ns left
    EXPECT_EQ(server.exec_frac_ns, 100u);
    EXPECT_EQ(server.remaining_us, BUDGET_US - 2);
    EXPECT_EQ(server.exec_start_ns, 0u);
}

TEST(CbsChargeTest, RemainingSaturatesAtZero)
{
    CbsState server = make_server();
    server.remaining_us = 100;

    const __u32 charged_us = run_and_charge(&server, at(1), 250 * NS_PER_US);

    EXPECT_EQ(charged_us, 250u);
    EXPECT_EQ(server.remaining_us, 0u);
}

TEST(CbsChargeTest, VeryLongRunSaturatesInsteadOfWrapping)
{
    constexpr __u64 LONG_RUN_NS = (0x100000000ULL + 5) * NS_PER_US;
    CbsState server = make_server();
    server.remaining_us = BUDGET_US;

    const __u32 charged_us = run_and_charge(&server, at(1), LONG_RUN_NS);

    EXPECT_EQ(charged_us, 0xffffffffu);
    EXPECT_EQ(server.remaining_us, 0u);
}

TEST(CbsChargeTest, NoChargeWhenNotOnCpu)
{
    CbsState server = make_server();
    server.remaining_us = BUDGET_US;
    server.exec_start_ns = 0;

    const __u32 charged_us = cbs_charge(&server, at(9));

    EXPECT_EQ(charged_us, 0u);
    EXPECT_EQ(server.remaining_us, BUDGET_US);
}

TEST(CbsChargeTest, ExhaustionAloneDoesNotStartThrottleEpisode)
{
    CbsState server = make_server();
    server.remaining_us = BUDGET_US;

    run_and_charge(&server, at(1), BUDGET_US * NS_PER_US);

    EXPECT_EQ(server.remaining_us, 0u);
    EXPECT_EQ(server.throttled, 0u);
    EXPECT_EQ(server.throttle_episodes, 0u);
}

// ---- Replenishment window (FR-B-014, FR-B-017, DDR-008 D-04) --------------

TEST(CbsWindowTest, FirstActivationOpensWindow)
{
    CbsState server = make_server();

    const int refilled = cbs_on_activation(&server, at(16));

    EXPECT_EQ(refilled, 1);
    EXPECT_EQ(server.remaining_us, BUDGET_US);
    EXPECT_EQ(server.replenish_at_ns, at(16) + PERIOD_NS);
}

// D-04 counter-example against grid-aligned refill: activations at 16 ms and
// 34 ms keep the declared spacing, so both get a full budget.
TEST(CbsWindowTest, ConformingActivationAlwaysRefills)
{
    CbsState server = make_server();
    cbs_on_activation(&server, at(16));
    run_and_charge(&server, at(18), BUDGET_US * NS_PER_US);

    TaskMeta meta = make_waiting_task();

    const int refilled = cbs_on_activation(&server, at(34));
    const int verdict = cbs_on_enqueue(&server, &meta, at(34));

    EXPECT_EQ(refilled, 1);
    EXPECT_EQ(server.remaining_us, BUDGET_US);
    EXPECT_EQ(server.replenish_at_ns, at(34) + PERIOD_NS);
    EXPECT_EQ(verdict, CBS_ENQ_READY);
    EXPECT_EQ(server.throttle_episodes, 0u);
}

TEST(CbsWindowTest, EarlyActivationKeepsRemainingAndMarksCarryover)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), 500 * NS_PER_US);
    job_fsm_on_quiescent(&meta);

    const int started = job_fsm_start_interval(&meta, &server, at(15));

    EXPECT_EQ(started, 1);
    EXPECT_EQ(server.remaining_us, BUDGET_US - 500);
    EXPECT_EQ(server.replenish_at_ns, at(0) + PERIOD_NS);
    EXPECT_EQ(meta.job_flags, JOB_FLAG_CARRYOVER);
}

TEST(CbsWindowTest, RefilledActivationIsNotMarkedCarryover)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();

    job_fsm_start_interval(&meta, &server, at(0));

    EXPECT_EQ(meta.job_flags, 0u);
    EXPECT_EQ(server.remaining_us, BUDGET_US);
}

TEST(CbsWindowTest, ToleranceAbsorbsWakeJitter)
{
    CbsState server = make_server();
    cbs_on_activation(&server, at(0));
    server.remaining_us = 0;

    const int refilled = cbs_on_activation(&server, at(0) + PERIOD_NS - TOL_NS);

    EXPECT_EQ(refilled, 1);
    EXPECT_EQ(server.remaining_us, BUDGET_US);
    EXPECT_EQ(server.replenish_at_ns, at(0) + 2 * PERIOD_NS - TOL_NS);
}

TEST(CbsWindowTest, ActivationJustOutsideToleranceDoesNotRefill)
{
    CbsState server = make_server();
    cbs_on_activation(&server, at(0));
    server.remaining_us = 0;
    const __u64 just_outside_ns = at(0) + PERIOD_NS - TOL_NS - 1;

    const int refilled = cbs_on_activation(&server, just_outside_ns);

    EXPECT_EQ(refilled, 0);
    EXPECT_EQ(server.remaining_us, 0u);
    EXPECT_EQ(server.replenish_at_ns, at(0) + PERIOD_NS);
}

TEST(CbsWindowTest, RefillClearsNanosecondCarry)
{
    CbsState server = make_server();
    cbs_on_activation(&server, at(0));
    run_and_charge(&server, at(1), 1500);
    const __u32 carry_before_ns = server.exec_frac_ns;

    cbs_on_activation(&server, at(18));

    EXPECT_EQ(carry_before_ns, 500u);
    EXPECT_EQ(server.exec_frac_ns, 0u);
}

// D-04 "behaviour after a violation": arrivals at 0 / 15 / 33 ms. The early
// arrival shifts the window to [18, 36). With slack in the budget the server
// is back in arrival phase at the next activation after the window ends.
TEST(CbsWindowTest, AfterViolationWindowShiftsThenRecoversWithSlack)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();

    // J1 at 0 ms uses the whole budget, then sleeps.
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), BUDGET_US * NS_PER_US);
    job_fsm_on_quiescent(&meta);

    // J2 at 15 ms: window [0, 18) still open, no budget -> throttled.
    job_fsm_start_interval(&meta, &server, at(15));
    const int j2_verdict = cbs_on_enqueue(&server, &meta, at(15));
    const int promoted_early = cbs_try_promote(&server, at(17));
    const int promoted = cbs_try_promote(&server, at(18));
    const __u64 shifted_window_end_ns = server.replenish_at_ns;
    run_and_charge(&server, at(18), 1500 * NS_PER_US);  // 0.5 ms slack
    job_fsm_on_quiescent(&meta);

    // J3 at 33 ms: inside [18, 36), carries the remaining 0.5 ms over.
    job_fsm_start_interval(&meta, &server, at(33));
    const int j3_verdict = cbs_on_enqueue(&server, &meta, at(33));
    const __u32 j3_flags = meta.job_flags;
    run_and_charge(&server, at(33), 400 * NS_PER_US);
    job_fsm_on_quiescent(&meta);

    // J4 at 51 ms: the shifted window ended at 36 ms -> full budget again.
    job_fsm_start_interval(&meta, &server, at(51));

    EXPECT_EQ(j2_verdict, CBS_ENQ_THROTTLE_ENTER);
    EXPECT_EQ(promoted_early, 0);
    EXPECT_EQ(promoted, 1);
    EXPECT_EQ(shifted_window_end_ns, at(18) + PERIOD_NS);
    EXPECT_EQ(j3_verdict, CBS_ENQ_READY);
    EXPECT_EQ(j3_flags, JOB_FLAG_CARRYOVER);
    EXPECT_EQ(meta.job_flags, 0u);
    EXPECT_EQ(server.remaining_us, BUDGET_US);
    EXPECT_EQ(server.replenish_at_ns, at(51) + PERIOD_NS);
    EXPECT_EQ(server.throttle_episodes, 1u);
}

// Rule 4: a requeue inside an interval never refills, even after the window
// end has passed.
TEST(CbsWindowTest, NoRefillWhileActiveAndNotThrottled)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), 1500 * NS_PER_US);

    const int started = job_fsm_start_interval(&meta, &server, at(40));

    EXPECT_EQ(started, 0);
    EXPECT_EQ(server.remaining_us, BUDGET_US - 1500);
    EXPECT_EQ(server.replenish_at_ns, at(0) + PERIOD_NS);
}

TEST(CbsWindowTest, PromotionRefillsAndOpensWindowFromNow)
{
    CbsState server = make_server();
    cbs_on_activation(&server, at(0));
    server.remaining_us = 0;
    server.throttled = 1;
    server.exec_frac_ns = 700;

    const int before_end = cbs_try_promote(&server, at(0) + PERIOD_NS - 1);
    const __u32 remaining_before_end = server.remaining_us;
    const int at_end = cbs_try_promote(&server, at_us(18300));

    EXPECT_EQ(before_end, 0);  // promotion has no tolerance
    EXPECT_EQ(remaining_before_end, 0u);
    EXPECT_EQ(at_end, 1);
    EXPECT_EQ(server.remaining_us, BUDGET_US);
    EXPECT_EQ(server.replenish_at_ns, at_us(18300) + PERIOD_NS);
    EXPECT_EQ(server.throttled, 0u);
    EXPECT_EQ(server.exec_frac_ns, 0u);
}

TEST(CbsWindowTest, PromotionOfServerWithBudgetKeepsBudgetAndWindow)
{
    CbsState server = make_server();
    cbs_on_activation(&server, at(0));
    server.remaining_us = 300;
    server.throttled = 1;

    const int promoted = cbs_try_promote(&server, at(5));

    EXPECT_EQ(promoted, 1);
    EXPECT_EQ(server.remaining_us, 300u);
    EXPECT_EQ(server.replenish_at_ns, at(0) + PERIOD_NS);
    EXPECT_EQ(server.throttled, 0u);
}

// (P-b): a task that never sleeps gets at most C_s of CPU time per window.
// The run length is capped by the remaining budget, as the running op does
// with the slice, so the CPU time below is what the server really obtained.
TEST(CbsWindowTest, ConsumptionPerWindowBounded)
{
    constexpr int WINDOWS = 3;
    constexpr __u64 STEP_NS = 500 * NS_PER_US;
    constexpr __u64 WANTED_RUN_NS = 700 * NS_PER_US;
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    __u64 cpu_ns[WINDOWS] = {};
    __u64 window_start_ns[WINDOWS] = {at(0)};
    int window = 0;

    for (__u64 t = at(0); t < at(0) + WINDOWS * PERIOD_NS; t += STEP_NS) {
        const __u64 window_end_before_ns = server.replenish_at_ns;
        if (cbs_on_enqueue(&server, &meta, t) != CBS_ENQ_READY)
            continue;  // throttled until the window ends
        if (server.replenish_at_ns != window_end_before_ns) {
            window++;
            ASSERT_LT(window, WINDOWS);
            window_start_ns[window] = t;
        }
        const __u64 budget_ns = server.remaining_us * NS_PER_US;
        const __u64 run_ns = WANTED_RUN_NS < budget_ns ? WANTED_RUN_NS
                                                         : budget_ns;
        run_and_charge(&server, t, run_ns);
        cpu_ns[window] += run_ns;
    }

    EXPECT_EQ(window, WINDOWS - 1);
    for (int i = 0; i < WINDOWS; i++) {
        EXPECT_EQ(cpu_ns[i], BUDGET_US * NS_PER_US) << "window " << i;
        EXPECT_EQ(window_start_ns[i], at(0) + i * PERIOD_NS) << "window " << i;
    }
    EXPECT_EQ(server.throttle_episodes, static_cast<__u32>(WINDOWS));
}

// D-04 "behaviour after a violation", as written: every Job uses the whole
// budget. J2 (15 ms) and J3 (33 ms) both wait for a window end.
TEST(CbsWindowTest, AfterViolationWithoutSlackEachJobWaitsForWindowEnd)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), BUDGET_US * NS_PER_US);
    job_fsm_on_quiescent(&meta);
    job_fsm_start_interval(&meta, &server, at(15));
    cbs_on_enqueue(&server, &meta, at(15));
    cbs_try_promote(&server, at(18));
    run_and_charge(&server, at(18), BUDGET_US * NS_PER_US);
    job_fsm_on_quiescent(&meta);

    job_fsm_start_interval(&meta, &server, at(33));
    const int j3_verdict = cbs_on_enqueue(&server, &meta, at(33));
    const __u32 j3_flags = meta.job_flags;
    const int promoted_early = cbs_try_promote(&server, at(36) - 1);
    const int promoted = cbs_try_promote(&server, at(36));

    EXPECT_EQ(j3_verdict, CBS_ENQ_THROTTLE_ENTER);
    EXPECT_EQ(j3_flags, JOB_FLAG_CARRYOVER | JOB_FLAG_THROTTLED);
    EXPECT_EQ(promoted_early, 0);
    EXPECT_EQ(promoted, 1);
    EXPECT_EQ(server.replenish_at_ns, at(36) + PERIOD_NS);
    EXPECT_EQ(server.remaining_us, BUDGET_US);
    EXPECT_EQ(server.throttle_episodes, 2u);
}

// ---- Throttle entry (FR-B-013, FR-B-014, AC-008-03) -----------------------

TEST(CbsThrottleTest, ExactBudgetThenSleepDoesNotThrottle)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));

    run_and_charge(&server, at(0), BUDGET_US * NS_PER_US);
    job_fsm_on_quiescent(&meta);  // sleeps: no enqueue follows

    EXPECT_EQ(server.remaining_us, 0u);
    EXPECT_EQ(server.throttled, 0u);
    EXPECT_EQ(server.throttle_episodes, 0u);
    EXPECT_EQ(meta.job_flags & JOB_FLAG_THROTTLED, 0u);
}

TEST(CbsThrottleTest, ExactBudgetStillRunnableThrottles)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), BUDGET_US * NS_PER_US);

    const int verdict = cbs_on_enqueue(&server, &meta, at(2));

    EXPECT_EQ(verdict, CBS_ENQ_THROTTLE_ENTER);
    EXPECT_EQ(server.throttled, 1u);
    EXPECT_EQ(server.throttle_episodes, 1u);
    EXPECT_EQ(meta.job_flags & JOB_FLAG_THROTTLED, JOB_FLAG_THROTTLED);
}

TEST(CbsThrottleTest, ThrottleTransitionOnce)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), BUDGET_US * NS_PER_US);
    cbs_on_enqueue(&server, &meta, at(2));

    const int verdict = cbs_on_enqueue(&server, &meta, at(3));  // same episode

    EXPECT_EQ(verdict, CBS_ENQ_THROTTLE_STAY);
    EXPECT_EQ(server.throttle_episodes, 1u);
}

TEST(CbsThrottleTest, EnqueueWithBudgetClearsThrottledFlag)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    cbs_on_activation(&server, at(0));
    server.remaining_us = 300;
    server.throttled = 1;  // stale: budget is available

    const int ready = cbs_on_enqueue(&server, &meta, at(5));
    const __u32 throttled_when_ready = server.throttled;
    server.remaining_us = 0;
    const int exhausted = cbs_on_enqueue(&server, &meta, at(6));

    EXPECT_EQ(ready, CBS_ENQ_READY);
    EXPECT_EQ(throttled_when_ready, 0u);
    EXPECT_EQ(exhausted, CBS_ENQ_THROTTLE_ENTER);
    EXPECT_EQ(server.throttle_episodes, 1u);
}

// A task that ran past its window end (for example alone on its CPU) and is
// then requeued has nothing to wait for: rule 3 applies at enqueue.
TEST(CbsThrottleTest, EnqueueAfterWindowEndPromotesInPlace)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), BUDGET_US * NS_PER_US);

    const int verdict = cbs_on_enqueue(&server, &meta, at(19));

    EXPECT_EQ(verdict, CBS_ENQ_READY);
    EXPECT_EQ(server.remaining_us, BUDGET_US);
    EXPECT_EQ(server.replenish_at_ns, at(19) + PERIOD_NS);
    EXPECT_EQ(server.throttle_episodes, 0u);
    EXPECT_EQ(meta.job_flags & JOB_FLAG_THROTTLED, 0u);
    EXPECT_EQ(meta.queue_key_ns, at(19));  // behind the waiting tasks
}

TEST(CbsThrottleTest, EnqueueAtWindowEndPromotesOneNanosecondEarlierThrottles)
{
    TaskMeta early_meta = make_waiting_task();
    CbsState early = make_server();
    job_fsm_start_interval(&early_meta, &early, at(0));
    run_and_charge(&early, at(0), BUDGET_US * NS_PER_US);
    TaskMeta on_time_meta = make_waiting_task();
    CbsState on_time = make_server();
    job_fsm_start_interval(&on_time_meta, &on_time, at(0));
    run_and_charge(&on_time, at(0), BUDGET_US * NS_PER_US);

    const int before_end = cbs_on_enqueue(&early, &early_meta, at(18) - 1);
    const int at_end = cbs_on_enqueue(&on_time, &on_time_meta, at(18));

    EXPECT_EQ(before_end, CBS_ENQ_THROTTLE_ENTER);
    EXPECT_EQ(early.replenish_at_ns, at(0) + PERIOD_NS);
    EXPECT_EQ(at_end, CBS_ENQ_READY);
    EXPECT_EQ(on_time.replenish_at_ns, at(18) + PERIOD_NS);
}

TEST(CbsThrottleTest, EnqueueWithBudgetAfterWindowEndDoesNotRefill)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), 500 * NS_PER_US);

    const int verdict = cbs_on_enqueue(&server, &meta, at(30));  // rule 4

    EXPECT_EQ(verdict, CBS_ENQ_READY);
    EXPECT_EQ(server.remaining_us, BUDGET_US - 500);
    EXPECT_EQ(server.replenish_at_ns, at(0) + PERIOD_NS);
}

TEST(CbsThrottleTest, ZeroBudgetServerIsNeverReady)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    server.budget_us = 0;

    const int verdict = cbs_on_enqueue(&server, &meta, at(5));

    EXPECT_NE(verdict, CBS_ENQ_READY);
    EXPECT_EQ(server.remaining_us, 0u);
}

TEST(CbsThrottleTest, NewEpisodeAfterPromotionIsCountedAgain)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), BUDGET_US * NS_PER_US);
    cbs_on_enqueue(&server, &meta, at(2));
    cbs_try_promote(&server, at(18));
    run_and_charge(&server, at(18), BUDGET_US * NS_PER_US);

    const int verdict = cbs_on_enqueue(&server, &meta, at(20));

    EXPECT_EQ(verdict, CBS_ENQ_THROTTLE_ENTER);
    EXPECT_EQ(server.throttle_episodes, 2u);
}

TEST(CbsThrottleTest, WakeWithoutEligibleRefillThrottles)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), BUDGET_US * NS_PER_US);
    job_fsm_on_quiescent(&meta);

    job_fsm_start_interval(&meta, &server, at(10));  // window [0, 18) open
    const int verdict = cbs_on_enqueue(&server, &meta, at(10));

    EXPECT_EQ(verdict, CBS_ENQ_THROTTLE_ENTER);
    EXPECT_EQ(server.replenish_at_ns, at(0) + PERIOD_NS);  // window kept
    EXPECT_EQ(meta.job_flags, JOB_FLAG_CARRYOVER | JOB_FLAG_THROTTLED);
}

TEST(CbsThrottleTest, WakeWithEligibleRefillDoesNotThrottle)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), BUDGET_US * NS_PER_US);
    job_fsm_on_quiescent(&meta);

    job_fsm_start_interval(&meta, &server, at(18));
    const int verdict = cbs_on_enqueue(&server, &meta, at(18));

    EXPECT_EQ(verdict, CBS_ENQ_READY);
    EXPECT_EQ(server.throttled, 0u);
    EXPECT_EQ(server.throttle_episodes, 0u);
    EXPECT_EQ(meta.job_flags, 0u);
}

TEST(CbsThrottleTest, WakeWithRemainingBudgetInsideWindowDoesNotThrottle)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(0));
    run_and_charge(&server, at(0), 500 * NS_PER_US);
    job_fsm_on_quiescent(&meta);

    job_fsm_start_interval(&meta, &server, at(10));
    const int verdict = cbs_on_enqueue(&server, &meta, at(10));

    EXPECT_EQ(verdict, CBS_ENQ_READY);
    EXPECT_EQ(meta.job_flags, JOB_FLAG_CARRYOVER);
}

// ---- Ready-queue rank (FR-B-016, DDR-008 D-08) ----------------------------

TEST(QueueOrderTest, RequeueKeepsActivationRank)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(3));

    job_fsm_start_interval(&meta, &server, at(9));  // preempted, requeued

    EXPECT_EQ(meta.queue_key_ns, at(3));
}

TEST(QueueOrderTest, PromotedJobGoesToTail)
{
    TaskMeta meta = make_waiting_task();
    CbsState server = make_server();
    job_fsm_start_interval(&meta, &server, at(3));

    TaskMeta waiting = make_waiting_task();
    job_fsm_start_interval(&waiting, nullptr, at(10));

    job_fsm_on_promote(&meta, at(18));

    EXPECT_EQ(meta.queue_key_ns, at(18));
    EXPECT_GT(meta.queue_key_ns, waiting.queue_key_ns);  // ranks behind it
    EXPECT_EQ(meta.activation_ns, at(3));  // t_act of the interval is kept
}

TEST(QueueOrderTest, StartIntervalWithoutServerStillStartsInterval)
{
    TaskMeta meta = make_waiting_task();

    const int started = job_fsm_start_interval(&meta, nullptr, at(3));

    EXPECT_EQ(started, 1);
    EXPECT_EQ(meta.job_flags, 0u);
}

// ---- Transitional L2 deadline check (removed with DDR-008 D-20) -----------

TEST(LegacyDeadlineTest, MissIsReportedOncePerInterval)
{
    constexpr __u32 DEADLINE_US = 5000;
    TaskMeta meta = make_waiting_task();
    job_fsm_on_runnable(&meta, at(0));

    const int first = job_fsm_legacy_deadline_missed(&meta, DEADLINE_US, at(6));
    const int again = job_fsm_legacy_deadline_missed(&meta, DEADLINE_US, at(7));

    EXPECT_EQ(first, 1);
    EXPECT_EQ(again, 0);
    EXPECT_EQ(meta.job_flags, JOB_FLAG_DMISS);
    EXPECT_EQ(meta.activation_ns, at(0));  // t_act is not cleared any more
}

TEST(LegacyDeadlineTest, StopAtExactDeadlineIsNotAMiss)
{
    constexpr __u32 DEADLINE_US = 5000;
    TaskMeta meta = make_waiting_task();
    job_fsm_on_runnable(&meta, at(0));

    const int at_deadline =
        job_fsm_legacy_deadline_missed(&meta, DEADLINE_US, at(5));
    const int just_after =
        job_fsm_legacy_deadline_missed(&meta, DEADLINE_US, at(5) + 1);

    EXPECT_EQ(at_deadline, 0);
    EXPECT_EQ(just_after, 1);
}

TEST(LegacyDeadlineTest, NextIntervalIsJudgedAgain)
{
    constexpr __u32 DEADLINE_US = 5000;
    TaskMeta meta = make_waiting_task();
    job_fsm_on_runnable(&meta, at(0));
    job_fsm_legacy_deadline_missed(&meta, DEADLINE_US, at(6));
    job_fsm_on_quiescent(&meta);
    job_fsm_on_runnable(&meta, at(20));

    const int in_time =
        job_fsm_legacy_deadline_missed(&meta, DEADLINE_US, at(24));
    const int late = job_fsm_legacy_deadline_missed(&meta, DEADLINE_US, at(26));

    EXPECT_EQ(in_time, 0);
    EXPECT_EQ(late, 1);
}

TEST(LegacyDeadlineTest, NoVerdictOutsideAnIntervalOrWithoutDeadline)
{
    TaskMeta waiting = make_waiting_task();
    TaskMeta active = make_waiting_task();
    job_fsm_on_runnable(&active, at(0));

    const int outside = job_fsm_legacy_deadline_missed(&waiting, 5000, at(99));
    const int no_deadline = job_fsm_legacy_deadline_missed(&active, 0, at(99));

    EXPECT_EQ(outside, 0);
    EXPECT_EQ(no_deadline, 0);
    EXPECT_EQ(active.job_flags, 0u);
}
