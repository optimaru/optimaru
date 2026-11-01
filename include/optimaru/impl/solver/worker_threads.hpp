#ifndef OPTIMARU_IMPL_SOLVER_WORKER_THREADS_HPP
#define OPTIMARU_IMPL_SOLVER_WORKER_THREADS_HPP

#include <atomic>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace optimaru {

// ---------------------------------------------------------------------------
// runWorkerThreads
// ---------------------------------------------------------------------------
// Runs work(i) for every i in [0, count) on a thread of its own and joins
// them all: the start-up and shutdown of every parallel pool in the engine
// (DefaultSolver::solveMultiStart) and in the use cases' own worker pools.
//
// Creating a thread can fail: std::thread throws std::system_error when the
// system has no thread to give (and the thread state allocation can throw
// std::bad_alloc). A std::thread that is still joinable when it is destroyed
// calls std::terminate, so a plain emplace_back loop would take the whole
// process down along with the workers it had already started. Here, a
// creation failure
//   1. calls stopStarted(), which makes the workers already running end at
//      their next step (the pools raise the run-scoped stop flag every
//      worker's engine checks once a step, or call terminateEarly() on each
//      engine),
//   2. joins every thread already started, and
//   3. rethrows the creation error on the calling thread.
//
// `work` is copied into each thread and must not throw: an exception leaving
// a std::thread calls std::terminate too. The pools catch inside it, keep the
// exception_ptr and stop the other workers themselves. stopStarted must not
// throw either (it runs while threads are still joinable).
//
// workerThreadStartHook is a test seam: when set, it is called on the calling
// thread just before thread `index` is created, and a test throws from it
// (std::system_error, as std::thread would) to exercise the clean-up above.
// It is nullptr outside tests; set it only while no pool is starting.
inline void (*workerThreadStartHook)(int index) = nullptr;

template <typename Work, typename StopStarted>
void runWorkerThreads(int count, const Work& work, const StopStarted& stopStarted) {
    std::vector<std::thread> threads;
    threads.reserve(count > 0 ? static_cast<std::size_t>(count) : 0);
    try {
        for (int i = 0; i < count; ++i) {
            if (workerThreadStartHook) workerThreadStartHook(i);
            threads.emplace_back(work, i);
        }
    } catch (...) {
        stopStarted();
        for (std::thread& thread : threads) thread.join();
        throw;
    }
    for (std::thread& thread : threads) thread.join();
}

// The common case: the workers share one run-scoped stop flag.
template <typename Work>
void runWorkerThreads(int count, std::atomic<bool>& stop, const Work& work) {
    runWorkerThreads(count, work, [&stop] { stop.store(true); });
}

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_SOLVER_WORKER_THREADS_HPP
