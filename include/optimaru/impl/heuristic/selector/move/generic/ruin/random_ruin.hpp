#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_RANDOM_RUIN_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_RANDOM_RUIN_HPP

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/impl/heuristic/selector/move/generic/ruin/removal_gain.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace optimaru {

// Random ruin: a uniform sample of the ruin size, drawn with a partial
// Fisher-Yates shuffle so the result is duplicate-free by construction.
// The baseline every other ruin is measured against.
template <typename Solution>
class RandomRuin final : public RuinOperator<Solution> {
public:
    const char* name() const override { return "randomRuin"; }

    std::vector<int> selectRuinSet(OperatorContext<Solution>& ctx, int ruinCount,
                                   std::mt19937_64& rng) override {
        const int n = ruinCandidateCount(ctx);
        std::vector<int> ids(n);
        for (int i = 0; i < n; ++i) ids[i] = i;
        for (int i = 0; i < ruinCount; ++i) {
            int j = i + static_cast<int>(rng() % static_cast<std::uint64_t>(n - i));
            std::swap(ids[i], ids[j]);
        }
        ids.resize(ruinCount);
        return ids;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_RANDOM_RUIN_HPP
