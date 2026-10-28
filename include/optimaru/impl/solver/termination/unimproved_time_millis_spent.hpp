#ifndef OPTIMARU_IMPL_SOLVER_TERMINATION_UNIMPROVED_TIME_MILLIS_SPENT_HPP
#define OPTIMARU_IMPL_SOLVER_TERMINATION_UNIMPROVED_TIME_MILLIS_SPENT_HPP

#include <chrono>
#include <cstdint>

#include "optimaru/impl/solver/termination/termination_criterion.hpp"

namespace optimaru {

// Stops when the best score has not improved for that long (stagnation exit).
class UnimprovedTimeMillisSpentTermination final : public Termination {
public:
    explicit UnimprovedTimeMillisSpentTermination(std::chrono::milliseconds unimprovedSpentLimit)
        : unimprovedSpentLimit_(unimprovedSpentLimit) {}
    bool isTerminated(const TerminationContext& c) const override {
        return c.now - c.lastImprovement >= unimprovedSpentLimit_;
    }

private:
    std::chrono::milliseconds unimprovedSpentLimit_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_SOLVER_TERMINATION_UNIMPROVED_TIME_MILLIS_SPENT_HPP
