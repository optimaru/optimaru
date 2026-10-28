#ifndef OPTIMARU_API_SOLVER_SOLVER_MANAGER_HPP
#define OPTIMARU_API_SOLVER_SOLVER_MANAGER_HPP

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace optimaru {

// Mirrors api.solver.SolverStatus.
enum class SolverStatus { NOT_SOLVING, SOLVING_ACTIVE };

// Mirrors api.solver.SolverJob: one asynchronous solve, running on a
// background thread. The solution and calculator are borrowed by reference
// and must outlive the job; the solution must not be read while
// SOLVING_ACTIVE (it is the solver's working solution).
template <typename Solution>
class SolverJob {
public:
    SolverJob(SolverConfig config, Solution& solution,
              IncrementalScoreCalculator<Solution>& calculator)
        : solver_(std::make_unique<Solver<Solution>>(std::move(config))),
          active_(std::make_shared<std::atomic<bool>>(true)),
          finalBestScore_(std::make_shared<HardSoftScore>()) {
        Solver<Solution>* solver = solver_.get();
        auto active = active_;
        auto score = finalBestScore_;
        thread_ = std::thread([solver, active, score, &solution, &calculator] {
            *score = solver->solve(solution, calculator);
            active->store(false);
        });
    }

    SolverJob(SolverJob&&) = default;
    SolverJob& operator=(SolverJob&&) = default;
    SolverJob(const SolverJob&) = delete;
    SolverJob& operator=(const SolverJob&) = delete;

    ~SolverJob() {
        if (thread_.joinable()) {
            solver_->terminateEarly();
            thread_.join();
        }
    }

    SolverStatus getSolverStatus() const {
        return active_->load() ? SolverStatus::SOLVING_ACTIVE : SolverStatus::NOT_SOLVING;
    }

    // Cooperatively stops the solve and waits for it to return its best
    // solution (like upstream, this blocks until the solver has stopped).
    void terminateEarly() {
        solver_->terminateEarly();
        if (thread_.joinable()) thread_.join();
    }

    // Blocks until the solve ends (by termination or terminateEarly).
    HardSoftScore getFinalBestScore() {
        if (thread_.joinable()) thread_.join();
        return *finalBestScore_;
    }

private:
    std::unique_ptr<Solver<Solution>> solver_;
    std::shared_ptr<std::atomic<bool>> active_;
    std::shared_ptr<HardSoftScore> finalBestScore_;
    std::thread thread_;
};

// Mirrors api.solver.SolverManager (the submission API; job bookkeeping by
// problem id is left to the caller).
template <typename Solution>
class SolverManager {
public:
    static SolverJob<Solution> solve(SolverConfig config, Solution& solution,
                                     IncrementalScoreCalculator<Solution>& calculator) {
        return SolverJob<Solution>(std::move(config), solution, calculator);
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SOLVER_SOLVER_MANAGER_HPP
