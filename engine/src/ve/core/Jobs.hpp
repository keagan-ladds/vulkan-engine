#pragma once

// =============================================================================
// Jobs - the engine's one pool of worker threads.
//
// World systems (foliage placement, terrain meshes, grass bakes, navmesh
// tiles, shader compiles) used to start their own threads; together that was
// several times more threads than cores. Now they all submit work here:
//
//   jobs::Handle h = jobs::submit([input](const std::atomic<bool>& cancelled) {
//       for (...) { if (cancelled) return; ... }   // poll the flag in long loops
//   });
//   if (h.done()) ...        // poll once a frame (main thread)
//   h.cancel();              // cooperative: a queued job never runs, a running one sees the flag
//   h.wait();                // block (runs the job right here if no worker started it yet)
//
//   jobs::parallelFor(cells.size(), [&](size_t i) { out[i] = generate(cells[i]); });
//
// Rules:
//   - Workers never run main-thread-only engine code: no Scene / GameObject /
//     Component access, no Renderer / Device / Mesh::create, no physics
//     world, no audio, no ImGui. A job computes plain data from inputs it owns
//     (or that its owner guarantees outlive it) and hands the result back
//     through something the main thread polls.
//   - Anything a job touches must outlive it. Prefer moving the inputs into
//     the job and sharing the output through a shared_ptr: the owner can then
//     drop the handle (cancel + forget) without waiting, so destructors never
//     block on a long job. Only a job that reads its owner (`this`) needs the
//     owner to cancel() + wait() before dying, and must then stop quickly.
//   - Jobs should be short (milliseconds). Long-running loops over a queue
//     call yieldSuggested() between items and return when it says so, so
//     other systems' jobs get a turn (the pool is FIFO, not preemptive).
//
// The pool is created on first use with hardwareThreads() - 1 workers (1 to
// 15) and shut down by the Engine after the scene and renderer are gone:
// queued jobs are cancelled, running ones finish, the threads are joined.
// After shutdown, submit() returns an already cancelled handle.
// =============================================================================

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace ve::jobs {

/// The job body. `cancelled` is the handle's cancel flag: check it in long loops.
using Fn = std::function<void(const std::atomic<bool>& cancelled)>;

enum class Lane {
    Normal,     ///< world streaming: taken first, in submission order
    Background, ///< nobody waits for it soon (dev tools): only when Normal is empty
};

namespace detail {
struct Job;
}

/// Refers to one submitted job. Cheap to copy; dropping it never blocks (the
/// job keeps running or stays queued; cancel() first to stop it).
class Handle {
public:
    Handle() = default;
    /// Whether this refers to a job (false for a default-constructed Handle).
    bool valid() const { return m_job != nullptr; }
    /// Finished, or cancelled before it started. True for an empty handle.
    bool done() const;
    /// cancel() was called (or the pool shut down before the job ran).
    bool cancelled() const;
    /// Sets the cancel flag: a job that hasn't started never runs, a running
    /// one sees the flag (it decides when to stop). Doesn't wait.
    void cancel();
    /// Blocks until done(). A job no worker has started yet runs right here,
    /// so waiting can't deadlock on a busy pool.
    void wait();

private:
    friend Handle submit(Fn fn, Lane lane);
    std::shared_ptr<detail::Job> m_job;
};

Handle submit(Fn fn, Lane lane = Lane::Normal);

/// Calls fn(i) for i in [0, count), spread over the workers; the calling thread
/// helps and returns when every call is done. fn must be safe to call
/// concurrently and shouldn't throw (on a worker that is only logged).
void parallelFor(size_t count, const std::function<void(size_t)>& fn);

/// True while other jobs are queued: a job draining a long queue returns (and
/// is submitted again later) so a busy system doesn't hog the workers.
bool yieldSuggested();

/// std::thread::hardware_concurrency(), at least 1 (it may report 0 = unknown).
unsigned hardwareThreads();
/// Pool workers: hardwareThreads() - 1, clamped to [1, 15].
unsigned workerCount();
/// Jolt's own thread pool (physics keeps it: its job system has barriers and
/// dependencies ours doesn't). Its workers only run inside a physics step,
/// while the main thread waits, so the overlap with the pool is bounded:
/// hardwareThreads() - 1, clamped to [1, 4].
unsigned physicsThreadCount();

/// Engine shutdown: stop accepting work, cancel queued jobs, let running ones
/// finish, join the workers. Safe to call more than once.
void shutdown();

/// Up to N jobs draining one queue a system owns (the system keeps its own
/// ordering: sorted, nearest-first, ...). Main thread only.
class DrainGroup {
public:
    /// Jobs still queued or running (finished ones are forgotten).
    size_t active();
    /// Submits fn until `want` jobs are active.
    void topUp(size_t want, const Fn& fn, Lane lane = Lane::Normal);
    /// Cancels and forgets every job without waiting (they must own what they touch).
    void cancel();
    /// Cancels and waits for running jobs: for jobs that read their owner.
    void cancelAndWait();

private:
    std::vector<Handle> m_handles;
};

} // namespace ve::jobs
