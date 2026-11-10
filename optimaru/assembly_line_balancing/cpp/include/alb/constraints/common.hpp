#ifndef ALB_CONSTRAINTS_COMMON_HPP
#define ALB_CONSTRAINTS_COMMON_HPP

#include <stdexcept>
#include <vector>

#include "alb/domain.hpp"
#include "alb/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace alb
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The ten assembly-line-balancing constraints, one small component each on the
// engine's DecomposedObjective — the assembly path NEXT TO the
// monolithic AssemblyLineBalancingIncrementalScoreCalculator, which stays the
// tuned default fast path.
//
// Unit choice: the BASIC descriptor's unit is the VALUE, i.e. one station;
// members are the tasks currently on it. Unassigned tasks (station == -1)
// belong to no unit and so contribute nothing (the engine convention).
//
//   - Per-station costs (stationCapacityCost, differentStationPairsCost) and
//     the four per-task terms (maxStationsCost, taskStationBoundsCost,
//     fixedTaskStationsCost, forbiddenTaskStationsCost, on PerTaskConstraint
//     below) are UnitConstraints: the exponent shapes each station's raw cost.
//   - The cross-station costs (taskPrecedenceCost, sameStationGroupsCost,
//     usedStationsCost, workloadImbalanceCost) are GlobalConstraints on the
//     on_member_changed fast path: the exponent shapes their single total.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                         // the whole default set
//       .weight("workloadImbalanceCost", 1)      // ... then only what differs
//       .soft("maxStationsCost")
//       .drop("fixedTaskStationsCost")
//       .into(calc);
//   solver.solve(problem, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from scratch.
//
// Each component declares its defaults once, as static constexpr
// kDefaultWeight / kDefaultLevel passed to the base constructor; they are
// assembly-time metadata, not part of the cost. Override them per instance
// with the constructor's weight argument or the mutable `level` / `weight` /
// `exponent`, or by name through ConstraintAssembly. The canonical problem
// carries no weights: the monolithic calculators read the same kDefaultWeight
// constants, which is why the default assembly scores bit-for-bit like them.
//
// Registry names are the struct names with a lower-case first letter
// ("stationCapacityCost"); Python sees them through python_name()
// ("station_capacity_cost").
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty, 0 = no violation; the calculator negates into the score.
//
// Every component requires ScoreCalculator::prepare(problem) to have run
// (the Solver, the Python calculate_score / constraint_costs and the
// monolithic path all call it).
//
// Reading the solution: a component may read another task's `station` from
// the solution. For BASIC solutions the calculator updates its member lists
// on every single variable change (also inside a batch), so the solution's
// planning variables and the member lists agree at every call.

using DecomposedCalculator
    = optimaru::DecomposedObjective<AssemblyLineBalancingProblem>;
using ConstraintAssembly
    = optimaru::ConstraintAssembly<AssemblyLineBalancingProblem>;
using AlbUnitConstraint
    = optimaru::UnitConstraint<AssemblyLineBalancingProblem>;
using AlbGlobalConstraint
    = optimaru::GlobalConstraint<AssemblyLineBalancingProblem>;

namespace detail
{

inline void requirePrepared(const AssemblyLineBalancingProblem &problem)
{
    if (!problem.prepared)
    {
        throw std::logic_error("alb constraints require "
                               "ScoreCalculator::prepare(problem) first");
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Per-task unit constraints
// ---------------------------------------------------------------------------
// Shared base of the four station-domain rules: their cost on a station is a
// sum of independent per-(task, station) terms, so the subclass writes
// `taskCost` only and both the full re-cost and the exact O(1) delta follow
// from it. The raw cost of a station is the sum over its tasks, so the
// exponent shapes that per-station sum (the pure-Python reference shapes
// each task's term; the two agree at exponent 1).
struct PerTaskConstraint : AlbUnitConstraint
{
    using AlbUnitConstraint::AlbUnitConstraint;

    // The raw cost task `task` incurs while it sits on `station`.
    virtual double taskCost(const AssemblyLineBalancingProblem &problem,
                            int station,
                            int task) const = 0;

    void on_reset(const AssemblyLineBalancingProblem &problem) override
    {
        detail::requirePrepared(problem);
    }
    double cost(const AssemblyLineBalancingProblem &problem,
                int unit,
                const std::vector<int> &members) const override
    {
        double total = 0.0;
        for (int task : members)
            total += taskCost(problem, unit, task);
        return total;
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const AssemblyLineBalancingProblem &problem,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return taskCost(problem, unit, entity);
    }
};

// ---------------------------------------------------------------------------
// Cross-station global constraints
// ---------------------------------------------------------------------------
// Shared base of the four globals: all of them run on the member-change fast
// path (every join / leave is delivered directly, also inside a batch), so
// on_unit_changed is unreachable.
struct MemberChangeGlobalConstraint : AlbGlobalConstraint
{
    using AlbGlobalConstraint::AlbGlobalConstraint;

    bool supports_member_change() const override { return true; }
    void on_unit_changed(const AssemblyLineBalancingProblem & /*problem*/,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        throw std::logic_error(name
                               + ": onUnitChanged is unreachable "
                                 "(member-change fast path)");
    }
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_COMMON_HPP
