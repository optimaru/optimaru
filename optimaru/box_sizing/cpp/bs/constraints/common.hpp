#ifndef BS_CONSTRAINTS_COMMON_HPP
#define BS_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "bs/model/domain.hpp"
#include "bs/score/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace bs {

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The eight box-sizing constraints, one small component each on the engine's
// DecomposedObjective: an assembly path NEXT TO the monolithic
// BoxSizingIncrementalScoreCalculator, which stays the default fast path.
//
// Unit choice: the assignment shape's unit is the VALUE, i.e. one candidate
// box; members are the parcels that ship in it (order irrelevant). Fit,
// weight, void, box cost, board area and shipping cost are sums over a box's
// parcels, so they are UnitConstraints (one cost per candidate, 0 for a box
// no parcel takes). The two that count the boxes in use, boxTypeLimitCost and
// boxTypeCost, are GlobalConstraints: one number for the whole solution, and
// a required box counts without any member.
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. The monolithic calculators read the same
// kDefaultWeight constants, so the default assembly scores like
// ScoreCalculator::score.
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (the solve() overloads, the bindings and the monolithic path call it).

using DecomposedCalculator = optimaru::DecomposedObjective<BoxSizingProblem>;
using ConstraintAssembly = optimaru::ConstraintAssembly<BoxSizingProblem>;
using BsUnitConstraint = optimaru::UnitConstraint<BoxSizingProblem>;
using BsGlobalConstraint = optimaru::GlobalConstraint<BoxSizingProblem>;

namespace detail {

inline void requirePrepared(const BoxSizingProblem& problem) {
    if (!problem.prepared) {
        throw std::logic_error(
                "bs constraints require "
                "ScoreCalculator::prepare(problem) first");
    }
}

// A cost that is a sum over a box's parcels of one (parcel, box) term: the
// base of parcelFitCost, boxWeightCost, voidVolumeCost and shippingCost
// (boxCost and boardAreaCost add up the shipments and multiply once). A component writes term(),
// and cost() adds the terms up.
//
// No delta contract, on purpose. A sum of per-parcel terms could state what
// one parcel adds (supports_delta / delta_cost), and a change of one parcel
// would then cost one term instead of a pass over its box. But most of what
// this search does is moves on the set of sizes, which move hundreds of
// parcels in one batch: a constraint with cost() alone is asked once per
// touched box when the batch ends, and its saved costs are put back when the
// move is turned down, while a delta is asked for every parcel that moves,
// there and back. Measured on the assembly with 1,000 parcel sizes: 159
// thousand steps per second with cost() alone, 142 thousand at best with the
// delta (docs/use-cases/box-sizing/extending.md, step 4).
struct PerParcelConstraint : BsUnitConstraint {
    using BsUnitConstraint::BsUnitConstraint;

    virtual double term(const BoxSizingProblem& problem, int parcel, int box) const = 0;

    void on_reset(const BoxSizingProblem& problem) override { requirePrepared(problem); }
    double cost(const BoxSizingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        double total = 0.0;
        for (int parcel : members) total += term(problem, parcel, unit);
        return total;
    }
};

// The count of boxes in use, kept from joins and leaves: a box is in use
// while a parcel ships in it, and a required box always is. The base of the
// two globals, which differ in what they make of the count.
struct BoxTypeCount : BsGlobalConstraint {
    using BsGlobalConstraint::BsGlobalConstraint;

    void reset(const BoxSizingProblem& problem,
               const std::vector<std::vector<int>>& members_per_unit) override {
        requirePrepared(problem);
        limit_ = problem.maxBoxTypes ? *problem.maxBoxTypes : -1;
        used_.assign(members_per_unit.size(), 0);
        count_ = 0;
        for (std::size_t box = 0; box < members_per_unit.size(); ++box) {
            if (!members_per_unit[box].empty() || problem.boxes[box].required) {
                used_[box] = 1;
                ++count_;
            }
        }
    }
    void on_unit_changed(const BoxSizingProblem& problem, int unit,
                         const std::vector<int>& members) override {
        const char now = (!members.empty() || problem.boxes[unit].required) ? 1 : 0;
        count_ += now - used_[unit];
        used_[unit] = now;
    }
    bool supports_member_change() const override { return true; }
    void on_member_changed(const BoxSizingProblem& problem, int unit,
                           const std::vector<int>& members, int /*entity*/,
                           bool /*joined*/) override {
        on_unit_changed(problem, unit, members);  // `members` is after the change
    }
    // Nothing but cost() reads the count, and reset() rebuilds all of it.
    bool idle_at_weight_zero() const override { return true; }

protected:
    int count_ = 0;
    int limit_ = -1;  // maxBoxTypes; -1 = none

private:
    std::vector<char> used_;  // per box: in use
};

}  // namespace detail

}  // namespace bs

#endif  // BS_CONSTRAINTS_COMMON_HPP
