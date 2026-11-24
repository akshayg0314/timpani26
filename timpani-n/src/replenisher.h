// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#pragma once

#include <pthread.h>

#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace timpani {
namespace node {

/// One CBS server the Replenisher watches.
struct ReplenishServer {
    uint64_t cbs_key;    ///< key of the server in the BPF cbs_map
    uint32_t cpu;        ///< CPU the server is assigned to
    uint64_t period_ns;  ///< T_s, length of a replenishment window
    uint64_t next_ns;    ///< next check time (CLOCK_MONOTONIC), set internally
};

/// State of a server as seen by a kick.
struct ReplenishSnapshot {
    bool valid;                ///< false: the kick could not be run
    bool throttled;            ///< the task waits for its window end
    uint32_t remaining_us;
    uint64_t replenish_at_ns;  ///< end of the open window; 0 = none
};

/**
 * @brief Backend that looks at one server and, if its window has ended while
 *        the task is throttled, makes the server's CPU reschedule.
 *
 * The backend only reads the server state. The budget is refilled by the BPF
 * scheduler on the assigned CPU, never from user space: a refill from another
 * CPU could overwrite a charge that is in progress.
 */
class ReplenishKicker {
public:
    virtual ~ReplenishKicker() = default;
    virtual ReplenishSnapshot kick(const ReplenishServer& server) = 0;
};

/**
 * @brief When to look at a server again after a kick.
 *
 * - kick failed, or the window has ended but the task is still throttled:
 *   soon (retry_ns), because the promotion may be pending;
 * - a window is open: at its end;
 * - no window is open: after one period. A window opened in between cannot
 *   end earlier than that.
 *
 * The result is always later than now_ns: a period shorter than retry_ns is
 * treated as retry_ns, and the additions saturate instead of wrapping, so
 * the loop cannot spin on one server.
 */
uint64_t next_replenish_check_ns(const ReplenishSnapshot& snapshot,
                                 uint64_t now_ns, uint64_t period_ns,
                                 uint64_t retry_ns);

/**
 * @brief Backup replenishment: wakes the CPU of a throttled server when its
 *        window ends.
 *
 * The scheduler promotes a throttled task from its dispatch path, which does
 * not run on an idle CPU. This thread follows each server's window end and
 * asks the backend to kick the CPU at that time. It holds no scheduling
 * state of its own and never writes the server state.
 */
class Replenisher {
public:
    /// Re-check interval for a server that is due but not promoted yet.
    static constexpr uint64_t REPLENISH_RETRY_NS = 1000000;

    /// Priority that leaves the thread in the default scheduling policy.
    static constexpr int NO_RT_PRIORITY = 0;

    /**
     * @param kicker        backend, must outlive this object
     * @param fifo_priority SCHED_FIFO priority of the loop thread, or
     *                      NO_RT_PRIORITY to keep the default policy
     */
    explicit Replenisher(ReplenishKicker& kicker, int fifo_priority = 98);
    ~Replenisher();

    Replenisher(const Replenisher&) = delete;
    Replenisher& operator=(const Replenisher&) = delete;

    /// Replaces the whole server set. Every server is checked once right
    /// away, then at the time its window state asks for.
    void set_servers(std::vector<ReplenishServer> servers);

    /// Stops watching one server. Other servers keep their schedule.
    void remove(uint64_t cbs_key);

    /// Starts the loop thread. Returns false if it is already running or
    /// the thread could not be created.
    bool start();

    /// Stops the loop thread and waits for it. Safe to call twice, and
    /// safe to call concurrently with start().
    void stop();

private:
    void loop();
    void apply_rt_priority() const;

    ReplenishKicker& kicker_;
    const int fifo_priority_;

    std::mutex control_mu_;  // serializes start() and stop(); guards thread_

    pthread_mutex_t mu_;  // priority inheritance: the loop may run SCHED_FIFO
    pthread_cond_t cv_;   // waits on CLOCK_MONOTONIC

    // All fields below are read and written under mu_ only.
    std::vector<ReplenishServer> heap_;  // min-heap by next_ns
    uint64_t epoch_ = 0;                 // bumped by set_servers() only
    uint64_t inflight_key_ = 0;          // server being kicked right now
    bool inflight_valid_ = false;
    bool inflight_removed_ = false;
    bool running_ = false;

    std::thread thread_;  // guarded by control_mu_
};

}  // namespace node
}  // namespace timpani
