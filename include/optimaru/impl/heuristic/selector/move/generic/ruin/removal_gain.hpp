#ifndef OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_REMOVAL_GAIN_HPP
#define OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_REMOVAL_GAIN_HPP

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace optimaru {

// Score after temporarily removing/unassigning one entity (higher = its
// removal helps more = "worse" placed). Shared by the ruin strategies that
// rank candidates; the solution is restored before returning.
template <typename Solution>
HardSoftScore removalGain(OperatorContext<Solution>& ctx, int e) {
    using Descriptor = SolutionDescriptor<Solution>;
    ScoreDirector<Solution>& director = ctx.director;
    if constexpr (Descriptor::kind == VariableKind::BASIC) {
        int old = Descriptor::getVariable(director.workingSolution(), e);
        director.changeVariable(e, -1);
        HardSoftScore gain = director.calculateScore();
        director.changeVariable(e, old);
        return gain;
    } else {
        int l = ctx.elementList[e], p = ctx.elementPosition[e];
        director.removeListElement(l, p);
        HardSoftScore gain = director.calculateScore();
        director.insertListElement(l, p, e);  // positions restored: index unchanged
        return gain;
    }
}

// How many entities (BASIC) or elements (LIST) the ruin may draw from.
template <typename Solution>
int ruinCandidateCount(OperatorContext<Solution>& ctx) {
    using Descriptor = SolutionDescriptor<Solution>;
    if constexpr (Descriptor::kind == VariableKind::BASIC) {
        return Descriptor::entityCount(ctx.director.workingSolution());
    } else {
        return Descriptor::elementCount(ctx.director.workingSolution());
    }
}

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_HEURISTIC_SELECTOR_MOVE_GENERIC_RUIN_REMOVAL_GAIN_HPP
