// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_BACKGROUND_SOLVE_HPP
#define OPTIMARU_BACKGROUND_SOLVE_HPP

#include <atomic>
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
template <typename Solution>
class BackgroundSolve {
public:
    BackgroundSolve(Settings settings, Solution& solution, Objective<Solution>& objective)
        : solver_(std::make_unique<Solver<Solution>>(std::move(settings))),
          active_(std::make_shared<std::atomic<bool>>(true)),
          final_score_(std::make_shared<Score>()) {
        require_pinned_values<SolutionTraits<Solution>>(solution);
        Solver<Solution>* solver = solver_.get();
        auto active = active_;
        auto score = final_score_;
        thread_ = std::thread([solver, active, score, &solution, &objective] {
            *score = solver->solve(solution, objective);
            active->store(false);
        });
    }

    BackgroundSolve(BackgroundSolve&&) = default;
    BackgroundSolve& operator=(BackgroundSolve&&) = default;
    BackgroundSolve(const BackgroundSolve&) = delete;
    BackgroundSolve& operator=(const BackgroundSolve&) = delete;

    ~BackgroundSolve() {
        if (thread_.joinable()) {
            solver_->request_stop();
            thread_.join();
        }
    }

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
    // the best score.
    Score wait() {
        if (thread_.joinable()) thread_.join();
        return *final_score_;
    }

private:
    std::unique_ptr<Solver<Solution>> solver_;
    std::shared_ptr<std::atomic<bool>> active_;
    std::shared_ptr<Score> final_score_;
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
