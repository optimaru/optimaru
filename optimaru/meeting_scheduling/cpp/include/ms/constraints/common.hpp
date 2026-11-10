#ifndef MS_CONSTRAINTS_COMMON_HPP
#define MS_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ms/domain.hpp"
#include "ms/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace ms
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The twelve meeting-scheduling constraints, one small component each on
// the engine's DecomposedObjective — an optional assembly path NEXT TO
// the monolithic MeetingSchedulingIncrementalScoreCalculator, which stays the
// tuned default fast path.
//
// Unit choice: with the assignment shape the calculator's unit is the VALUE,
// i.e. one composite grainRoom — a (starting time grain, room) cell; members
// are the meeting assignments placed there. Unassigned assignments
// (grainRoom == -1) belong to no unit and contribute nothing.
//
// Five rules judge one assignment against its own cell and derive from
// PerAssignmentConstraint below, with an exact O(1) delta. The other seven
// compare assignments that are generally in DIFFERENT cells — meetings
// overlapping in time, sharing a room, running back to back, or sharing an
// attendee — so they run as GlobalConstraints over AssignmentPairConstraint,
// which tracks where each assignment currently sits and folds the marginal of
// one placement against all the others.
//
// The three attendance-conflict rules fold their same-meeting "self" term
// (a meeting whose attendee list lists someone twice conflicts with itself)
// into the same marginal, so each rule stays exactly one component.
//
// Names and defaults: every component is one `<Subject>Cost` struct in
// constraints/<subject>_cost.hpp whose registry name is the struct name with a
// lower-case first letter ("roomConflictCost"; Python sees it as
// room_conflict_cost). Its default level and weight are declared ONCE, as
// `kDefaultLevel` / `kDefaultWeight`; the constructor takes the weight first
// and defaults it to kDefaultWeight. constraint_info() and the CP-SAT
// reference read them from there, and the monolithic calculators read the
// same constants (score_calculator.cpp / incremental_calculator.cpp).
//
// Score levels: hard, medium and soft. This domain
// keeps the engine's Score and declares how its medium level folds
// into soft ONCE, in domain.hpp (kMediumFold = into_soft(kMediumScale = 1e6),
// registered through optimaru::MediumFoldOf<MeetingSchedule>). The two medium
// components are `Level::Medium` at weight 1 in medium units, the decomposed
// calculator keeps a separate medium accumulator and folds it with that
// constant; the soft ones carry the x 1 / x 100 / x 10 factors.
//
//   DecomposedCalculator calc;
//   defaultConstraints()                          // the whole default set
//       .weight("meetingOverlapCost", 2)          // ... then only what differs
//       .soft("roomCapacityCost")
//       .drop("overtimeCost")
//       .into(calc);
//   solver.solve(schedule, calc);
//
// Or `addDefaultConstraints(calc)` when nothing differs, and
// `ConstraintAssembly{}` when you would rather list components from
// scratch than start from the defaults.
//
// The level and the weight of each component are DEFAULTS — assembly-time
// metadata, not part of the cost. Override them per instance through the
// assembly's knob setters (`weight` / `level` (or its `hard` / `soft`
// shorthands) / `exponent` / `offset` / `clip`) or the validated
// `set_weight` / `set_level` / `set_exponent` / `set_offset` of the component
// (`component->set_level(optimaru::Level::Medium)`). Any component may take
// any of the three levels. A medium component moved to hard costs 1 hard per
// raw unit, moved to soft 1 soft per raw unit (outside the fold); an explicit
// weight stays in units of the level in effect.
//
// Cost sign convention (see score/decomposed_objective.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all twelve
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::score.
//
// Every component requires ScoreCalculator::prepare(schedule) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator
    = optimaru::DecomposedObjective<MeetingSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<MeetingSchedule>;
using MsUnitConstraint = optimaru::UnitConstraint<MeetingSchedule>;
using MsGlobalConstraint = optimaru::GlobalConstraint<MeetingSchedule>;

// A component's kDefaultWeight as an exact integer, for the monolithic
// calculators (Breakdown's weighted totals and the incremental calculator),
// which are the default assembly computed in int64. Fails to compile if a
// default weight stops being integral or if a component leaves the level the
// monolith sums it into.
template <typename Component, optimaru::Level kLevel>
constexpr std::int64_t monolithicWeight()
{
    static_assert(Component::kDefaultLevel == kLevel,
                  "the monolithic calculators sum this constraint into "
                  "another level");
    constexpr auto weight
        = static_cast<std::int64_t>(Component::kDefaultWeight);
    static_assert(weight == Component::kDefaultWeight,
                  "the monolithic calculators assume integral default "
                  "weights");
    return weight;
}
template <typename Component>
constexpr std::int64_t kHardWeight
    = monolithicWeight<Component, optimaru::Level::Hard>();
template <typename Component>
constexpr std::int64_t kMediumWeight
    = monolithicWeight<Component, optimaru::Level::Medium>();
template <typename Component>
constexpr std::int64_t kSoftWeight
    = monolithicWeight<Component, optimaru::Level::Soft>();

namespace detail
{

inline void requirePrepared(const MeetingSchedule &schedule)
{
    if (!schedule.prepared)
    {
        throw std::logic_error("ms constraints require "
                               "ScoreCalculator::prepare(schedule) first");
    }
}

// The grain-index interval [start, end) an assignment occupies when placed in
// `unit` (a composite grainRoom).
inline void spanOf(const MeetingSchedule &schedule,
                   int unit,
                   int assignmentIndex,
                   int &start,
                   int &end)
{
    const int pos = schedule.timeGrainPosOf(unit);
    start = schedule.timeGrains[pos].grainIndex;
    end = start
          + schedule
                .meetings[schedule.meetingAssignments[assignmentIndex].meeting]
                .durationInGrains;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Per-assignment unit constraints
// ---------------------------------------------------------------------------
// Shared base of the five rules that judge one assignment against its own
// cell: their cost is a sum of independent per-(assignment, cell) terms, so
// the subclass writes `assignmentCost` only and both the full re-cost and the
// exact O(1) delta follow from it.
struct PerAssignmentConstraint : MsUnitConstraint
{
    using MsUnitConstraint::MsUnitConstraint;

    // The penalty units assignment `assignmentIndex` incurs in cell `unit`.
    virtual double assignmentCost(const MeetingSchedule &schedule,
                                  int unit,
                                  int assignmentIndex) const = 0;

    void on_reset(const MeetingSchedule &schedule) override
    {
        detail::requirePrepared(schedule);
    }
    double cost(const MeetingSchedule &schedule,
                int unit,
                const std::vector<int> &members) const override
    {
        double total = 0.0;
        for (int assignmentIndex : members)
            total += assignmentCost(schedule, unit, assignmentIndex);
        return total;
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const MeetingSchedule &schedule,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return assignmentCost(schedule, unit, entity);
    }
};

// ---------------------------------------------------------------------------
// Cross-cell pair constraints
// ---------------------------------------------------------------------------
// Shared base of the seven rules that compare two placed assignments. It
// tracks where each assignment currently sits — the solution's own variable
// cannot be read for that, because on a leave notification it already holds
// the NEW value — and asks the subclass for the marginal of one placement
// against every other. Join adds the marginal before the placement is
// recorded, leave subtracts it after the placement is cleared, so the
// aggregate is exactly reversible.
struct AssignmentPairConstraint : MsGlobalConstraint
{
    using MsGlobalConstraint::MsGlobalConstraint;

    bool supports_member_change() const override { return true; }
    void on_unit_changed(const MeetingSchedule & /*schedule*/,
                       int /*unit*/,
                       const std::vector<int> & /*members*/) override
    {
        throw std::logic_error(name
                               + ": onUnitChanged is unreachable "
                                 "(member-change fast path)");
    }

    void reset(const MeetingSchedule &schedule,
               const std::vector<std::vector<int>> &members_per_unit) override
    {
        detail::requirePrepared(schedule);
        placed_.assign(schedule.meetingAssignments.size(), -1);
        total_ = 0;
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit)
        {
            for (int assignmentIndex : members_per_unit[unit])
            {
                total_ += marginal(
                    schedule, assignmentIndex, static_cast<int>(unit));
                placed_[assignmentIndex] = static_cast<int>(unit);
            }
        }
    }

    void on_member_changed(const MeetingSchedule &schedule,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int entity,
                         bool joined) override
    {
        if (joined)
        {
            total_ += marginal(schedule, entity, unit);
            placed_[entity] = unit;
            return;
        }
        placed_[entity] = -1;
        total_ -= marginal(schedule, entity, unit);
    }

    double cost() const override { return static_cast<double>(total_); }

protected:
    // The cell assignment `assignmentIndex` currently occupies, or -1 — never
    // including the assignment whose marginal is being computed.
    int placedUnit(int assignmentIndex) const
    {
        return placed_[assignmentIndex];
    }

    // The pair terms `assignmentIndex` contributes when placed in `unit`,
    // against every OTHER currently placed assignment.
    virtual std::int64_t marginal(const MeetingSchedule &schedule,
                                  int assignmentIndex,
                                  int unit) const = 0;

private:
    std::vector<int> placed_;  // assignment -> its current cell, or -1
    std::int64_t total_ = 0;
};

// ---------------------------------------------------------------------------
// Attendance-arc constraints
// ---------------------------------------------------------------------------
// Shared base of the four rules keyed by a shared attendee: they read
// MeetingSchedule::conflictArcs (built by ScoreCalculator::prepare, one entry
// per meeting pair with at least one attendee in common) rather than every
// other assignment, so their marginal is O(arcs of this meeting). This base
// only builds the per-meeting index; each component writes its own marginal,
// because their pair terms differ in kind — three weigh overlap grains, one
// looks at ordering and room changes.
struct AttendanceArcConstraint : AssignmentPairConstraint
{
    using AssignmentPairConstraint::AssignmentPairConstraint;

    void reset(const MeetingSchedule &schedule,
               const std::vector<std::vector<int>> &members_per_unit) override
    {
        arcsByMeeting_.assign(schedule.meetings.size(), {});
        for (std::size_t i = 0; i < schedule.conflictArcs.size(); ++i)
        {
            const ConflictArc &arc = schedule.conflictArcs[i];
            arcsByMeeting_[arc.meeting1].push_back(static_cast<int>(i));
            if (arc.meeting2 != arc.meeting1)
                arcsByMeeting_[arc.meeting2].push_back(static_cast<int>(i));
        }
        AssignmentPairConstraint::reset(schedule, members_per_unit);
    }

protected:
    // The arcs touching `meeting`, each listed once.
    const std::vector<int> &arcsOf(int meeting) const
    {
        return arcsByMeeting_[meeting];
    }

    // The other end of `arc` seen from `meeting`, resolved to its assignment
    // and current cell. Returns false when the partner is this meeting itself
    // or is not currently placed.
    bool partnerOf(const MeetingSchedule &schedule,
                   const ConflictArc &arc,
                   int meeting,
                   int &partnerAssignment,
                   int &partnerUnit) const
    {
        const int partnerMeeting
            = arc.meeting1 == meeting ? arc.meeting2 : arc.meeting1;
        if (partnerMeeting == meeting)
            return false;
        partnerAssignment = schedule.assignmentOfMeeting[partnerMeeting];
        if (partnerAssignment < 0)
            return false;
        partnerUnit = placedUnit(partnerAssignment);
        return partnerUnit >= 0;
    }

private:
    std::vector<std::vector<int>> arcsByMeeting_;
};

}  // namespace ms

#endif  // MS_CONSTRAINTS_COMMON_HPP
