#ifndef FP_CONSTRAINTS_COMMON_HPP
#define FP_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "fp/domain.hpp"
#include "fp/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace fp {

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The five food-packaging constraints, one small component each on the
// engine's DecomposedObjective — an optional assembly path NEXT TO the
// monolithic FoodPackagingIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the sequence shape the calculator's unit is one list,
// i.e. one packaging line; members are its jobs IN PRODUCTION ORDER. Three
// rules are per-line sums over that ordered chain and run as UnitConstraints
// over the lines (PerLineConstraint: the unassigned pool list is no line, so
// it is not one of their units).
// The other two belong to no single line: operatorCleaningConflictCost pairs
// jobs whose LINES share an operator (two different lines, generally), and
// unassignedJobsCost counts the jobs in no list at all — both are
// GlobalConstraints.
//
// A component never reads the Job derived fields (lineIndex,
// startCleaning/startProduction/endDateTime): LineChain below re-derives the
// cleaning-and-production chain from a unit's own member order, which is what
// makes each cost a pure function of (solution, unit, members) as the
// engine's contract requires. The sequence path re-costs a touched list whole, so
// no delta contract is needed.
//
// Score levels: the score has three levels (hard / medium / soft), which this
// domain folds into the engine's Score as
// `soft = medium * kMediumUnit + soft` (kMediumUnit = 10^10; kMediumFold and
// the MediumFoldOf<PackagingSchedule> specialization in domain.hpp). The two
// medium components (afterIdealEndTimeCost, unassignedJobsCost) declare
// Level::Medium at weight 1, in medium units; the decomposed calculator keeps
// a separate medium accumulator and applies the fold, so no component carries
// the 10^10 factor in its weight.
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. The monolithic calculators read the same
// kDefaultWeight constants, so the default assembly scores bit-for-bit like
// ScoreCalculator::score.
//
// Every component requires ScoreCalculator::prepare(schedule) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator = optimaru::DecomposedObjective<PackagingSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<PackagingSchedule>;
using FpUnitConstraint = optimaru::UnitConstraint<PackagingSchedule>;
using FpGlobalConstraint = optimaru::GlobalConstraint<PackagingSchedule>;

namespace detail {

inline void requirePrepared(const PackagingSchedule& schedule) {
    if (schedule.pinnedPrefixLen.size() != static_cast<std::size_t>(schedule.listCountWithPool())) {
        throw std::logic_error(
                "fp constraints require "
                "ScoreCalculator::prepare(schedule) first");
    }
}

// Is this list the unassigned pool rather than a real packaging line?
inline bool isPool(const PackagingSchedule& schedule, int unit) {
    return unit >= schedule.lineCount();
}

// The base of the per-line components: a unit constraint that EVALUATES the
// packaging lines and not the unassigned pool list (scoped_units() = 0 ..
// lines - 1, set at reset). The pool's cost is 0 either way, so no score
// changes; what the scope decides is where the literal offset formula
// (clip = false) charges shape_cost(0 - offset): every line, an empty one
// included, and never the pool, which is no line. constraint_costs() reports
// the scope as "units", so the Python score() charges the same lines.
struct PerLineConstraint : optimaru::UnitConstraint<PackagingSchedule> {
    using optimaru::UnitConstraint<PackagingSchedule>::UnitConstraint;

    void on_reset(const PackagingSchedule& schedule) override {
        requirePrepared(schedule);
        lines_.resize(static_cast<std::size_t>(schedule.lineCount()));
        std::iota(lines_.begin(), lines_.end(), 0);
    }
    const std::vector<int>* scoped_units() const override { return &lines_; }

private:
    std::vector<int> lines_;
};

// The unit's jobs in production order. The engine hides each list's pinned
// prefix from a calculator (`members` is only the movable suffix, in engine
// element ids), but the schedule's own list is the full ordered truth — and
// it is exactly what the monolithic calculator scores.
inline const std::vector<int>& jobsOfUnit(const PackagingSchedule& schedule, int unit) {
    return schedule.rawList(unit);
}

// The job times of one line, derived from its member order:
//   first job:  startCleaning = startProduction = line.startDateTime
//   later job:  startCleaning = previous end
//               startProduction = startCleaning + cleanup(previous, this)
//   every job:  end = startProduction + duration
// Calls onJob(jobIndex, startCleaning, startProduction, end) in order and
// returns the end time of the last job (kNoTime for an empty line).
template <typename F>
inline Minutes walkChain(const PackagingSchedule& schedule, int lineIndex,
                         const std::vector<int>& members, F&& onJob) {
    const Line& line = schedule.lines[lineIndex];  // never the pool
    Minutes previousEnd = kNoTime;
    int previousProduct = -1;
    for (int jobIndex : members) {
        const Job& job = schedule.jobs[jobIndex];
        Minutes startCleaning = 0;
        Minutes startProduction = 0;
        if (previousProduct < 0) {
            startCleaning = line.startDateTime;
            startProduction = line.startDateTime;
        } else {
            startCleaning = previousEnd;
            startProduction = startCleaning + schedule.cleanup(previousProduct, job.productIndex);
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
