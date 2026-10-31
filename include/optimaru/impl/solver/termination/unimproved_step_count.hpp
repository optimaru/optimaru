#ifndef OPTIMARU_IMPL_SOLVER_TERMINATION_UNIMPROVED_STEP_COUNT_HPP
#define OPTIMARU_IMPL_SOLVER_TERMINATION_UNIMPROVED_STEP_COUNT_HPP

#include <cstdint>

#include "optimaru/impl/solver/termination/termination_criterion.hpp"

namespace optimaru {

// Stops when the best score has not improved for that many steps.
class UnimprovedStepCountTermination final : public Termination {
public:
    explicit UnimprovedStepCountTermination(std::int64_t unimprovedStepCountLimit)
        : unimprovedStepCountLimit_(unimprovedStepCountLimit) {}
    bool isTerminated(const TerminationContext& c) const override {
        return c.step - c.lastImprovementStep >= unimprovedStepCountLimit_;
    }

private:
    std::int64_t unimprovedStepCountLimit_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_SOLVER_TERMINATION_UNIMPROVED_STEP_COUNT_HPP
