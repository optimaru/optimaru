#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_ALPHA_UCB_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_ALPHA_UCB_HPP

#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/impl/heuristic/selector/move/generic/select/operator_selection_scheme.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// AlphaUcbSelector
// ---------------------------------------------------------------------------
// The alpha-UCB bandit scheme (Hendel 2022). Plays the action maximising
//
//     avgReward(a) + sqrt(alpha * ln(1 + t) / times(a))
//
// with every untried action played first. alpha near 0 exploits, near 1
// explores; alpha <= 0.1 is the usual choice. Average rewards start at 1, so
// the reward vector should be "reasonable" relative to that.
class AlphaUcbSelector final : public OperatorSelectionScheme {
public:
    AlphaUcbSelector(int operatorCount, std::vector<double> rewards, double alpha)
        : OperatorSelectionScheme(operatorCount),
          rewards_(std::move(rewards)),
          alpha_(alpha),
          avgRewards_(static_cast<std::size_t>(operatorCount), 1.0),
          times_(static_cast<std::size_t>(operatorCount), 0) {}

    int select(std::mt19937_64& rng) override {
        // Untried actions first, so every operator is measured at least once.
        untried_.clear();
        for (std::size_t i = 0; i < times_.size(); ++i) {
            if (times_[i] == 0) untried_.push_back(static_cast<int>(i));
        }
        if (!untried_.empty()) {
            return untried_[rng() % static_cast<std::uint64_t>(untried_.size())];
        }

        const double bonusNumerator = alpha_ * std::log(1.0 + static_cast<double>(iteration_));
        double bestValue = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < avgRewards_.size(); ++i) {
            const double value = avgRewards_[i] +
                                 std::sqrt(bonusNumerator /
                                           static_cast<double>(std::max<std::int64_t>(times_[i], 1)));
            if (value > bestValue) bestValue = value;
        }
        // Ties broken uniformly, matching numpy's isclose-then-choose.
        untried_.clear();
        for (std::size_t i = 0; i < avgRewards_.size(); ++i) {
            const double value = avgRewards_[i] +
                                 std::sqrt(bonusNumerator /
                                           static_cast<double>(std::max<std::int64_t>(times_[i], 1)));
            if (value >= bestValue - 1e-9) untried_.push_back(static_cast<int>(i));
        }
        return untried_[rng() % static_cast<std::uint64_t>(untried_.size())];
    }

    void update(int operatorIndex, LnsOutcome outcome) override {
        const auto i = static_cast<std::size_t>(operatorIndex);
        const double reward = rewards_[static_cast<std::size_t>(outcome)];
        const auto played = static_cast<double>(times_[i]);
        avgRewards_[i] = (played * avgRewards_[i] + reward) / (played + 1.0);
        ++times_[i];
        ++iteration_;
    }

    void reset() override {
        avgRewards_.assign(avgRewards_.size(), 1.0);
        times_.assign(times_.size(), 0);
        iteration_ = 0;
    }

private:
    std::vector<double> rewards_;
    double alpha_;
    std::vector<double> avgRewards_;
    std::vector<std::int64_t> times_;
    std::int64_t iteration_ = 0;
    std::vector<int> untried_;  // scratch, reused across selects
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_SELECT_ALPHA_UCB_HPP
