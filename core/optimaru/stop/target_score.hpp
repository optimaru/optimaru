// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_TARGET_SCORE_HPP
#define OPTIMARU_STOP_TARGET_SCORE_HPP

#include "optimaru/score/score.hpp"
#include "optimaru/stop/stop_state.hpp"

namespace optimaru {

// Stop once the best score reaches `target` (Limits::target_score; the
// default, Score::unreachable(), never fires).
class TargetScore {
public:
    explicit TargetScore(const Score& target) : target_(target) {}

    bool reached(const StopState& s) const { return s.best_score >= target_; }

private:
    Score target_;
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_TARGET_SCORE_HPP
