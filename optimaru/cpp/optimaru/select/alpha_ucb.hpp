// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SELECT_ALPHA_UCB_HPP
#define OPTIMARU_SELECT_ALPHA_UCB_HPP

#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/select/scheme.hpp"

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
    AlphaUcbSelector(int operator_count, std::vector<double> rewards, double alpha)
        : OperatorSelectionScheme(operator_count),
          rewards_(std::move(rewards)),
          alpha_(alpha),
          avg_rewards_(static_cast<std::size_t>(operator_count), 1.0),
          times_(static_cast<std::size_t>(operator_count), 0) {}

    int select(std::mt19937_64& rng) override {
        // Untried actions first, so every operator is measured at least once.
        untried_.clear();
        for (std::size_t i = 0; i < times_.size(); ++i) {
            if (times_[i] == 0) untried_.push_back(static_cast<int>(i));
        }
        if (!untried_.empty()) {
            return untried_[rng() % static_cast<std::uint64_t>(untried_.size())];
        }

        const double bonus_numerator = alpha_ * std::log(1.0 + static_cast<double>(iteration_));
        double best_value = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < avg_rewards_.size(); ++i) {
            const double value = avg_rewards_[i] +
                                 std::sqrt(bonus_numerator /
                                           static_cast<double>(std::max<std::int64_t>(times_[i], 1)));
            if (value > best_value) best_value = value;
        }
        // Ties broken uniformly, matching numpy's isclose-then-choose.
        untried_.clear();
        for (std::size_t i = 0; i < avg_rewards_.size(); ++i) {
            const double value = avg_rewards_[i] +
                                 std::sqrt(bonus_numerator /
                                           static_cast<double>(std::max<std::int64_t>(times_[i], 1)));
            if (value >= best_value - 1e-9) untried_.push_back(static_cast<int>(i));
        }
        return untried_[rng() % static_cast<std::uint64_t>(untried_.size())];
    }

    void update(int operator_index, MoveOutcome outcome) override {
        const auto i = static_cast<std::size_t>(operator_index);
        const double reward = rewards_[static_cast<std::size_t>(outcome)];
        const auto played = static_cast<double>(times_[i]);
        avg_rewards_[i] = (played * avg_rewards_[i] + reward) / (played + 1.0);
        ++times_[i];
        ++iteration_;
    }

    void reset() override {
        avg_rewards_.assign(avg_rewards_.size(), 1.0);
        times_.assign(times_.size(), 0);
        iteration_ = 0;
    }

private:
    std::vector<double> rewards_;
    double alpha_;
    std::vector<double> avg_rewards_;
    std::vector<std::int64_t> times_;
    std::int64_t iteration_ = 0;
    std::vector<int> untried_;  // scratch, reused across selects
};

}  // namespace optimaru

#endif  // OPTIMARU_SELECT_ALPHA_UCB_HPP
