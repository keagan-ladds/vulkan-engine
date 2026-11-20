#include "ve/core/Jobs.hpp"

#include "ve/core/Log.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace ve::jobs {

namespace detail {

// Queued -> Running -> Done, or Queued -> Done when cancelled before it ran.
// Whoever wins the Queued -> Running exchange (a worker, or wait() on the
// owner's thread) runs it; the loser skips it. So a job runs at most once and
// a cancelled one never starts.
enum State : int { Queued, Running, Done };

struct Job {
    Fn fn;
    std::atomic<bool> cancel{false};
    std::atomic<int> state{Queued};

    // Runs the job if nobody else has; returns false if it was taken already.
    bool tryRun()
    {
        int expected = Queued;
        if (!state.compare_exchange_strong(expected, Running)) return false;
        if (!cancel.load()) {
            try {
                fn(cancel);
            } catch (const std::exception& e) {
                log::error("Job threw: {}", e.what());
            } catch (...) {
                log::error("Job threw an unknown exception");
            }
        }
        fn = nullptr; // release what it captured now, not when the last handle goes
        finish();
        return true;
    }
    // Cancelled before it started: it never runs.
    bool tryDrop()
    {
        int expected = Queued;
        if (!state.compare_exchange_strong(expected, Done)) return false;
        fn = nullptr;
        state.notify_all();
        return true;
    }
    void finish()
    {
        state.store(Done);
        state.notify_all();
    }
};

} // namespace detail

namespace {

using detail::Job;

class Pool {
public:
    Pool()
    {
        const unsigned n = workerCount();
        m_workers.reserve(n);
        for (unsigned i = 0; i < n; ++i) m_workers.emplace_back([this] { workerLoop(); });
    }
    ~Pool() { shutdown(); }

    // False once shut down (the job is not queued).
    bool push(std::shared_ptr<Job> job, Lane lane)
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) return false;
            (lane == Lane::Normal ? m_normal : m_background).push_back(std::move(job));
        }
        m_wake.notify_one();
        return true;
    }

    bool hasQueued()
    {
        std::lock_guard lock(m_mutex);
        return !m_normal.empty() || !m_background.empty();
    }

    void shutdown()
    {
        std::deque<std::shared_ptr<Job>> dropped;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) return;
            m_stopping = true;
            dropped.swap(m_normal);
            dropped.insert(dropped.end(), m_background.begin(), m_background.end());
            m_background.clear();
        }
        m_wake.notify_all();
        for (auto& job : dropped) {
            job->cancel.store(true);
            job->tryDrop();
        }
        // Running jobs finish (they were asked to stop only if their owner cancelled them).
        for (std::thread& t : m_workers) t.join();
        m_workers.clear();
    }

private:
    void workerLoop()
    {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [&] { return m_stopping || !m_normal.empty() || !m_background.empty(); });
                if (m_stopping) return;
                auto& q = m_normal.empty() ? m_background : m_normal;
                job = std::move(q.front());
                q.pop_front();
            }
            job->tryRun(); // false: cancelled, or a wait() ran it already
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::shared_ptr<Job>> m_normal, m_background;
    bool m_stopping = false;
    std::vector<std::thread> m_workers;
};

std::mutex g_poolMutex;
std::unique_ptr<Pool> g_pool;
bool g_shutDown = false;

// Created on first use; null after shutdown().
Pool* pool()
{
    std::lock_guard lock(g_poolMutex);
    if (!g_pool && !g_shutDown) g_pool = std::make_unique<Pool>();
    return g_shutDown ? nullptr : g_pool.get();
}

} // namespace

// -----------------------------------------------------------------------------
// Handle
// -----------------------------------------------------------------------------

bool Handle::done() const
{
    return !m_job || m_job->state.load() == detail::Done;
}

bool Handle::cancelled() const
{
    return m_job && m_job->cancel.load();
}

void Handle::cancel()
{
    if (!m_job) return;
    m_job->cancel.store(true);
    m_job->tryDrop(); // still queued: it will never run (the worker that pops it skips it)
}

void Handle::wait()
{
    if (!m_job) return;
    if (m_job->tryRun()) return; // nobody started it: do it here
    for (int s = m_job->state.load(); s != detail::Done; s = m_job->state.load()) m_job->state.wait(s);
}

// -----------------------------------------------------------------------------
// Free functions
// -----------------------------------------------------------------------------

Handle submit(Fn fn, Lane lane)
{
    Handle h;
    h.m_job = std::make_shared<Job>();
    h.m_job->fn = std::move(fn);
    Pool* p = pool();
    if (!p || !p->push(h.m_job, lane)) {
        // Shut down: the job never runs.
        h.m_job->cancel.store(true);
        h.m_job->tryDrop();
    }
    return h;
}

void parallelFor(size_t count, const std::function<void(size_t)>& fn)
{
    if (count == 0) return;
    // Items are claimed one at a time from a shared counter, so uneven items balance out.
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) fn(i);
    };
    const size_t helpers = std::min<size_t>(workerCount(), count - 1);
    std::vector<Handle> handles;
    handles.reserve(helpers);
    for (size_t i = 0; i < helpers; ++i) handles.push_back(submit([&](const std::atomic<bool>&) { work(); }));
    // Helpers that never started run (and find nothing left) in wait(); running ones finish their item.
    // Everything they reference lives on this stack frame, so we must wait for them all, even if fn throws here.
    try {
        work();
    } catch (...) {
        next.store(count);
        for (Handle& h : handles) h.wait();
        throw;
    }
    for (Handle& h : handles) h.wait();
}

bool yieldSuggested()
{
    Pool* p = pool();
    return p && p->hasQueued();
}

unsigned hardwareThreads()
{
    return std::max(1u, std::thread::hardware_concurrency());
}

unsigned workerCount()
{
    return std::clamp(hardwareThreads() - 1u, 1u, 15u);
}

unsigned physicsThreadCount()
{
    return std::clamp(hardwareThreads() - 1u, 1u, 4u);
}

void shutdown()
{
    Pool* p = nullptr;
    {
        std::lock_guard lock(g_poolMutex);
        g_shutDown = true;
        p = g_pool.get();
    }
    // Outside the lock: a job finishing now may still call yieldSuggested().
    if (p) p->shutdown();
}

// -----------------------------------------------------------------------------
// DrainGroup
// -----------------------------------------------------------------------------

size_t DrainGroup::active()
{
    std::erase_if(m_handles, [](const Handle& h) { return h.done(); });
    return m_handles.size();
}

void DrainGroup::topUp(size_t want, const Fn& fn, Lane lane)
{
    for (size_t n = active(); n < want; ++n) {
        Handle h = submit(fn, lane);
        if (h.done()) break; // shut down
        m_handles.push_back(std::move(h));
    }
}

void DrainGroup::cancel()
{
    for (Handle& h : m_handles) h.cancel();
    m_handles.clear();
}

void DrainGroup::cancelAndWait()
{
    for (Handle& h : m_handles) h.cancel();
    for (Handle& h : m_handles) h.wait();
    m_handles.clear();
}

} // namespace ve::jobs
