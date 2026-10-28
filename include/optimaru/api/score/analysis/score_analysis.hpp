#ifndef OPTIMARU_API_SCORE_ANALYSIS_SCORE_ANALYSIS_HPP
#define OPTIMARU_API_SCORE_ANALYSIS_SCORE_ANALYSIS_HPP

#include <string>
#include <vector>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace optimaru {

// Mirrors api.score.analysis.ConstraintAnalysis: one constraint's (weighted)
// contribution to the total score.
struct ConstraintAnalysis {
    std::string constraintName;
    HardSoftScore score;
};

// Mirrors api.score.analysis.ScoreAnalysis: the total score broken down per
// constraint — "which constraint costs how much". Produced by
// SolutionManager::analyze(); the per-constraint split requires the
// constraints to be registered as named components of a
// CompositeIncrementalScoreCalculator (a monolithic calculator yields a
// single unnamed entry).
struct ScoreAnalysis {
    HardSoftScore score;
    std::vector<ConstraintAnalysis> constraints;

    std::string toString() const {
        std::string out = score.toString();
        for (const ConstraintAnalysis& c : constraints) {
            out += "\n  " + c.constraintName + ": " + c.score.toString();
        }
        return out;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SCORE_ANALYSIS_SCORE_ANALYSIS_HPP
