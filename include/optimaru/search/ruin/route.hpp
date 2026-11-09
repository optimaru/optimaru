// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_RUIN_ROUTE_HPP
#define OPTIMARU_SEARCH_RUIN_ROUTE_HPP

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/solution_traits.hpp"
#include "optimaru/search/ruin/random.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Route ruin — empty whole lists
// ---------------------------------------------------------------------------
// Route destroy: pick random lists and take
// *everything* on them, rather than a sample spread across all lists.
//
// This is the only ruin that can change the number of routes in use. Element-
// wise ruins hand the recreate a set of holes it will refill roughly where they
// were; emptying a list lets the recreate redistribute a whole vehicle's work
// and leave that vehicle unused (or repurposed) — the move that fixes an
// instance stuck one route above its true optimum. It is deliberately the
// coarsest operator in the pool, which is exactly why it must be adaptively
// weighted rather than fired on a fixed schedule.
//
// sequence shape only; delegates to the random ruin for assignment solutions.
template <typename Solution>
class RouteRuin final : public RuinOperator<Solution> {
    using Traits = SolutionTraits<Solution>;

public:
    const char* name() const override { return "routeRuin"; }

    std::vector<int> select_ruin_set(OperatorContext<Solution>& ctx, int ruin_count,
                                   std::mt19937_64& rng) override {
        if constexpr (Traits::shape != Shape::Sequence) {
            return fallback_.select_ruin_set(ctx, ruin_count, rng);
        } else {
            const Solution& solution = ctx.editor.solution();
            const int list_count = Traits::list_count(solution);
            if (list_count == 0) return {};

            // Visit the lists in a random order and drain them until the ruin
            // budget is spent; a partially drained last list is fine (the
            // budget is what keeps the kick recoverable).
            std::vector<int> order(static_cast<std::size_t>(list_count));
            for (int l = 0; l < list_count; ++l) order[static_cast<std::size_t>(l)] = l;
            std::shuffle(order.begin(), order.end(), rng);

            std::vector<int> ruined;
            ruined.reserve(static_cast<std::size_t>(ruin_count));
            for (int list : order) {
                if (static_cast<int>(ruined.size()) >= ruin_count) break;
                const int size = Traits::list_size(solution, list);
                for (int p = 0; p < size; ++p) {
                    if (static_cast<int>(ruined.size()) >= ruin_count) break;
                    ruined.push_back(Traits::element_at(solution, list, p));
                }
            }
            if (ruined.empty()) return fallback_.select_ruin_set(ctx, ruin_count, rng);
            return ruined;
        }
    }

private:
    RandomRuin<Solution> fallback_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_RUIN_ROUTE_HPP
