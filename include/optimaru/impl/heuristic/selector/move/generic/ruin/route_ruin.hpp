#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_ROUTE_RUIN_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_ROUTE_RUIN_HPP

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin/random_ruin.hpp"
#include "optimaru/preview/api/move/operators.hpp"

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
// LIST kind only; delegates to the random ruin for BASIC solutions.
template <typename Solution>
class RouteRuin final : public RuinOperator<Solution> {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    const char* name() const override { return "routeRuin"; }

    std::vector<int> selectRuinSet(OperatorContext<Solution>& ctx, int ruinCount,
                                   std::mt19937_64& rng) override {
        if constexpr (Descriptor::kind != VariableKind::LIST) {
            return fallback_.selectRuinSet(ctx, ruinCount, rng);
        } else {
            const Solution& solution = ctx.director.workingSolution();
            const int listCount = Descriptor::listCount(solution);
            if (listCount == 0) return {};

            // Visit the lists in a random order and drain them until the ruin
            // budget is spent; a partially drained last list is fine (the
            // budget is what keeps the kick recoverable).
            std::vector<int> order(static_cast<std::size_t>(listCount));
            for (int l = 0; l < listCount; ++l) order[static_cast<std::size_t>(l)] = l;
            std::shuffle(order.begin(), order.end(), rng);

            std::vector<int> ruined;
            ruined.reserve(static_cast<std::size_t>(ruinCount));
            for (int list : order) {
                if (static_cast<int>(ruined.size()) >= ruinCount) break;
                const int size = Descriptor::listSize(solution, list);
                for (int p = 0; p < size; ++p) {
                    if (static_cast<int>(ruined.size()) >= ruinCount) break;
                    ruined.push_back(Descriptor::getElement(solution, list, p));
                }
            }
            if (ruined.empty()) return fallback_.selectRuinSet(ctx, ruinCount, rng);
            return ruined;
        }
    }

private:
    RandomRuin<Solution> fallback_;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_ROUTE_RUIN_HPP
