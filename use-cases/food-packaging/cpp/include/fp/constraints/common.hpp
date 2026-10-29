#ifndef FP_CONSTRAINTS_COMMON_HPP
#define FP_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "fp/domain.hpp"
#include "fp/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace fp
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The five constraints of the reference food-packaging
// FoodPackagingConstraintProvider, split into one small component each on the
// engine's DecomposedScoreCalculator — an optional assembly path NEXT TO the
// monolithic FoodPackagingIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the LIST descriptor the calculator's unit is one LIST,
// i.e. one packaging line; members are its jobs IN PRODUCTION ORDER. Three
// rules are per-line sums over that ordered chain and run as UnitConstraints.
// The other two belong to no single line: operatorCleaningConflict pairs jobs
// whose LINES share an operator (two different lines, generally), and
// maximizeJobsAssigned counts the jobs in no list at all — both are
// GlobalConstraints.
//
// A component never reads the Job shadow variables (lineIndex,
// startCleaning/startProduction/endDateTime): LineChain below re-derives the
// cleaning-and-production chain from a unit's own member order, which is what
// makes each cost a pure function of (solution, unit, members) as the
// engine's contract requires. The LIST path re-costs a touched list whole, so
// no delta contract is needed.
//
// Score levels: the quickstart scores with HardMediumSoftScore, which this
// domain folds into the engine's HardSoftScore as
// `soft = medium * kMediumUnit + soft` (see domain.hpp). The two
// medium-level components therefore carry the kMediumUnit factor in their
// default weight.
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
// penalty; the calculator negates into the score. An assembly of all five
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.
//
// Every component requires ScoreCalculator::prepare(schedule) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator
    = optimaru::DecomposedScoreCalculator<PackagingSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<PackagingSchedule>;
using FpUnitConstraint = optimaru::UnitConstraint<PackagingSchedule>;
using FpGlobalConstraint = optimaru::GlobalConstraint<PackagingSchedule>;

namespace detail
{

inline void requirePrepared(const PackagingSchedule &schedule)
{
    if (schedule.pinnedPrefixLen.size()
        != static_cast<std::size_t>(schedule.listCountWithPool()))
    {
        throw std::logic_error("fp constraints require "
                               "ScoreCalculator::prepare(schedule) first");
    }
}

// Is this list the unassigned pool rather than a real packaging line?
inline bool isPool(const PackagingSchedule &schedule, int unit)
{
    return unit >= schedule.lineCount();
}

// The unit's jobs in production order. The engine hides each list's pinned
// prefix from a calculator (`members` is only the movable suffix, in engine
// element ids), but the schedule's own list is the full ordered truth — and
// it is exactly what the monolithic calculator scores.
inline const std::vector<int> &jobsOfUnit(const PackagingSchedule &schedule,
                                          int unit)
{
    return schedule.rawList(unit);
}

// The Java shadow chain of one line, re-derived from its member order:
//   first job:  startCleaning = startProduction = line.startDateTime
//   later job:  startCleaning = previous end
//               startProduction = startCleaning + cleanup(previous, this)
//   every job:  end = startProduction + duration
// Calls onJob(jobIndex, startCleaning, startProduction, end) in order and
// returns the end time of the last job (kNoTime for an empty line).
template <typename F>
inline Minutes walkChain(const PackagingSchedule &schedule,
                         int lineIndex,
                         const std::vector<int> &members,
                         F &&onJob)
{
    const Line &line = schedule.lines[lineIndex];  // never the pool
    Minutes previousEnd = kNoTime;
    int previousProduct = -1;
    for (int jobIndex : members)
    {
        const Job &job = schedule.jobs[jobIndex];
        Minutes startCleaning = 0;
        Minutes startProduction = 0;
        if (previousProduct < 0)
        {
            startCleaning = line.startDateTime;
            startProduction = line.startDateTime;
        }
        else
        {
            startCleaning = previousEnd;
            startProduction
                = startCleaning
                  + schedule.cleanup(previousProduct, job.productIndex);
        }
        previousEnd = startProduction + job.durationMinutes;
        previousProduct = job.productIndex;
        onJob(jobIndex, startCleaning, startProduction, previousEnd);
    }
    return previousEnd;
}

}  // namespace detail

}  // namespace fp

#endif  // FP_CONSTRAINTS_COMMON_HPP
