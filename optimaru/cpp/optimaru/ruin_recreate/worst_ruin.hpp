// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_RUIN_RECREATE_WORST_RUIN_HPP
#define OPTIMARU_RUIN_RECREATE_WORST_RUIN_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/score/score.hpp"
#include "optimaru/ruin_recreate/removal_gain.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// Worst ruin: rank candidates by how much the score improves when they are
// taken out (removal_gain) and remove the worst placed ones. The pick is
// power-law randomized (p = 3, Ropke & Pisinger) instead of strictly greedy,
// so repeated kicks do not keep removing the same handful.
//
// The ranking costs one full score evaluation per candidate, which is why
// this only runs inside an LNS kick and never on the move path.
template <typename Solution>
class WorstRuin final : public RuinOperator<Solution> {
public:
    const char* name() const override { return "worstRuin"; }

    std::vector<int> select_ruin_set(OperatorContext<Solution>& ctx, int ruin_count,
                                     std::mt19937_64& rng) override {
        const int n = ruin_candidate_count(ctx);
        std::vector<std::pair<Score, int>> gains;
        gains.reserve(n);
        for (int e = 0; e < n; ++e) gains.emplace_back(removal_gain(ctx, e), e);
        std::sort(gains.begin(), gains.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::vector<int> set;
        set.reserve(ruin_count);
        while (static_cast<int>(set.size()) < ruin_count && !gains.empty()) {
            auto idx = static_cast<std::size_t>(std::pow(unit(rng), 3.0) *
                                                static_cast<double>(gains.size()));
            if (idx >= gains.size()) idx = gains.size() - 1;
            set.push_back(gains[idx].second);
            gains.erase(gains.begin() + static_cast<std::ptrdiff_t>(idx));
        }
        return set;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_RUIN_RECREATE_WORST_RUIN_HPP
