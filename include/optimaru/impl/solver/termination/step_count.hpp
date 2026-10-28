#ifndef OPTIMARU_IMPL_SOLVER_TERMINATION_STEP_COUNT_HPP
#define OPTIMARU_IMPL_SOLVER_TERMINATION_STEP_COUNT_HPP

#include <chrono>
#include <cstdint>

#include "optimaru/impl/solver/termination/termination_criterion.hpp"

namespace optimaru {

// Stops after a number of steps — a wall-clock-independent budget.
class StepCountTermination final : public Termination {
public:
    explicit StepCountTermination(std::int64_t stepCountLimit)
        : stepCountLimit_(stepCountLimit) {}
    bool isTerminated(const TerminationContext& c) const override {
        return c.step >= stepCountLimit_;
    }

private:
    std::int64_t stepCountLimit_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_SOLVER_TERMINATION_STEP_COUNT_HPP
