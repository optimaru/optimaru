// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_RUIN_RECREATE_RELATED_RUIN_HPP
#define OPTIMARU_RUIN_RECREATE_RELATED_RUIN_HPP

#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/ruin_recreate/random_ruin.hpp"
#include "optimaru/ruin_recreate/removal_gain.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// Related ruin (Shaw): take a seed and its nearest neighbours, so the kick
// removes a cluster the recreate can genuinely re-plan together — the ruin
// that pays off when the domain has a distance meter.
//
// Falls back to the random sample when no local_search.nearby.distance is configured
// (empty neighbour lists), and tops up randomly when the seed has fewer
// neighbours than the ruin size.
template <typename Solution>
class RelatedRuin final : public RuinOperator<Solution> {
public:
    const char* name() const override { return "relatedRuin"; }

    std::vector<int> select_ruin_set(OperatorContext<Solution>& ctx, int ruin_count,
                                   std::mt19937_64& rng) override {
        if (!ctx.nearby.enabled()) return fallback_.select_ruin_set(ctx, ruin_count, rng);

        const int n = ruin_candidate_count(ctx);
        std::vector<char> mark(n, 0);
        std::vector<int> set;
        int seed = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
        mark[seed] = 1;
        set.push_back(seed);
        for (int nb : ctx.nearby.of(seed)) {
            if (static_cast<int>(set.size()) >= ruin_count) break;
            if (!mark[nb]) {
                mark[nb] = 1;
                set.push_back(nb);
            }
        }
        while (static_cast<int>(set.size()) < ruin_count) {  // few neighbors: fill randomly
            int e = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
            if (!mark[e]) {
                mark[e] = 1;
                set.push_back(e);
            }
        }
        return set;
    }

private:
    RandomRuin<Solution> fallback_;
};

}  // namespace optimaru

#endif  // OPTIMARU_RUIN_RECREATE_RELATED_RUIN_HPP
