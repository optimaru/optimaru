#ifndef PJS_CONSTRAINTS_COMMON_HPP
#define PJS_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "pjs/domain.hpp"
#include "pjs/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace pjs
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The four project-job-scheduling constraints, one small component each
// on the engine's DecomposedObjective — an optional assembly path NEXT
// TO the monolithic ProjectJobSchedulingIncrementalScoreCalculator, which
// stays the tuned default fast path.
//
// Unit choice: none of the four rules decomposes per assigned value, and that
// is the domain's defining property rather than an oversight. Every cost here
// reads the derived dates predecessorsDoneDate -> startDate -> endDate, so
// changing ONE allocation's (execution mode, delay) shifts the dates of its
// whole successor subgraph — and with them the renewable resource profile,
// the project delays and the makespan. A cost that spans the precedence graph
// belongs to no unit, so all four components are GlobalConstraints over the
// ScheduleAggregateConstraint base below.
//
// That base trades the monolithic calculator's topological delta propagation
// for something much simpler: any notification marks the component dirty, and
// the aggregate (with the date chain it needs) is recomputed once, lazily, in
// cost(). Since cost() runs once per score, a move costs O(allocations) per
// component instead of O(affected subgraph) — slower than the tuned path, and
// exact by construction rather than by a propagation argument. That is the
// trade this assembly path is for: composability, not peak throughput.
//
// Score levels: the domain scores on hard / medium / soft levels, which it
// folds into the engine's Score as
// `soft = medium * kMediumUnit + soft` (kMediumUnit = 10^9; kMediumFold and
// the MediumFoldOf<ProjectJobSchedule> specialization in domain.hpp). The
// medium component (projectDelayCost) declares Level::Medium at weight 1, in
// medium units; the decomposed calculator keeps a separate medium
// accumulator and applies the fold, so no component carries the 10^9 factor
// in its weight.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                    // the whole default set ...
//       .weight("nonRenewableResourceCapacityCost", 2)  // ... then only
//       .soft("renewableResourceCapacityCost")          //     what differs
//       .drop("makespanCost")
//       .into(calc);
//   solver.solve(schedule, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// Each component declares its defaults once, as static constexpr
// kDefaultWeight / kDefaultLevel passed to the base constructor; they are
// assembly-time metadata, not part of the cost. Override them per instance
// with the constructor's weight argument or the mutable `level` / `weight` /
// `exponent` / `offset` / `clip`, or by name through ConstraintAssembly
// (weight / level / exponent / offset / clip; hard() / soft() are
// level(Hard) / level(Soft)). A name may be held several times
// (ConstraintAssembly::add_instances): each instance has its own knobs, its
// level included, and the instances add up. Any component may take any of
// the three levels: the medium component moved to hard / soft costs 1 hard /
// 1 soft unit per raw unit, a hard or soft one moved to medium 1 medium unit,
// and a weight is in units of the level in effect. The monolithic calculators
// read the same kDefaultWeight constants, which is why the default assembly
// scores bit-for-bit like them.
//
// Registry names are the struct names with a lower-case first letter
// ("makespanCost"); Python sees them through python_name() ("makespan_cost").
//
// Cost sign convention (see score/decomposed_objective.hpp): positive =
// penalty, 0 = no violation; the calculator negates into the score. An
// assembly of all four components at their default weights therefore scores
// bit-for-bit like ScoreCalculator::score.
//
// Every component requires ScoreCalculator::prepare(schedule) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator
    = optimaru::DecomposedObjective<ProjectJobSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<ProjectJobSchedule>;
using PjsUnitConstraint = optimaru::UnitConstraint<ProjectJobSchedule>;
using PjsGlobalConstraint = optimaru::GlobalConstraint<ProjectJobSchedule>;

namespace detail
{

inline void requirePrepared(const ProjectJobSchedule &schedule)
{
    if (!schedule.prepared)
    {
        throw std::logic_error("pjs constraints require "
                               "ScoreCalculator::prepare(schedule) first");
    }
}

// ScoreCalculator::updateDates written into caller-owned vectors instead of
// into the solution's derived fields, so a component can derive the chain from
// the assigned values alone (the engine hands components a const solution,
// and on this path nothing else refreshes the derived fields).
inline void computeDates(const ProjectJobSchedule &schedule,
                         std::vector<int> &startDate,
                         std::vector<int> &endDate)
{
    const int count = schedule.allocationCount();
    startDate.assign(count, 0);
    endDate.assign(count, 0);
    for (int a : schedule.topologicalOrder)
    {
        const Allocation &allocation = schedule.allocations[a];
        int doneDate = 0;
        for (int p : allocation.predecessorAllocations)
            doneDate = std::max(doneDate, endDate[p]);
        const int value = allocation.modeDelay;
        const int delay = value < 0 ? 0 : schedule.delayOfValue(value);
        const int duration
            = value < 0 ? 0 : schedule.executionModeOf(a, value).duration;
        startDate[a] = doneDate + delay;
        endDate[a] = startDate[a] + duration;
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Whole-schedule global constraints
// ---------------------------------------------------------------------------
// Shared base of all four rules: the subclass writes `evaluate` against the
// derived date chain, and this base takes care of invalidation and of
// recomputing the chain at most once per score.
struct ScheduleAggregateConstraint : PjsGlobalConstraint
{
    using PjsGlobalConstraint::PjsGlobalConstraint;

    // The component's penalty units over the whole schedule. `startDate` /
    // `endDate` are the derived chain, empty when needsDates() is false.
    virtual double evaluate(const ProjectJobSchedule &schedule,
                            const std::vector<int> &startDate,
                            const std::vector<int> &endDate) const = 0;

    // Rules that read only the assigned execution modes (not when they run)
    // opt out of the date chain and skip that pass entirely.
    virtual bool needsDates() const { return true; }

    void
    reset(const ProjectJobSchedule &schedule,
          const std::vector<std::vector<int>> & /*members_per_unit*/) override
    {
        detail::requirePrepared(schedule);
        schedule_ = &schedule;
        dirty_ = true;
    }
    void on_unit_changed(const ProjectJobSchedule &schedule,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        schedule_ = &schedule;
        dirty_ = true;
    }
    double cost() const override
    {
        if (dirty_)
        {
            if (needsDates())
                detail::computeDates(*schedule_, startDate_, endDate_);
            value_ = evaluate(*schedule_, startDate_, endDate_);
            dirty_ = false;
        }
        return value_;
    }

private:
    const ProjectJobSchedule *schedule_ = nullptr;
    mutable std::vector<int> startDate_;
    mutable std::vector<int> endDate_;
    mutable double value_ = 0.0;
    mutable bool dirty_ = true;
};

}  // namespace pjs

#endif  // PJS_CONSTRAINTS_COMMON_HPP
