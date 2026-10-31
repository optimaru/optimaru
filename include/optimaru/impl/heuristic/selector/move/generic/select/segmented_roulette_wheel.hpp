#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_SEGMENTED_ROULETTE_WHEEL_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_SEGMENTED_ROULETTE_WHEEL_HPP

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/impl/heuristic/selector/move/generic/select/roulette_wheel.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// SegmentedRouletteWheelSelector
// ---------------------------------------------------------------------------
// Extends the roulette wheel by holding the weights FIXED for a segment of
// segmentLength iterations. Observed rewards accumulate into a per-operator
// segment score; at the segment boundary the scores fold into the weights with
// the same convex combination, and the segment scores reset.
//
// Compared to plain roulette this reacts to a whole phase of the search rather
// than to a single lucky iteration — the ruin that only pays off in the
// endgame is not weighted out of existence by an unlucky early streak.
class SegmentedRouletteWheelSelector final : public RouletteWheelSelector {
public:
    SegmentedRouletteWheelSelector(int operatorCount, std::vector<double> rewards, double decay,
                                   std::int64_t segmentLength)
        : RouletteWheelSelector(operatorCount, std::move(rewards), decay),
          segmentScores_(static_cast<std::size_t>(operatorCount), 0.0),
          segmentLength_(segmentLength < 1 ? 1 : segmentLength) {}

    void update(int operatorIndex, LnsOutcome outcome) override {
        segmentScores_[static_cast<std::size_t>(operatorIndex)] +=
                rewards_[static_cast<std::size_t>(outcome)];
        ++iteration_;
        if (iteration_ % segmentLength_ != 0) return;
        for (std::size_t i = 0; i < weights_.size(); ++i) {
            weights_[i] = decay_ * weights_[i] + (1.0 - decay_) * segmentScores_[i];
        }
        segmentScores_.assign(segmentScores_.size(), 0.0);
    }

    void reset() override {
        RouletteWheelSelector::reset();
        segmentScores_.assign(segmentScores_.size(), 0.0);
        iteration_ = 0;
    }

private:
    std::vector<double> segmentScores_;
    std::int64_t segmentLength_;
    std::int64_t iteration_ = 0;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_SEGMENTED_ROULETTE_WHEEL_HPP
