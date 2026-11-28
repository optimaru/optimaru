#ifndef CL_CONSTRAINTS_COMMON_HPP
#define CL_CONSTRAINTS_COMMON_HPP

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cl/model/domain.hpp"
#include "cl/score/packing.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace cl {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The eight container-loading constraints, one small component each on the
// engine's DecomposedObjective. This assembly is also the default path: a
// solve without a selection scores with defaultConstraints() (there is no
// hand-written incremental calculator beside it; ScoreCalculator is the full
// recompute the assembly is checked against).
//
// Unit choice: with the sequence shape the calculator's unit is one list,
// i.e. one container; members are its items IN LOADING ORDER. Seven of the
// eight costs are sums over the containers and so UnitConstraints.
// groupSplitCost spans containers and is the one GlobalConstraint.
//
// Six of the unit constraints cost where the items stand, which is not in
// the solution: it is derived from the list by the decoder
// (cl/score/packing.hpp). They share one decode per (container, list) through
// PackingContext below, the engine's shared-context contract, so a move that
// touches a container decodes its candidate list once however many of them
// are in the assembly. The decode is a pure function of (problem facts,
// container, members), so each cost stays a pure function of (solution, unit,
// members) as the engine's contract requires; the context only remembers
// answers.
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md.

using DecomposedCalculator = optimaru::DecomposedObjective<ContainerLoadingProblem>;
using ConstraintAssembly = optimaru::ConstraintAssembly<ContainerLoadingProblem>;
using ClUnitConstraint = optimaru::UnitConstraint<ContainerLoadingProblem>;
using ClGlobalConstraint = optimaru::GlobalConstraint<ContainerLoadingProblem>;

// The decodes one calculator's constraints share (see PackingCache). The
// objective tells it when a list has changed; it then keeps that list's
// decode, which is the one the next candidates start from.
struct PackingContext final : optimaru::SharedConstraintContext {
    // Called by every constraint's on_reset(); the first call of a reset does
    // the work.
    void reset(const ContainerLoadingProblem& problem) {
        if (problem_ == &problem && seen_ == generation) return;
        problem_ = &problem;
        seen_ = generation;
        cache.reset(problem);
    }

    void on_list_changed(int list) override {
        if (problem_ == nullptr) return;
        cache.pin(list, problem_->containers[static_cast<std::size_t>(list)].items);
    }

    PackingCache cache;

private:
    // The solution the last reset was handed. The engine's shared-context
    // contract (SharedConstraintContext::on_list_changed) is that the lists
    // changed are those of the solution handed in on_reset(), and every
    // rebuild resets first, so this points at the solution being edited.
    // Only pinning reads it; a decode never does.
    const ContainerLoadingProblem* problem_ = nullptr;
    std::uint64_t seen_ = 0;
};

namespace detail {

// A unit constraint that costs where the items of a container stand.
struct PackedConstraint : ClUnitConstraint {
    using ClUnitConstraint::ClUnitConstraint;

    const char* shared_context_key() const final { return "cl.packing"; }
    std::unique_ptr<optimaru::SharedConstraintContext> create_shared_context() const final {
        return std::make_unique<PackingContext>();
    }
    void bind_shared_context(optimaru::SharedConstraintContext& context) final {
        context_ = static_cast<PackingContext*>(&context);
    }
    void on_reset(const ContainerLoadingProblem& problem) final { context_->reset(problem); }

protected:
    // The decode of `members` as container `unit`'s loading order.
    const Packing& packing(const ContainerLoadingProblem& problem, int unit,
                           const std::vector<int>& members) const {
        return context_->cache.packing(problem, unit, members);
    }

private:
    PackingContext* context_ = nullptr;
};

}  // namespace detail

}  // namespace cl

#endif  // CL_CONSTRAINTS_COMMON_HPP
