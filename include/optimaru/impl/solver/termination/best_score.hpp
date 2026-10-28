#ifndef OPTIMARU_IMPL_SOLVER_TERMINATION_BEST_SCORE_HPP
#define OPTIMARU_IMPL_SOLVER_TERMINATION_BEST_SCORE_HPP

#include "optimaru/api/score/buildin/hard_soft_score.hpp"
#include "optimaru/impl/solver/termination/termination_criterion.hpp"

namespace optimaru {

// Stops once the best score reaches a target (the "good enough" exit).
class BestScoreTermination final : public Termination {
public:
    explicit BestScoreTermination(const HardSoftScore& bestScoreLimit)
        : bestScoreLimit_(bestScoreLimit) {}
    bool isTerminated(const TerminationContext& c) const override {
        return c.bestScore >= bestScoreLimit_;
    }

private:
    HardSoftScore bestScoreLimit_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_SOLVER_TERMINATION_BEST_SCORE_HPP
