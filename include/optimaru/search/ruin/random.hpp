// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_RUIN_RANDOM_HPP
#define OPTIMARU_SEARCH_RUIN_RANDOM_HPP

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/search/ruin/removal_gain.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// Random ruin: a uniform sample of the ruin size, drawn with a partial
// Fisher-Yates shuffle so the result is duplicate-free by construction.
// The baseline every other ruin is measured against.
template <typename Solution>
class RandomRuin final : public RuinOperator<Solution> {
public:
    const char* name() const override { return "randomRuin"; }

    std::vector<int> select_ruin_set(OperatorContext<Solution>& ctx, int ruin_count,
                                   std::mt19937_64& rng) override {
        const int n = ruin_candidate_count(ctx);
        std::vector<int> ids(n);
        for (int i = 0; i < n; ++i) ids[i] = i;
        for (int i = 0; i < ruin_count; ++i) {
            int j = i + static_cast<int>(rng() % static_cast<std::uint64_t>(n - i));
            std::swap(ids[i], ids[j]);
        }
        ids.resize(ruin_count);
        return ids;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_RUIN_RANDOM_HPP
