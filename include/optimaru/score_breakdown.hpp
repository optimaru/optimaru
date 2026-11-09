// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SCORE_BREAKDOWN_HPP
#define OPTIMARU_SCORE_BREAKDOWN_HPP

#include <string>
#include <vector>

#include "optimaru/score.hpp"

namespace optimaru {

// One constraint's (weighted) share of the total score.
struct ConstraintScore {
    std::string name;
    Score score;
};

// The total score split per constraint: which constraint costs how much.
// Produced by explain_score(); the split needs the constraints registered as
// named components of a WeightedObjective (any other objective yields a
// single entry).
struct ScoreBreakdown {
    Score score;
    std::vector<ConstraintScore> constraints;

    std::string str() const {
        std::string out = score.str();
        for (const ConstraintScore& c : constraints) {
            out += "\n  " + c.name + ": " + c.score.str();
        }
        return out;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_SCORE_BREAKDOWN_HPP
