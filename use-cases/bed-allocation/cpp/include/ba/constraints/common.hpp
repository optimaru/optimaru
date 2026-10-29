#ifndef BA_CONSTRAINTS_COMMON_HPP
#define BA_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ba/domain.hpp"
#include "ba/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace ba
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The twelve constraints of the reference bed-allocation
// BedAllocationConstraintProvider, split into one small component each on the
// engine's DecomposedScoreCalculator — an optional assembly path NEXT TO the
// monolithic BedAllocationIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the BASIC descriptor the calculator's unit is the VALUE,
// i.e. one planning value — a real bed, or the "no bed" sentinel
// (BedPlan::noBedValue()); members are the stays currently assigned to it.
// Uninitialized stays (bed == -1) belong to no unit and so contribute
// nothing, exactly like in the monolithic path.
//
// Ten of the twelve rules are a sum of independent per-(stay, value) terms —
// they derive from PerStayConstraint below and only write that one term.
// sameBedInSameNight is a sum over pairs INSIDE one bed, so it is a unit
// constraint with an exact pairwise delta. Only
// differentGenderInSameGenderRoomInSameNight spans units (its pairs share a
// ROOM, and a room holds several beds), so it runs as a GlobalConstraint on
// the onMemberChanged fast path.
//
// Score levels: the quickstart scores with HardMediumSoftScore, which this
// domain folds into the engine's HardSoftScore as
// `soft = medium * kMediumUnit + soft` (see domain.hpp). A medium-level
// component is therefore a soft component whose weight carries the
// kMediumUnit factor — assignEveryPatientToABed is the only one.
//
//   DecomposedCalculator calc;
//   defaultConstraints()              // the whole default set
//       .weight("...", 2)            // ... then only what differs
//       .soft("...")
//       .drop("...")
//       .into(calc);
//   solver.solve(plan, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// The "Hard:"/"Medium:"/"Soft:" level and the weight of each component are
// DEFAULTS — assembly-time metadata, not part of the cost. Override them per
// instance via the mutable `isHard` / `weight` / `exponent`.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all twelve
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.
//
// Every component requires ScoreCalculator::prepare(plan) to have run (the
// solve() overload and the monolithic path both call it).

using DecomposedCalculator = optimaru::DecomposedScoreCalculator<BedPlan>;
using ConstraintAssembly = optimaru::ConstraintAssembly<BedPlan>;
using BaUnitConstraint = optimaru::UnitConstraint<BedPlan>;
using BaGlobalConstraint = optimaru::GlobalConstraint<BedPlan>;

namespace detail
{

inline void requirePrepared(const BedPlan &plan)
{
    if (!plan.prepared)
    {
        throw std::logic_error("ba constraints require "
                               "ScoreCalculator::prepare(plan) first");
    }
}

// Is this planning value a real bed (rather than the "no bed" sentinel)?
inline bool isRealBed(const BedPlan &plan, int value)
{
    return value >= 0 && value < plan.bedCount();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Per-stay unit constraints
// ---------------------------------------------------------------------------
// Shared base of the ten rules whose cost is a sum of independent
// per-(stay, value) terms: the subclass writes `stayCost` only, and both the
// full re-cost and the exact O(1) delta follow from it.
struct PerStayConstraint : BaUnitConstraint
{
    using BaUnitConstraint::BaUnitConstraint;

    // The penalty units stay `stayIndex` incurs while assigned to planning
    // value `unit`, in this constraint's own (unweighted) unit of account.
    virtual double stayCost(const BedPlan &plan, int unit, int stayIndex) const
        = 0;

    void onReset(const BedPlan &plan) override
    {
        detail::requirePrepared(plan);
    }
    double cost(const BedPlan &plan,
                int unit,
                const std::vector<int> &members) const override
    {
        double total = 0.0;
        for (int stayIndex : members)
            total += stayCost(plan, unit, stayIndex);
        return total;
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const BedPlan &plan,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return stayCost(plan, unit, entity);
    }
};

// Shared base of the per-stay rules that read the bed's ROOM: the "no bed"
// sentinel has none, so they are silent there.
struct PerStayRoomConstraint : PerStayConstraint
{
    using PerStayConstraint::PerStayConstraint;

    // The penalty units stay `stayIndex` incurs in `room` (already resolved
    // from the bed), never called for the sentinel.
    virtual double
    roomCost(const BedPlan &plan, int roomIndex, int stayIndex) const = 0;

    double stayCost(const BedPlan &plan, int unit, int stayIndex) const override
    {
        if (!detail::isRealBed(plan, unit))
            return 0.0;
        return roomCost(plan, plan.beds[unit].room, stayIndex);
    }
};

// ---------------------------------------------------------------------------
// Cross-unit global constraints
// ---------------------------------------------------------------------------
// Shared base: supportsMemberChange() routes everything through
// onMemberChanged; the unit-diff hook is unreachable for BASIC solutions.
struct MemberChangeGlobalConstraint : BaGlobalConstraint
{
    using BaGlobalConstraint::BaGlobalConstraint;
    bool supportsMemberChange() const override { return true; }
    void onUnitChanged(const BedPlan & /*plan*/,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        throw std::logic_error(name
                               + ": onUnitChanged is unreachable "
                                 "(member-change fast path)");
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_COMMON_HPP
