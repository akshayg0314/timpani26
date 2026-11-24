// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

// Host tests for timpani-n/src/replenisher.{h,cpp} with a fake kick backend.
// The loop runs on the real monotonic clock, so the tests use short times and
// wait for a condition with a generous timeout instead of sleeping a fixed
// time and hoping.

#include <gtest/gtest.h>

#include <time.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "replenisher.h"

namespace
{

using timpani::node::ReplenishKicker;
using timpani::node::ReplenishServer;
using timpani::node::ReplenishSnapshot;
using timpani::node::Replenisher;
using timpani::node::next_replenish_check_ns;

constexpr uint64_t NS_PER_MS = 1000000ULL;
constexpr uint64_t RETRY_NS = Replenisher::REPLENISH_RETRY_NS;
constexpr uint64_t PERIOD_NS = 18 * NS_PER_MS;
constexpr uint64_t LONG_PERIOD_NS = 10000 * NS_PER_MS;  // never due in a test
constexpr auto WAIT_TIMEOUT = std::chrono::seconds(5);
constexpr uint64_t KEY_A = 0xA;
constexpr uint64_t KEY_B = 0xB;
constexpr uint64_t KEY_C = 0xC;

uint64_t mono_now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

ReplenishServer make_server(uint64_t key, uint64_t period_ns = LONG_PERIOD_NS)
{
    return ReplenishServer{key, /*cpu=*/2, period_ns, /*next_ns=*/0};
}

// Snapshot of a server whose window ends `delay_ns` from now.
ReplenishSnapshot window_ending_in(uint64_t delay_ns)
{
    return ReplenishSnapshot{true, false, 0, mono_now_ns() + delay_ns};
}

// Snapshot of a throttled server whose window has already ended.
ReplenishSnapshot due_but_not_promoted()
{
    return ReplenishSnapshot{true, true, 0, mono_now_ns() - NS_PER_MS};
}

struct KickRecord {
    uint64_t key;
    uint64_t at_ns;
};

// Records every kick and answers with a scripted snapshot. A test can make
// the kick of one key block, to hold that server "in flight".
class FakeKicker : public ReplenishKicker
{
public:
    using Script = std::function<ReplenishSnapshot(const ReplenishServer&)>;

    explicit FakeKicker(Script script) : script_(std::move(script)) {}

    ReplenishSnapshot kick(const ReplenishServer& server) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        records_.push_back(KickRecord{server.cbs_key, mono_now_ns()});
        changed_.notify_all();
        if (blocking_ && server.cbs_key == blocked_key_) {
            inside_blocked_kick_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return !blocking_; });
            inside_blocked_kick_ = false;
        }
        lock.unlock();  // the script may call back into this object
        return script_(server);
    }

    void block_kicks_of(uint64_t key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocked_key_ = key;
        blocking_ = true;
    }

    void release_blocked_kick()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocking_ = false;
        changed_.notify_all();
    }

    bool wait_until_inside_blocked_kick()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, WAIT_TIMEOUT,
                                 [this] { return inside_blocked_kick_; });
    }

    bool wait_for_kicks(size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, WAIT_TIMEOUT, [this, count] {
            return records_.size() >= count;
        });
    }

    bool wait_for_kicks_of(uint64_t key, size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, WAIT_TIMEOUT, [this, key, count] {
            return count_locked(key) >= count;
        });
    }

    std::vector<KickRecord> records()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    size_t count_of(uint64_t key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_locked(key);
    }

private:
    size_t count_locked(uint64_t key) const
    {
        size_t count = 0;
        for (const KickRecord& record : records_)
            count += record.key == key ? 1 : 0;
        return count;
    }

    Script script_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<KickRecord> records_;
    uint64_t blocked_key_ = 0;
    bool blocking_ = false;
    bool inside_blocked_kick_ = false;
};

void settle(uint64_t ns)
{
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

}  // namespace

// ---- Next check time (pure) -----------------------------------------------

TEST(NextCheckTest, FailedKickIsRetriedSoon)
{
    const ReplenishSnapshot failed{false, false, 0, 0};

    const uint64_t next =
        next_replenish_check_ns(failed, 1000, LONG_PERIOD_NS, RETRY_NS);

    EXPECT_EQ(next, 1000 + RETRY_NS);
}

TEST(NextCheckTest, DueButNotPromotedIsRetriedSoon)
{
    const ReplenishSnapshot due{true, true, 0, 900};

    const uint64_t at_end =
        next_replenish_check_ns({true, true, 0, 1000}, 1000, 500, RETRY_NS);
    const uint64_t after_end =
        next_replenish_check_ns(due, 1000, 500, RETRY_NS);

    EXPECT_EQ(at_end, 1000 + RETRY_NS);
    EXPECT_EQ(after_end, 1000 + RETRY_NS);
}

TEST(NextCheckTest, OpenWindowIsCheckedAtItsEnd)
{
    const ReplenishSnapshot running{true, false, 700, 5000};
    const ReplenishSnapshot throttled{true, true, 0, 5000};

    const uint64_t next_running =
        next_replenish_check_ns(running, 1000, 500, RETRY_NS);
    const uint64_t next_throttled =
        next_replenish_check_ns(throttled, 1000, 500, RETRY_NS);

    EXPECT_EQ(next_running, 5000u);
    EXPECT_EQ(next_throttled, 5000u);
}

TEST(NextCheckTest, NoOpenWindowIsCheckedAfterOnePeriod)
{
    const ReplenishSnapshot never_ran{true, false, 2000, 0};
    const ReplenishSnapshot ended{true, false, 300, 900};

    const uint64_t next_never_ran =
        next_replenish_check_ns(never_ran, 1000, PERIOD_NS, RETRY_NS);
    const uint64_t next_ended =
        next_replenish_check_ns(ended, 1000, PERIOD_NS, RETRY_NS);

    EXPECT_EQ(next_never_ran, 1000 + PERIOD_NS);
    EXPECT_EQ(next_ended, 1000 + PERIOD_NS);
}

TEST(NextCheckTest, WindowEndingNowWithoutThrottleCountsAsNoOpenWindow)
{
    const ReplenishSnapshot ending_now{true, false, 0, 1000};

    const uint64_t next =
        next_replenish_check_ns(ending_now, 1000, 500, RETRY_NS);

    EXPECT_EQ(next, 1000 + RETRY_NS);  // period 500 ns < retry: see below
}

// A period of 0 (or shorter than the retry interval) must not make the loop
// spin on one server.
TEST(NextCheckTest, ResultIsAlwaysLaterThanNow)
{
    const ReplenishSnapshot no_window{true, false, 2000, 0};

    const uint64_t zero_period =
        next_replenish_check_ns(no_window, 1000, 0, RETRY_NS);
    const uint64_t long_period =
        next_replenish_check_ns(no_window, 1000, 5 * RETRY_NS, RETRY_NS);

    EXPECT_EQ(zero_period, 1000 + RETRY_NS);
    EXPECT_EQ(long_period, 1000 + 5 * RETRY_NS);
}

TEST(NextCheckTest, HugeValuesSaturateInsteadOfWrapping)
{
    constexpr uint64_t MAX = std::numeric_limits<uint64_t>::max();
    const ReplenishSnapshot no_window{true, false, 2000, 0};
    const ReplenishSnapshot failed{false, false, 0, 0};

    const uint64_t huge_period =
        next_replenish_check_ns(no_window, 1000, MAX - 10, RETRY_NS);
    const uint64_t late_now =
        next_replenish_check_ns(failed, MAX - 10, 500, RETRY_NS);

    EXPECT_EQ(huge_period, MAX);
    EXPECT_EQ(late_now, MAX);
}

// ---- Loop -----------------------------------------------------------------

TEST(ReplenisherTest, EmptyHeapSleeps)
{
    FakeKicker kicker([](const ReplenishServer&) {
        return window_ending_in(LONG_PERIOD_NS);
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);

    const bool started = replenisher.start();
    settle(30 * NS_PER_MS);
    replenisher.stop();

    EXPECT_TRUE(started);
    EXPECT_EQ(kicker.records().size(), 0u);
}

TEST(ReplenisherTest, StartTwiceIsRefused)
{
    FakeKicker kicker([](const ReplenishServer&) {
        return window_ending_in(LONG_PERIOD_NS);
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();

    const bool started_again = replenisher.start();
    replenisher.stop();

    EXPECT_FALSE(started_again);
}

TEST(ReplenisherTest, StopWakesEmptyLoop)
{
    FakeKicker kicker([](const ReplenishServer&) {
        return window_ending_in(LONG_PERIOD_NS);
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    settle(10 * NS_PER_MS);

    const uint64_t before_ns = mono_now_ns();
    replenisher.stop();
    const uint64_t stop_ns = mono_now_ns() - before_ns;

    EXPECT_LT(stop_ns, 1000 * NS_PER_MS);
}

TEST(ReplenisherTest, StopWakesLoopWaitingForFarWindowEnd)
{
    FakeKicker kicker([](const ReplenishServer&) {
        return window_ending_in(LONG_PERIOD_NS);
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.set_servers({make_server(KEY_A)});
    ASSERT_TRUE(kicker.wait_for_kicks(1));

    const uint64_t before_ns = mono_now_ns();
    replenisher.stop();
    const uint64_t stop_ns = mono_now_ns() - before_ns;

    EXPECT_LT(stop_ns, 1000 * NS_PER_MS);
    EXPECT_EQ(kicker.records().size(), 1u);
}

TEST(ReplenisherTest, EveryServerIsCheckedOnceRightAway)
{
    FakeKicker kicker([](const ReplenishServer&) {
        return window_ending_in(LONG_PERIOD_NS);
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();

    replenisher.set_servers(
        {make_server(KEY_A), make_server(KEY_B), make_server(KEY_C)});
    const bool all_checked = kicker.wait_for_kicks(3);
    settle(20 * NS_PER_MS);
    replenisher.stop();

    EXPECT_TRUE(all_checked);
    EXPECT_EQ(kicker.count_of(KEY_A), 1u);
    EXPECT_EQ(kicker.count_of(KEY_B), 1u);
    EXPECT_EQ(kicker.count_of(KEY_C), 1u);
}

// Windows end at +300 ms (A), +100 ms (B) and +200 ms (C): the second round
// of checks comes in that order of window ends, not in insertion order.
TEST(ReplenisherTest, HeapOrder)
{
    std::mutex script_mutex;
    std::vector<uint64_t> seen;
    FakeKicker kicker([&](const ReplenishServer& server) {
        std::lock_guard<std::mutex> lock(script_mutex);
        const bool first = std::count(seen.begin(), seen.end(),
                                      server.cbs_key) == 0;
        seen.push_back(server.cbs_key);
        if (!first)
            return window_ending_in(LONG_PERIOD_NS);
        if (server.cbs_key == KEY_A)
            return window_ending_in(300 * NS_PER_MS);
        if (server.cbs_key == KEY_B)
            return window_ending_in(100 * NS_PER_MS);
        return window_ending_in(200 * NS_PER_MS);
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();

    replenisher.set_servers(
        {make_server(KEY_A), make_server(KEY_B), make_server(KEY_C)});
    const bool six_kicks = kicker.wait_for_kicks(6);
    replenisher.stop();

    ASSERT_TRUE(six_kicks);
    const std::vector<KickRecord> records = kicker.records();
    EXPECT_EQ(records[3].key, KEY_B);
    EXPECT_EQ(records[4].key, KEY_C);
    EXPECT_EQ(records[5].key, KEY_A);
}

TEST(ReplenisherTest, RearmsFromReturnedReplenishAt)
{
    constexpr uint64_t WINDOW_NS = 60 * NS_PER_MS;
    uint64_t window_end_ns = 0;
    FakeKicker kicker([&](const ReplenishServer&) {
        if (window_end_ns != 0)
            return window_ending_in(LONG_PERIOD_NS);
        const ReplenishSnapshot snapshot = window_ending_in(WINDOW_NS);
        window_end_ns = snapshot.replenish_at_ns;
        return snapshot;
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();

    replenisher.set_servers({make_server(KEY_A)});
    const bool second_check = kicker.wait_for_kicks(2);
    replenisher.stop();

    ASSERT_TRUE(second_check);
    const std::vector<KickRecord> records = kicker.records();
    EXPECT_GE(records[1].at_ns, window_end_ns);  // not before the window end
    EXPECT_LT(records[1].at_ns, window_end_ns + 1000 * NS_PER_MS);
}

TEST(ReplenisherTest, RetriesWhileDueButNotPromoted)
{
    constexpr size_t RETRIES = 10;
    FakeKicker kicker(
        [](const ReplenishServer&) { return due_but_not_promoted(); });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();

    replenisher.set_servers({make_server(KEY_A)});  // period: 10 s
    const bool retried = kicker.wait_for_kicks(RETRIES);
    replenisher.stop();

    ASSERT_TRUE(retried);  // far sooner than one period
    const std::vector<KickRecord> records = kicker.records();
    for (size_t i = 1; i < RETRIES; i++)
        EXPECT_GE(records[i].at_ns - records[i - 1].at_ns, RETRY_NS) << i;
    // 9 retries at 1 ms each; the bound leaves room for a loaded machine
    EXPECT_LT(records[RETRIES - 1].at_ns - records[0].at_ns, 500 * NS_PER_MS);
}

TEST(ReplenisherTest, SwapOnTableChangeWakesLoop)
{
    FakeKicker kicker([](const ReplenishServer&) {
        return window_ending_in(LONG_PERIOD_NS);
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.set_servers({make_server(KEY_A)});
    ASSERT_TRUE(kicker.wait_for_kicks_of(KEY_A, 1));  // now waits ~10 s

    replenisher.set_servers({make_server(KEY_B)});
    const bool new_server_checked = kicker.wait_for_kicks_of(KEY_B, 1);
    settle(20 * NS_PER_MS);
    replenisher.stop();

    EXPECT_TRUE(new_server_checked);
    EXPECT_EQ(kicker.count_of(KEY_A), 1u);  // the old table is gone
}

TEST(ReplenisherTest, SwapDropsServersWaitingInTheHeap)
{
    FakeKicker kicker([](const ReplenishServer& server) {
        return server.cbs_key == KEY_A ? window_ending_in(300 * NS_PER_MS)
                                       : due_but_not_promoted();
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.set_servers({make_server(KEY_A)});
    ASSERT_TRUE(kicker.wait_for_kicks_of(KEY_A, 1));

    replenisher.set_servers({make_server(KEY_B)});
    const bool b_keeps_going = kicker.wait_for_kicks_of(KEY_B, 400);
    replenisher.stop();

    EXPECT_TRUE(b_keeps_going);  // at 1 ms per retry, past A's 300 ms
    EXPECT_EQ(kicker.count_of(KEY_A), 1u);
}

// Six servers with window ends 100 ms apart. After the one with the earliest
// end is removed, the others must still be checked in window-end order.
TEST(ReplenisherTest, RemoveKeepsCheckOrderOfTheOthers)
{
    constexpr size_t SERVERS = 6;
    constexpr uint64_t SPACING_NS = 100 * NS_PER_MS;
    std::mutex script_mutex;
    std::vector<uint64_t> seen;
    FakeKicker kicker([&](const ReplenishServer& server) {
        std::lock_guard<std::mutex> lock(script_mutex);
        const bool first = std::count(seen.begin(), seen.end(),
                                      server.cbs_key) == 0;
        seen.push_back(server.cbs_key);
        // key k (1..6): window ends k * 100 ms after its first check
        return first ? window_ending_in(server.cbs_key * SPACING_NS)
                     : window_ending_in(LONG_PERIOD_NS);
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.set_servers({make_server(4), make_server(1), make_server(6),
                             make_server(3), make_server(5), make_server(2)});
    ASSERT_TRUE(kicker.wait_for_kicks(SERVERS));

    replenisher.remove(1);
    const bool second_round = kicker.wait_for_kicks(2 * SERVERS - 1);
    replenisher.stop();

    ASSERT_TRUE(second_round);
    const std::vector<KickRecord> records = kicker.records();
    for (size_t i = 0; i < SERVERS - 1; i++)
        EXPECT_EQ(records[SERVERS + i].key, i + 2) << "check " << i;
}

TEST(ReplenisherTest, ConcurrentStartAndStopLeaveAConsistentState)
{
    constexpr int ROUNDS = 200;
    FakeKicker kicker(
        [](const ReplenishServer&) { return due_but_not_promoted(); });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.set_servers({make_server(KEY_A)});

    std::thread starter([&] {
        for (int i = 0; i < ROUNDS; i++)
            replenisher.start();
    });
    std::thread stopper([&] {
        for (int i = 0; i < ROUNDS; i++)
            replenisher.stop();
    });
    starter.join();
    stopper.join();
    replenisher.stop();
    const size_t kicks_after_stop = kicker.records().size();
    settle(20 * NS_PER_MS);

    EXPECT_EQ(kicker.records().size(), kicks_after_stop);  // loop is gone
    EXPECT_TRUE(replenisher.start());                      // and restartable
    replenisher.stop();
}

TEST(ReplenisherTest, SwapDuringKickDropsTheInflightServer)
{
    FakeKicker kicker(
        [](const ReplenishServer&) { return due_but_not_promoted(); });
    kicker.block_kicks_of(KEY_A);
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.set_servers({make_server(KEY_A)});
    ASSERT_TRUE(kicker.wait_until_inside_blocked_kick());

    replenisher.set_servers({make_server(KEY_B)});
    kicker.release_blocked_kick();
    const bool new_server_checked = kicker.wait_for_kicks_of(KEY_B, 3);
    replenisher.stop();

    EXPECT_TRUE(new_server_checked);
    EXPECT_EQ(kicker.count_of(KEY_A), 1u);  // not pushed back into the new set
}

TEST(ReplenisherTest, RemoveDuringKickDoesNotPushBack)
{
    FakeKicker kicker(
        [](const ReplenishServer&) { return due_but_not_promoted(); });
    kicker.block_kicks_of(KEY_A);
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.set_servers({make_server(KEY_A), make_server(KEY_B)});
    ASSERT_TRUE(kicker.wait_until_inside_blocked_kick());

    replenisher.remove(KEY_A);
    kicker.release_blocked_kick();
    // B is retried every ms; a pushed-back A would be retried in between.
    const bool loop_went_on = kicker.wait_for_kicks_of(KEY_B, 10);
    replenisher.stop();

    EXPECT_TRUE(loop_went_on);
    EXPECT_EQ(kicker.count_of(KEY_A), 1u);
}

TEST(ReplenisherTest, RemoveOfOtherServerKeepsInflightServer)
{
    FakeKicker kicker([](const ReplenishServer& server) {
        return server.cbs_key == KEY_A ? due_but_not_promoted()
                                       : window_ending_in(LONG_PERIOD_NS);
    });
    kicker.block_kicks_of(KEY_A);
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.set_servers({make_server(KEY_A), make_server(KEY_B)});
    ASSERT_TRUE(kicker.wait_until_inside_blocked_kick());

    replenisher.remove(KEY_B);
    kicker.release_blocked_kick();
    const bool a_retried = kicker.wait_for_kicks_of(KEY_A, 3);
    replenisher.stop();

    EXPECT_TRUE(a_retried);  // A kept its schedule
    EXPECT_LE(kicker.count_of(KEY_B), 1u);
}

TEST(ReplenisherTest, RemovedServerIsNotCheckedAgain)
{
    FakeKicker kicker([](const ReplenishServer& server) {
        return server.cbs_key == KEY_A ? window_ending_in(300 * NS_PER_MS)
                                       : due_but_not_promoted();
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.set_servers({make_server(KEY_A), make_server(KEY_B)});
    ASSERT_TRUE(kicker.wait_for_kicks_of(KEY_A, 1));

    replenisher.remove(KEY_A);  // waiting in the heap for its window end
    const bool b_keeps_going = kicker.wait_for_kicks_of(KEY_B, 400);
    replenisher.stop();

    EXPECT_TRUE(b_keeps_going);  // at 1 ms per retry, past A's 300 ms
    EXPECT_EQ(kicker.count_of(KEY_A), 1u);
}

TEST(ReplenisherTest, StopWhileKickIsInFlightReturns)
{
    FakeKicker kicker(
        [](const ReplenishServer&) { return due_but_not_promoted(); });
    kicker.block_kicks_of(KEY_A);
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.set_servers({make_server(KEY_A)});
    ASSERT_TRUE(kicker.wait_until_inside_blocked_kick());

    std::thread stopper([&] { replenisher.stop(); });
    settle(10 * NS_PER_MS);
    kicker.release_blocked_kick();
    stopper.join();

    EXPECT_EQ(kicker.count_of(KEY_A), 1u);
}

TEST(ReplenisherTest, DestructorStopsRunningLoop)
{
    FakeKicker kicker(
        [](const ReplenishServer&) { return due_but_not_promoted(); });
    {
        Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
        replenisher.start();
        replenisher.set_servers({make_server(KEY_A)});
        ASSERT_TRUE(kicker.wait_for_kicks(2));
    }
    const size_t kicks_at_destruction = kicker.records().size();

    settle(20 * NS_PER_MS);

    EXPECT_EQ(kicker.records().size(), kicks_at_destruction);
}

TEST(ReplenisherTest, CanBeRestartedAfterStop)
{
    FakeKicker kicker([](const ReplenishServer&) {
        return window_ending_in(LONG_PERIOD_NS);
    });
    Replenisher replenisher(kicker, Replenisher::NO_RT_PRIORITY);
    replenisher.start();
    replenisher.stop();

    const bool restarted = replenisher.start();
    replenisher.set_servers({make_server(KEY_A)});
    const bool checked = kicker.wait_for_kicks(1);
    replenisher.stop();

    EXPECT_TRUE(restarted);
    EXPECT_TRUE(checked);
}
