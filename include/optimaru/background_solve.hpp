// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_BACKGROUND_SOLVE_HPP
#define OPTIMARU_BACKGROUND_SOLVE_HPP

#include <atomic>
#include <exception>
#include <memory>
#include <thread>
#include <utility>

#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

enum class SolveStatus { Idle, Running };

// One solve running on a thread of its own; start one with
// solve_in_background(). The solution and objective are borrowed by
// reference and must outlive the job; the solution must not be read while
// the job is Running (the search is writing to it).
//
// An exception thrown by the solve (a ScoreMismatchError under
// ScoreChecks::Full, a throwing on_new_best callback, bad_alloc) is caught on
// the worker thread and rethrown by wait(). Assigning over a job, or
// destroying it, stops its solve and joins the thread first.
template <typename Solution>
class BackgroundSolve {
public:
    BackgroundSolve(Settings settings, Solution& solution, Objective<Solution>& objective)
        : solver_(std::make_unique<Solver<Solution>>(std::move(settings))),
          active_(std::make_shared<std::atomic<bool>>(true)),
          final_score_(std::make_shared<Score>()),
          failure_(std::make_shared<std::exception_ptr>()) {
        require_pinned_values<SolutionTraits<Solution>>(solution);
        Solver<Solution>* solver = solver_.get();
        auto active = active_;
        auto score = final_score_;
        auto failure = failure_;
        thread_ = std::thread([solver, active, score, failure, &solution, &objective] {
            // An exception escaping a std::thread calls std::terminate, so
            // keep it for wait() instead.
            try {
                *score = solver->solve(solution, objective);
            } catch (...) {
                *failure = std::current_exception();
            }
            active->store(false);
        });
    }

    BackgroundSolve(BackgroundSolve&&) noexcept = default;
    BackgroundSolve& operator=(BackgroundSolve&& other) noexcept {
        if (this != &other) {
            // Stop and join this job's own solve before its Solver goes: the
            // worker thread still uses it, and a joinable std::thread must
            // not be assigned over.
            finish();
            solver_ = std::move(other.solver_);
            active_ = std::move(other.active_);
            final_score_ = std::move(other.final_score_);
            failure_ = std::move(other.failure_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    BackgroundSolve(const BackgroundSolve&) = delete;
    BackgroundSolve& operator=(const BackgroundSolve&) = delete;

    ~BackgroundSolve() { finish(); }

    SolveStatus status() const {
        return active_->load() ? SolveStatus::Running : SolveStatus::Idle;
    }

    // Asks the solve to stop at its next step and waits until it has: on
    // return the solution holds the best one found.
    void request_stop() {
        solver_->request_stop();
        if (thread_.joinable()) thread_.join();
    }

    // Blocks until the solve ends (by its limits or request_stop) and returns
    // the best score, or rethrows the exception the solve ended with.
    Score wait() {
        if (thread_.joinable()) thread_.join();
        if (*failure_) std::rethrow_exception(*failure_);
        return *final_score_;
    }

private:
    void finish() noexcept {
        if (thread_.joinable()) {
            solver_->request_stop();
            thread_.join();
        }
    }

    std::unique_ptr<Solver<Solution>> solver_;
    std::shared_ptr<std::atomic<bool>> active_;
    std::shared_ptr<Score> final_score_;
    std::shared_ptr<std::exception_ptr> failure_;
    std::thread thread_;
};

// Starts `solution` solving on a background thread and returns at once.
template <typename Solution>
BackgroundSolve<Solution> solve_in_background(Settings settings, Solution& solution,
                                              Objective<Solution>& objective) {
    return BackgroundSolve<Solution>(std::move(settings), solution, objective);
}

}  // namespace optimaru

#endif  // OPTIMARU_BACKGROUND_SOLVE_HPP
