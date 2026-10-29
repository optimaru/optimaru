#ifndef MNT_CONSTRAINTS_COMMON_HPP
#define MNT_CONSTRAINTS_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "mnt/domain.hpp"
#include "mnt/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace mnt
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The six constraints of the reference maintenance-scheduling
// MaintenanceScheduleConstraintProvider, split into one small component each
// on the engine's DecomposedScoreCalculator — an optional assembly path NEXT
// TO the monolithic MaintenanceSchedulingIncrementalScoreCalculator, which
// stays the tuned default fast path.
//
// Unit choice: with the BASIC descriptor the calculator's unit is the VALUE,
// i.e. one composite crewDate — a (start date, crew) cell of the grid;
// members are the jobs placed there. Unassigned jobs (crewDate == -1) belong
// to no unit and so contribute nothing.
//
// The four date rules are per-job terms of that cell and derive from
// PerJobConstraint below. The two pairwise rules (crewConflict, tagConflict)
// compare jobs that share a crew or a tag on some DAY, which spans cells —
// two jobs of one crew starting on different dates sit in different units —
// so they run as GlobalConstraints on the onMemberChanged fast path, keeping
// the same per-(crew, day) / per-(tag, day) occupancy counters the monolithic
// calculator does.
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
// The "Hard:"/"Soft:" level and the weight of each component are DEFAULTS —
// assembly-time metadata, not part of the cost. Override them per instance
// via the mutable `isHard` / `weight` / `exponent`. The Java soft weights
// (x 1 beforeIdeal, x 1'000 tagConflict, x 1'000'000 afterIdeal) are exactly
// these defaults, so re-pricing a term is a one-argument change.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all six
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.
//
// Every component requires ScoreCalculator::prepare(schedule) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator
    = optimaru::DecomposedScoreCalculator<MaintenanceSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<MaintenanceSchedule>;
using MntUnitConstraint = optimaru::UnitConstraint<MaintenanceSchedule>;
using MntGlobalConstraint = optimaru::GlobalConstraint<MaintenanceSchedule>;

namespace detail
{

inline void requirePrepared(const MaintenanceSchedule &schedule)
{
    if (!schedule.prepared)
    {
        throw std::logic_error("mnt constraints require "
                               "ScoreCalculator::prepare(schedule) first");
    }
}

// The [start, end) epoch-day interval a job occupies when placed in `unit`
// (a composite crewDate). end is exclusive, exactly Java's shadow endDate.
inline void occupancyOf(const MaintenanceSchedule &schedule,
                        int unit,
                        int jobIndex,
                        std::int64_t &startDate,
                        std::int64_t &endDate)
{
    const int dateIndex = schedule.dateIndexOf(unit);
    startDate = schedule.startDates[dateIndex];
    endDate = schedule.endDateFor(schedule.jobs[jobIndex].durationInDays,
                                  dateIndex);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Per-job unit constraints
// ---------------------------------------------------------------------------
// Shared base of the four date rules: their cost is a sum of independent
// per-(job, cell) terms, so the subclass writes `jobCost` only and both the
// full re-cost and the exact O(1) delta follow from it.
struct PerJobConstraint : MntUnitConstraint
{
    using MntUnitConstraint::MntUnitConstraint;

    // The penalty units (days) job `jobIndex` incurs while placed in `unit`.
    virtual double jobCost(const MaintenanceSchedule &schedule,
                           int unit,
                           int jobIndex) const = 0;

    void onReset(const MaintenanceSchedule &schedule) override
    {
        detail::requirePrepared(schedule);
    }
    double cost(const MaintenanceSchedule &schedule,
                int unit,
                const std::vector<int> &members) const override
    {
        double total = 0.0;
        for (int jobIndex : members)
            total += jobCost(schedule, unit, jobIndex);
        return total;
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const MaintenanceSchedule &schedule,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return jobCost(schedule, unit, entity);
    }
};

// ---------------------------------------------------------------------------
// Cross-unit global constraints
// ---------------------------------------------------------------------------
// Shared base of the two pairwise rules: a job joining or leaving a cell adds
// or removes, per day it covers, exactly as many overlapping pairs as there
// already are occupants of that (owner, day) — the telescoping identity the
// monolithic calculator uses. `ownerCount` and the owners of one job are what
// the subclass supplies.
struct DayOverlapGlobalConstraint : MntGlobalConstraint
{
    using MntGlobalConstraint::MntGlobalConstraint;

    // How many owner rows the occupancy table has (crews, or tags).
    virtual int ownerCount(const MaintenanceSchedule &schedule) const = 0;
    // The owner rows job `jobIndex` occupies while placed in `unit`, appended
    // to `owners` (cleared by the caller).
    virtual void ownersOf(const MaintenanceSchedule &schedule,
                          int unit,
                          int jobIndex,
                          std::vector<int> &owners) const = 0;

    bool supportsMemberChange() const override { return true; }
    void onUnitChanged(const MaintenanceSchedule & /*schedule*/,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        throw std::logic_error(name
                               + ": onUnitChanged is unreachable "
                                 "(member-change fast path)");
    }

    void reset(const MaintenanceSchedule &schedule,
               const std::vector<std::vector<int>> &membersPerUnit) override
    {
        detail::requirePrepared(schedule);
        dayCount_ = schedule.dayCount;
        occupancy_.assign(
            static_cast<std::size_t>(ownerCount(schedule)) * dayCount_, 0);
        overlap_ = 0;
        for (std::size_t unit = 0; unit < membersPerUnit.size(); ++unit)
        {
            for (int jobIndex : membersPerUnit[unit])
                apply(schedule, static_cast<int>(unit), jobIndex, true);
        }
    }

    void onMemberChanged(const MaintenanceSchedule &schedule,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        apply(schedule, unit, entity, joined);
    }

    double cost() const override { return static_cast<double>(overlap_); }

private:
    // On a join the pair count grows by the occupants already there and only
    // then is the cell incremented; on a leave the cell is decremented first
    // — the mirror image, so the aggregate is exactly reversible.
    void apply(const MaintenanceSchedule &schedule,
               int unit,
               int jobIndex,
               bool joined)
    {
        owners_.clear();
        ownersOf(schedule, unit, jobIndex, owners_);
        if (owners_.empty())
            return;
        std::int64_t startDate = 0;
        std::int64_t endDate = 0;
        detail::occupancyOf(schedule, unit, jobIndex, startDate, endDate);
        for (std::int64_t day = startDate; day < endDate; ++day)
        {
            const std::size_t offset
                = static_cast<std::size_t>(day - schedule.dayBase);
            for (int owner : owners_)
            {
                std::int64_t &count
                    = occupancy_[static_cast<std::size_t>(owner) * dayCount_
                                 + offset];
                if (joined)
                {
                    overlap_ += count;
                    ++count;
                }
                else
                {
                    --count;
                    overlap_ -= count;
                }
            }
        }
    }

    int dayCount_ = 0;
    std::vector<std::int64_t> occupancy_;  // (owner, day) -> jobs covering it
    std::int64_t overlap_ = 0;             // summed pairwise overlap in days
    std::vector<int> owners_;              // scratch, reused per apply
};

}  // namespace mnt

#endif  // MNT_CONSTRAINTS_COMMON_HPP
