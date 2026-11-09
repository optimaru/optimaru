// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_SELECTION_SEGMENTED_ROULETTE_WHEEL_HPP
#define OPTIMARU_SEARCH_SELECTION_SEGMENTED_ROULETTE_WHEEL_HPP

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/search/selection/roulette_wheel.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// SegmentedRouletteWheelSelector
// ---------------------------------------------------------------------------
// Extends the roulette wheel by holding the weights FIXED for a segment of
// segment_length iterations. Observed rewards accumulate into a per-operator
// segment score; at the segment boundary the scores fold into the weights with
// the same convex combination, and the segment scores reset.
//
// Compared to plain roulette this reacts to a whole phase of the search rather
// than to a single lucky iteration — the ruin that only pays off in the
// endgame is not weighted out of existence by an unlucky early streak.
class SegmentedRouletteWheelSelector final : public RouletteWheelSelector {
public:
    SegmentedRouletteWheelSelector(int operator_count, std::vector<double> rewards, double decay,
                                   std::int64_t segment_length)
        : RouletteWheelSelector(operator_count, std::move(rewards), decay),
          segment_scores_(static_cast<std::size_t>(operator_count), 0.0),
          segment_length_(segment_length < 1 ? 1 : segment_length) {}

    void update(int operator_index, MoveOutcome outcome) override {
        segment_scores_[static_cast<std::size_t>(operator_index)] +=
                rewards_[static_cast<std::size_t>(outcome)];
        ++iteration_;
        if (iteration_ % segment_length_ != 0) return;
        for (std::size_t i = 0; i < weights_.size(); ++i) {
            weights_[i] = decay_ * weights_[i] + (1.0 - decay_) * segment_scores_[i];
        }
        segment_scores_.assign(segment_scores_.size(), 0.0);
    }

    void reset() override {
        RouletteWheelSelector::reset();
        segment_scores_.assign(segment_scores_.size(), 0.0);
        iteration_ = 0;
    }

private:
    std::vector<double> segment_scores_;
    std::int64_t segment_length_;
    std::int64_t iteration_ = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_SELECTION_SEGMENTED_ROULETTE_WHEEL_HPP
