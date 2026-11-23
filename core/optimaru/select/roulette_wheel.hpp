// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SELECT_ROULETTE_WHEEL_HPP
#define OPTIMARU_SELECT_ROULETTE_WHEEL_HPP

#include <cstddef>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/select/scheme.hpp"
#include "optimaru/portable_random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// RouletteWheelSelector
// ---------------------------------------------------------------------------
// Ropke & Pisinger's scheme: each operator starts with weight 1; after every
// application the applied operator's weight becomes a convex combination of
// its current weight and the reward of the observed outcome:
//
//     w = decay * w + (1 - decay) * reward(outcome)
//
// so operators that keep producing new bests get selected more often.
class RouletteWheelSelector : public OperatorSelectionScheme {
public:
    // rewards indexed by MoveOutcome; decay in [0, 1] (higher = slower adaptation).
    RouletteWheelSelector(int operator_count, std::vector<double> rewards, double decay)
        : OperatorSelectionScheme(operator_count),
          weights_(static_cast<std::size_t>(operator_count), 1.0),
          rewards_(std::move(rewards)),
          decay_(decay) {}

    int select(std::mt19937_64& rng) override {
        if (weights_.size() <= 1) return 0;
        double total = 0.0;
        for (double w : weights_) total += w;
        // Degenerate wheel (all weights decayed to ~0, or a non-finite total):
        // fall back to uniform rather than always returning the last operator.
        if (!std::isfinite(total) || total <= 0.0) {
            return random_index(rng, static_cast<int>(weights_.size()));
        }
        std::uniform_real_distribution<double> pick(0.0, total);
        double r = pick(rng);
        for (std::size_t i = 0; i < weights_.size(); ++i) {
            r -= weights_[i];
            if (r <= 0) return static_cast<int>(i);
        }
        return static_cast<int>(weights_.size()) - 1;
    }

    void update(int operator_index, MoveOutcome outcome) override {
        const double reward = rewards_[static_cast<std::size_t>(outcome)];
        double& w = weights_[static_cast<std::size_t>(operator_index)];
        w = decay_ * w + (1.0 - decay_) * reward;
    }

    void reset() override { weights_.assign(weights_.size(), 1.0); }

protected:
    std::vector<double> weights_;
    std::vector<double> rewards_;
    double decay_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SELECT_ROULETTE_WHEEL_HPP
