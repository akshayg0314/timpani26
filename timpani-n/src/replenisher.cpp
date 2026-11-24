// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

#include "replenisher.h"

#include <sched.h>
#include <time.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <system_error>
#include <utility>

namespace timpani {
namespace node {

namespace {

constexpr uint64_t NS_PER_SEC = 1000000000ULL;

uint64_t mono_now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * NS_PER_SEC +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Orders the heap so that front() is the server with the earliest check.
struct LaterCheckFirst {
    bool operator()(const ReplenishServer& a, const ReplenishServer& b) const
    {
        return a.next_ns > b.next_ns;
    }
};

struct timespec to_timespec(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(ns / NS_PER_SEC);
    ts.tv_nsec = static_cast<long>(ns % NS_PER_SEC);
    return ts;
}

// Waits on `cv` until the CLOCK_MONOTONIC time `deadline_ns`, a signal or a
// spurious wakeup. The caller re-evaluates its condition in every case.
void wait_until_mono(pthread_cond_t* cv, pthread_mutex_t* mu,
                     uint64_t deadline_ns)
{
#if defined(__APPLE__)
    // Darwin has no pthread_condattr_setclock(); only the host unit tests
    // run there. A relative wait is equivalent for this caller.
    const uint64_t now_ns = mono_now_ns();
    const struct timespec relative =
        to_timespec(deadline_ns > now_ns ? deadline_ns - now_ns : 0);
    pthread_cond_timedwait_relative_np(cv, mu, &relative);
#else
    const struct timespec absolute = to_timespec(deadline_ns);
    pthread_cond_timedwait(cv, mu, &absolute);
#endif
}

// pthread calls return the error number. A failure here leaves the loop
// working but without the property the call was meant to give, so it is
// reported instead of ignored.
void report_if_failed(int rc, const char* what)
{
    if (rc != 0) {
        std::cerr << "[Replenisher] " << what << " failed: "
                  << std::strerror(rc) << std::endl;
    }
}

void init_monotonic_cond(pthread_cond_t* cv)
{
    pthread_condattr_t attr;
    report_if_failed(pthread_condattr_init(&attr), "condattr init");
#if !defined(__APPLE__)
    report_if_failed(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC),
                     "monotonic clock for the condition variable");
#endif
    report_if_failed(pthread_cond_init(cv, &attr), "cond init");
    pthread_condattr_destroy(&attr);
}

void init_priority_inherit_mutex(pthread_mutex_t* mu)
{
    pthread_mutexattr_t attr;
    report_if_failed(pthread_mutexattr_init(&attr), "mutexattr init");
    report_if_failed(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT),
                     "priority inheritance for the mutex");
    report_if_failed(pthread_mutex_init(mu, &attr), "mutex init");
    pthread_mutexattr_destroy(&attr);
}

uint64_t saturating_add(uint64_t a, uint64_t b)
{
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    return b > max - a ? max : a + b;
}

}  // namespace

uint64_t next_replenish_check_ns(const ReplenishSnapshot& snapshot,
                                 uint64_t now_ns, uint64_t period_ns,
                                 uint64_t retry_ns)
{
    const uint64_t soon_ns = saturating_add(now_ns, retry_ns);

    if (!snapshot.valid)
        return soon_ns;
    if (snapshot.throttled && snapshot.replenish_at_ns <= now_ns)
        return soon_ns;
    if (snapshot.replenish_at_ns > now_ns)
        return snapshot.replenish_at_ns;
    return std::max(saturating_add(now_ns, period_ns), soon_ns);
}

Replenisher::Replenisher(ReplenishKicker& kicker, int fifo_priority)
    : kicker_(kicker), fifo_priority_(fifo_priority)
{
    init_priority_inherit_mutex(&mu_);
    init_monotonic_cond(&cv_);
}

Replenisher::~Replenisher()
{
    stop();
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
}

void Replenisher::set_servers(std::vector<ReplenishServer> servers)
{
    // Allocation and ordering happen here, outside the lock.
    const uint64_t now_ns = mono_now_ns();
    for (ReplenishServer& server : servers)
        server.next_ns = now_ns;
    std::make_heap(servers.begin(), servers.end(), LaterCheckFirst{});

    pthread_mutex_lock(&mu_);
    heap_.swap(servers);
    ++epoch_;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
    // The previous storage is freed here, on the caller's thread.
}

void Replenisher::remove(uint64_t cbs_key)
{
    pthread_mutex_lock(&mu_);
    heap_.erase(std::remove_if(heap_.begin(), heap_.end(),
                               [cbs_key](const ReplenishServer& server) {
                                   return server.cbs_key == cbs_key;
                               }),
                heap_.end());
    std::make_heap(heap_.begin(), heap_.end(), LaterCheckFirst{});
    // If the loop is kicking this very server, it must not put it back.
    if (inflight_valid_ && inflight_key_ == cbs_key)
        inflight_removed_ = true;
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
}

bool Replenisher::start()
{
    std::lock_guard<std::mutex> control(control_mu_);
    if (thread_.joinable())
        return false;

    pthread_mutex_lock(&mu_);
    running_ = true;
    pthread_mutex_unlock(&mu_);

    try {
        thread_ = std::thread(&Replenisher::loop, this);
    } catch (const std::system_error& error) {
        pthread_mutex_lock(&mu_);
        running_ = false;
        pthread_mutex_unlock(&mu_);
        std::cerr << "[Replenisher] loop thread not started: " << error.what()
                  << std::endl;
        return false;
    }
    return true;
}

void Replenisher::stop()
{
    std::lock_guard<std::mutex> control(control_mu_);

    pthread_mutex_lock(&mu_);
    running_ = false;
    pthread_cond_broadcast(&cv_);
    pthread_mutex_unlock(&mu_);

    if (thread_.joinable())
        thread_.join();
}

void Replenisher::apply_rt_priority() const
{
    if (fifo_priority_ == NO_RT_PRIORITY)
        return;

    struct sched_param param = {};
    param.sched_priority = fifo_priority_;
    const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        std::cerr << "[Replenisher] SCHED_FIFO priority " << fifo_priority_
                  << " not applied (" << std::strerror(rc)
                  << "); running with the default policy, window-end kicks"
                     " may be late"
                  << std::endl;
    }
}

void Replenisher::loop()
{
    apply_rt_priority();

    pthread_mutex_lock(&mu_);
    while (running_) {
        if (heap_.empty()) {
            pthread_cond_wait(&cv_, &mu_);
            continue;
        }

        ReplenishServer server = heap_.front();
        if (mono_now_ns() < server.next_ns) {
            // Until the earliest check, a table change, a removal or stop().
            wait_until_mono(&cv_, &mu_, server.next_ns);
            continue;
        }

        std::pop_heap(heap_.begin(), heap_.end(), LaterCheckFirst{});
        heap_.pop_back();
        const uint64_t epoch = epoch_;
        inflight_key_ = server.cbs_key;
        inflight_valid_ = true;
        inflight_removed_ = false;
        pthread_mutex_unlock(&mu_);

        const ReplenishSnapshot snapshot = kicker_.kick(server);
        server.next_ns = next_replenish_check_ns(
            snapshot, mono_now_ns(), server.period_ns, REPLENISH_RETRY_NS);

        pthread_mutex_lock(&mu_);
        // Put the server back unless the whole set was replaced or the
        // server itself was removed while it was being kicked. The heap has
        // the capacity: one element was popped above.
        if (epoch == epoch_ && !inflight_removed_) {
            heap_.push_back(server);
            std::push_heap(heap_.begin(), heap_.end(), LaterCheckFirst{});
        }
        inflight_valid_ = false;
    }
    pthread_mutex_unlock(&mu_);
}

}  // namespace node
}  // namespace timpani
