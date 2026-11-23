// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_WORKER_THREADS_HPP
#define OPTIMARU_WORKER_THREADS_HPP

#include <atomic>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace optimaru {

// ---------------------------------------------------------------------------
// run_worker_threads
// ---------------------------------------------------------------------------
// Runs work(i) for every i in [0, count) on a thread of its own and joins
// them all: the start-up and shutdown of both parallel pools of the engine
// (Solver::solve_multistart and solve_parallel_starts).
//
// Creating a thread can fail: std::thread throws std::system_error when the
// system has no thread to give (and the thread state allocation can throw
// std::bad_alloc). A std::thread that is still joinable when it is destroyed
// calls std::terminate, so a plain emplace_back loop would take the whole
// process down along with the workers it had already started. Here, a
// creation failure
//   1. calls stop_started(), which makes the workers already running end at
//      their next step (the pools raise the run-scoped stop flag every
//      worker's engine checks once a step, or call request_stop() on each
//      engine),
//   2. joins every thread already started, and
//   3. rethrows the creation error on the calling thread.
//
// `work` is copied into each thread and must not throw: an exception leaving
// a std::thread calls std::terminate too. The pools catch inside it, keep the
// exception_ptr and stop the other workers themselves. stop_started must not
// throw either (it runs while threads are still joinable).
//
// worker_thread_start_hook is a test seam: when set, it is called on the calling
// thread just before thread `index` is created, and a test throws from it
// (std::system_error, as std::thread would) to exercise the clean-up above.
// It is nullptr outside tests; set it only while no pool is starting.
inline void (*worker_thread_start_hook)(int index) = nullptr;

template <typename Work, typename StopStarted>
void run_worker_threads(int count, const Work& work, const StopStarted& stop_started) {
    std::vector<std::thread> threads;
    threads.reserve(count > 0 ? static_cast<std::size_t>(count) : 0);
    try {
        for (int i = 0; i < count; ++i) {
            if (worker_thread_start_hook) worker_thread_start_hook(i);
            threads.emplace_back(work, i);
        }
    } catch (...) {
        stop_started();
        for (std::thread& thread : threads) thread.join();
        throw;
    }
    for (std::thread& thread : threads) thread.join();
}

// The common case: the workers share one run-scoped stop flag.
template <typename Work>
void run_worker_threads(int count, std::atomic<bool>& stop, const Work& work) {
    run_worker_threads(count, work, [&stop] { stop.store(true); });
}

}  // namespace optimaru

#endif  // OPTIMARU_WORKER_THREADS_HPP
