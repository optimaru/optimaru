#ifndef PJS_CONSTRAINTS_COMMON_HPP
#define PJS_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "pjs/domain.hpp"
#include "pjs/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace pjs
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The four constraints of the Timefold project-job-scheduling
// ProjectJobSchedulingConstraintProvider, split into one small component each
// on the engine's DecomposedScoreCalculator — an optional assembly path NEXT
// TO the monolithic ProjectJobSchedulingIncrementalScoreCalculator, which
// stays the tuned default fast path.
//
// Unit choice: none of the four rules decomposes per planning value, and that
// is the domain's defining property rather than an oversight. Every cost here
// reads the shadow chain predecessorsDoneDate -> startDate -> endDate, so
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
// Score levels: the quickstart scores with HardMediumSoftScore, which this
// domain folds into the engine's HardSoftScore as
// `soft = medium * kMediumUnit + soft` (see domain.hpp). The medium-level
// component (totalProjectDelay) therefore carries the kMediumUnit factor in
// its default weight.
//
//   DecomposedCalculator calc;
//   defaultConstraints()              // the whole default set
//       .weight("...", 2)            // ... then only what differs
//       .soft("...")
//       .drop("...")
//       .into(calc);
//   solver.solve(schedule, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// The level and the weight of each component are DEFAULTS — assembly-time
// metadata, not part of the cost. Override them per instance via the mutable
// `isHard` / `weight` / `exponent`.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all four
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.
//
// Every component requires ScoreCalculator::prepare(schedule) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator
    = optimaru::DecomposedScoreCalculator<ProjectJobSchedule>;
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
// into the solution's shadow fields, so a component can derive the chain from
// the planning values alone (the engine hands components a const solution,
// and on this path nothing else refreshes the shadow fields).
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
          const std::vector<std::vector<int>> & /*membersPerUnit*/) override
    {
        detail::requirePrepared(schedule);
        schedule_ = &schedule;
        dirty_ = true;
    }
    void onUnitChanged(const ProjectJobSchedule &schedule,
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
