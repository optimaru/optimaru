#ifndef FCS_CONSTRAINTS_COMMON_HPP
#define FCS_CONSTRAINTS_COMMON_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "fcs/domain.hpp"
#include "fcs/score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace fcs
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The six constraints of the Timefold flight-crew-scheduling
// FlightCrewSchedulingConstraintProvider, split into one small component each
// on the engine's DecomposedScoreCalculator — an optional assembly path NEXT
// TO the monolithic FlightCrewSchedulingIncrementalScoreCalculator, which
// stays the tuned default fast path.
//
// Unit choice: with the BASIC descriptor the calculator's unit is the VALUE,
// i.e. one employee; members are the flight assignments they hold. This
// domain decomposes unusually cleanly — every single constraint is a function
// of ONE employee's itinerary, so all six are UnitConstraints and there is no
// GlobalConstraint at all.
//
// Two of them are per-assignment terms (PerAssignmentConstraint below) with an
// exact O(1) delta. The other four read the itinerary as a whole and go
// through ScoreCalculator::itineraryStats, the very function the monolithic
// calculator uses — that is what keeps the "consecutive departure" and
// "earliest/latest departure, ties included" semantics identical rather than
// re-derived. flightConflict is a plain pair sum on top of that, so it also
// carries an exact delta; the other three are re-costed per touched employee.
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
// via the mutable `isHard` / `weight` / `exponent`. The Java weights (100 /
// 10 / 1 / 10 / 1'000 / 1'000) are exactly these defaults.
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score. An assembly of all six
// components at their default weights therefore scores bit-for-bit like
// ScoreCalculator::calculateScore.
//
// Every component requires ScoreCalculator::prepare(schedule) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator
    = optimaru::DecomposedScoreCalculator<FlightCrewSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<FlightCrewSchedule>;
using FcsUnitConstraint = optimaru::UnitConstraint<FlightCrewSchedule>;
using FcsGlobalConstraint = optimaru::GlobalConstraint<FlightCrewSchedule>;

namespace detail
{

inline void requirePrepared(const FlightCrewSchedule &schedule)
{
    if (!schedule.prepared)
    {
        throw std::logic_error("fcs constraints require "
                               "ScoreCalculator::prepare(schedule) first");
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Per-assignment unit constraints
// ---------------------------------------------------------------------------
// Shared base of the two rules that judge one (assignment, employee) pairing
// on its own: the subclass writes `violated` only, and both the full re-cost
// and the exact O(1) delta follow from it.
struct PerAssignmentConstraint : FcsUnitConstraint
{
    using FcsUnitConstraint::FcsUnitConstraint;

    virtual bool violated(const FlightCrewSchedule &schedule,
                          int assignmentIndex,
                          int employeeIndex) const = 0;

    void onReset(const FlightCrewSchedule &schedule) override
    {
        detail::requirePrepared(schedule);
    }
    double cost(const FlightCrewSchedule &schedule,
                int unit,
                const std::vector<int> &members) const override
    {
        std::int64_t violations = 0;
        for (int assignmentIndex : members)
            violations += violated(schedule, assignmentIndex, unit) ? 1 : 0;
        return static_cast<double>(violations);
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const FlightCrewSchedule &schedule,
                     int unit,
                     const std::vector<int> & /*members*/,
                     int entity) const override
    {
        return violated(schedule, entity, unit) ? 1.0 : 0.0;
    }
};

// ---------------------------------------------------------------------------
// Whole-itinerary unit constraints
// ---------------------------------------------------------------------------
// Shared base of the four rules that read one employee's itinerary as a
// whole. It sorts the unit's members by departure and runs
// ScoreCalculator::itineraryStats — the same function the monolithic
// calculator uses — so the subtle tie-handling of "consecutive departures"
// and "earliest / latest departure" stays defined in exactly one place. The
// subclass picks its field.
struct ItineraryConstraint : FcsUnitConstraint
{
    using FcsUnitConstraint::FcsUnitConstraint;

    virtual std::int64_t select(const ItineraryStats &stats) const = 0;

    void onReset(const FlightCrewSchedule &schedule) override
    {
        detail::requirePrepared(schedule);
    }
    double cost(const FlightCrewSchedule &schedule,
                int unit,
                const std::vector<int> &members) const override
    {
        sorted_ = members;
        ScoreCalculator::sortByDeparture(schedule, sorted_);
        return static_cast<double>(
            select(ScoreCalculator::itineraryStats(schedule, unit, sorted_)));
    }

private:
    // Scratch copy of the unit's members, reused across calls (cost() is
    // const by contract; the calculator drives one solve per instance).
    mutable std::vector<int> sorted_;
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_COMMON_HPP
