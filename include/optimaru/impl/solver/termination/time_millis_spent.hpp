#ifndef OPTIMARU_IMPL_SOLVER_TERMINATION_TIME_MILLIS_SPENT_HPP
#define OPTIMARU_IMPL_SOLVER_TERMINATION_TIME_MILLIS_SPENT_HPP

#include <chrono>

#include "optimaru/impl/solver/termination/termination_criterion.hpp"

namespace optimaru {

// Stops after a wall-clock budget — the default exit of every solve.
class TimeMillisSpentTermination final : public Termination {
public:
    explicit TimeMillisSpentTermination(std::chrono::milliseconds spentLimit)
        : spentLimit_(spentLimit) {}
    bool isTerminated(const TerminationContext& c) const override {
        return c.now - c.start >= spentLimit_;
    }

private:
    std::chrono::milliseconds spentLimit_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_SOLVER_TERMINATION_TIME_MILLIS_SPENT_HPP
