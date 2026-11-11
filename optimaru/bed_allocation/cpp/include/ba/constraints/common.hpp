#ifndef BA_CONSTRAINTS_COMMON_HPP
#define BA_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "ba/domain.hpp"
#include "ba/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace ba {

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The twelve bed-allocation constraints, one small component each on the
// engine's DecomposedObjective — an optional assembly path NEXT TO the
// monolithic BedAllocationIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the assignment shape the calculator's unit is the VALUE,
// i.e. one assignable value — a real bed, or the "no bed" sentinel
// (BedPlan::noBedValue()); members are the stays currently assigned to it.
// Uninitialized stays (bed == -1) belong to no unit and so contribute
// nothing, exactly like in the monolithic path. The per-bed rules evaluate
// the real beds (detail::RealBeds), unassignedStaysCost the sentinel only;
// that scope is where a literal offset (clip = false) charges a raw 0.
//
// Ten of the twelve rules are a sum of independent per-(stay, value) terms —
// they derive from PerStayConstraint below and only write that one term.
// bedConflictCost is a sum over pairs INSIDE one bed, so it is a unit
// constraint with an exact pairwise delta. Only mixedGenderRoomCost spans
// units (its pairs share a ROOM, and a room holds several beds), so it runs
// as a GlobalConstraint on the on_member_changed fast path.
//
// Score levels: the domain scores on hard / medium / soft levels, which it
// folds into the engine's Score as
// `soft = medium * kMediumUnit + soft` (kMediumFold in domain.hpp, registered
// as optimaru::MediumFoldOf<BedPlan>). The decomposed calculator keeps a
// separate medium accumulator and applies that fold itself, so a medium
// component declares optimaru::Level::Medium and a weight in MEDIUM units —
// unassignedStaysCost (weight 1) is the only one. No weight carries the
// 10^9 factor.
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. The monolithic calculators read the same
// kDefaultWeight constants, so the default assembly scores bit-for-bit like
// ScoreCalculator::score.
//
// Every component requires ScoreCalculator::prepare(plan) to have run (the
// solve() overload and the monolithic path both call it).

using DecomposedCalculator = optimaru::DecomposedObjective<BedPlan>;
using ConstraintAssembly = optimaru::ConstraintAssembly<BedPlan>;
using BaUnitConstraint = optimaru::UnitConstraint<BedPlan>;

namespace detail {

inline void requirePrepared(const BedPlan& plan) {
    if (!plan.prepared) {
        throw std::logic_error(
                "ba constraints require "
                "ScoreCalculator::prepare(plan) first");
    }
}

// Is this value a real bed (rather than the "no bed" sentinel)?
inline bool isRealBed(const BedPlan& plan, int value) {
    return value >= 0 && value < plan.bedCount();
}

// The units a per-bed component EVALUATES: the real beds 0 .. bedCount - 1,
// set at reset (its scoped_units()). The "no bed" sentinel is no bed, so it is
// not one of them. Its cost is 0 either way, so no score changes; what the
// scope decides is where the literal offset formula (clip = false) charges
// shape_cost(0 - offset): every real bed, an empty one included, and never the
// sentinel. constraint_costs() reports the scope as "units", so the Python
// score() charges the same beds.
class RealBeds {
public:
    void reset(const BedPlan& plan) {
        beds_.resize(static_cast<std::size_t>(plan.bedCount()));
        std::iota(beds_.begin(), beds_.end(), 0);
    }
    const std::vector<int>* units() const { return &beds_; }

private:
    std::vector<int> beds_;
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Per-stay unit constraints
// ---------------------------------------------------------------------------
// Shared base of the ten rules whose cost is a sum of independent
// per-(stay, value) terms: the subclass writes `stayCost` only, and both the
// full re-cost and the exact O(1) delta follow from it.
struct PerStayConstraint : BaUnitConstraint {
    using BaUnitConstraint::BaUnitConstraint;

    // The penalty units stay `stayIndex` incurs while assigned to
    // value `unit`, in this constraint's own (unweighted) unit of account.
    virtual double stayCost(const BedPlan& plan, int unit, int stayIndex) const = 0;

    void on_reset(const BedPlan& plan) override { detail::requirePrepared(plan); }
    double cost(const BedPlan& plan, int unit, const std::vector<int>& members) const override {
        double total = 0.0;
        for (int stayIndex : members) total += stayCost(plan, unit, stayIndex);
        return total;
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const BedPlan& plan, int unit, const std::vector<int>& /*members*/,
                      int entity) const override {
        return stayCost(plan, unit, entity);
    }
};

// Shared base of the per-stay rules that read the bed's ROOM: the "no bed"
// sentinel has none, so they are silent there, and they evaluate the real
// beds only (detail::RealBeds).
struct PerStayRoomConstraint : PerStayConstraint {
    using PerStayConstraint::PerStayConstraint;

    void on_reset(const BedPlan& plan) override {
        PerStayConstraint::on_reset(plan);
        beds_.reset(plan);
    }
    const std::vector<int>* scoped_units() const override { return beds_.units(); }

    // The penalty units stay `stayIndex` incurs in `room` (already resolved
    // from the bed), never called for the sentinel.
    virtual double roomCost(const BedPlan& plan, int roomIndex, int stayIndex) const = 0;

    double stayCost(const BedPlan& plan, int unit, int stayIndex) const override {
        if (!detail::isRealBed(plan, unit)) return 0.0;
        return roomCost(plan, plan.beds[unit].room, stayIndex);
    }

private:
    detail::RealBeds beds_;
};

// ---------------------------------------------------------------------------
// Cross-unit global constraints
// ---------------------------------------------------------------------------
// Shared base: the aggregates are kept from joins and leaves alone.
using MemberChangeGlobalConstraint = optimaru::MemberChangeGlobalConstraint<BedPlan>;

}  // namespace ba

#endif  // BA_CONSTRAINTS_COMMON_HPP
