#ifndef OPTIMARU_IMPL_LOCALSEARCH_MOVABLE_ENTITY_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_MOVABLE_ENTITY_HPP

#include <random>

#include "optimaru/api/domain/solution_descriptor.hpp"

namespace optimaru {

// Return a non-pinned entity: keep `e` if movable, otherwise resample (then
// scan) for one. Falls back to `e` only in the degenerate all-pinned case.
template <typename Solution>
int movableEntity(const Solution& sol, int e, std::uniform_int_distribution<int>& pick,
                  std::mt19937_64& rng) {
    using Descriptor = SolutionDescriptor<Solution>;
    if (!entityPinned<Descriptor>(sol, e)) return e;
    for (int t = 0; t < 8; ++t) {
        const int c = pick(rng);
        if (!entityPinned<Descriptor>(sol, c)) return c;
    }
    const int n = Descriptor::entityCount(sol);
    for (int c = 0; c < n; ++c)
        if (!entityPinned<Descriptor>(sol, c)) return c;
    return e;
}

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_MOVABLE_ENTITY_HPP
