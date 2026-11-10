#ifndef MNT_DOMAIN_HPP
#define MNT_DOMAIN_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"

namespace mnt
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// This use case scores hard and soft only, so the engine's
// Score (int64 hard, double soft) maps 1:1 — no level remapping is
// needed (no medium level, so no optimaru::MediumFoldOf specialization). The
// soft weights are:
// beforeIdealEndDateCost x1, tagConflictCost x1'000, afterIdealEndDateCost
// x1'000'000 per day — the components' kDefaultWeight
// (mnt/constraints/*_cost.hpp). All contributions are integral, so the
// double soft field stays exact.
using Score = optimaru::Score;

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------
// A date is an epoch day: days since 1970-01-01 (a Thursday), i.e.
// Python date.toordinal() - 719163. kNoDate
// means "not set" on the optional min/max/ideal date fields.
constexpr std::int64_t kNoDate = std::numeric_limits<std::int64_t>::min();

// 0 = Monday .. 6 = Sunday (1970-01-01 is a Thursday, weekday 3).
inline int weekdayOf(std::int64_t epochDay)
{
    return static_cast<int>(((epochDay % 7) + 10) % 7);
}

inline bool isWeekend(std::int64_t epochDay)
{
    return weekdayOf(epochDay) >= 5;
}

// plusBusinessDays: walk forward from
// startDate counting only non-weekend days; the result is the EXCLUSIVE end
// date after `businessDays` working days (it may land on a Saturday).
// businessDays == 0 -> startDate.
inline std::int64_t plusBusinessDays(std::int64_t startDate, int businessDays)
{
    std::int64_t result = startDate;
    int addedDays = 0;
    while (addedDays < businessDays)
    {
        if (!isWeekend(result))
            ++addedDays;
        ++result;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Crew  (problem fact, the crew half of the value range)
// ---------------------------------------------------------------------------
struct Crew
{
    std::string id;
    std::string name;
};

// ---------------------------------------------------------------------------
// Job  (the entity the solver assigns)
// ---------------------------------------------------------------------------
// A job has TWO decisions (crew, startDate) plus a derived endDate.
// The engine supports one BASIC int variable
// per entity, so both are composed into a single value (the school-
// timetabling slotRoom pattern):
//   crewDate = startDateIndex * crewCount + crewIndex
// where startDateIndex indexes MaintenanceSchedule::startDates (the business
// days of the work calendar). -1 means
// unassigned. The derived endDate is a pure
// function of (startDate, durationInDays) and is precomputed per (duration,
// startDateIndex) by ScoreCalculator::prepare.
struct Job
{
    std::string id;
    std::string name;
    int durationInDays = 1;  // business days, weekends skipped

    std::int64_t minStartDate = kNoDate;  // inclusive; kNoDate = none
    std::int64_t maxEndDate = kNoDate;    // exclusive; kNoDate = none
    std::int64_t idealEndDate = kNoDate;  // exclusive; kNoDate = none

    std::vector<std::string> tags;  // a set; duplicates are ignored

    int crewDate = -1;  // decision variable; -1 == unassigned

    // Engine feature: pinned jobs keep their
    // crewDate fixed — scored like any job but never moved by the solver.
    bool pinned = false;

    // Precomputed by ScoreCalculator::prepare (interned, deduplicated tags).
    std::vector<int> tagIds;
};

// ---------------------------------------------------------------------------
// MaintenanceSchedule  (the problem and its solution)
// ---------------------------------------------------------------------------
// The work calendar is the [fromDate, toDate) pair.
struct MaintenanceSchedule
{
    std::int64_t fromDate = 0;  // inclusive
    std::int64_t toDate = 0;    // exclusive

    std::vector<Crew> crews;
    std::vector<Job> jobs;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against rebuilding the
    // derived tables on every score call.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int tagCount = 0;
    // Value range of the date half: every non-weekend day in
    // [fromDate, toDate).
    std::vector<std::int64_t> startDates;
    int maxDuration = 0;
    // endDateByDur[dur * dateCount() + i] = plusBusinessDays(startDates[i],
    // dur): the endDate for any (duration, startDateIndex) pair.
    std::vector<std::int64_t> endDateByDur;
    // Day horizon covered by any possible assignment; occupancy counters are
    // indexed by (epochDay - dayBase) in [0, dayCount).
    std::int64_t dayBase = 0;
    int dayCount = 0;

    // --- composite crewDate encoding helpers ---
    int crewCount() const { return static_cast<int>(crews.size()); }
    int dateCount() const { return static_cast<int>(startDates.size()); }
    int crewDateCount() const { return dateCount() * crewCount(); }
    int crewDateOf(int startDateIndex, int crewIndex) const
    {
        return startDateIndex * crewCount() + crewIndex;
    }
    int dateIndexOf(int crewDate) const { return crewDate / crewCount(); }
    int crewIndexOf(int crewDate) const { return crewDate % crewCount(); }

    // endDate lookup (valid after prepare).
    std::int64_t endDateFor(int durationInDays, int startDateIndex) const
    {
        return endDateByDur[static_cast<std::size_t>(durationInDays)
                                * dateCount()
                            + startDateIndex];
    }
};

}  // namespace mnt

// ---------------------------------------------------------------------------
// SolutionTraits specialization (describes the solution's shape to the
// engine): entities are jobs, the decision variable is
// Job::crewDate, the value range is the (startDate x crew) grid.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionTraits<mnt::MaintenanceSchedule>
{
    static constexpr optimaru::Shape shape
        = optimaru::Shape::Assignment;
    static int entity_count(const mnt::MaintenanceSchedule &s)
    {
        return static_cast<int>(s.jobs.size());
    }
    static int value_count(const mnt::MaintenanceSchedule &s)
    {
        return s.crewDateCount();
    }
    static int value_of(const mnt::MaintenanceSchedule &s, int jobIndex)
    {
        return s.jobs[jobIndex].crewDate;
    }
    static void
    assign(mnt::MaintenanceSchedule &s, int jobIndex, int crewDate)
    {
        s.jobs[jobIndex].crewDate = crewDate;
    }
    static bool is_pinned(const mnt::MaintenanceSchedule &s, int jobIndex)
    {
        return s.jobs[jobIndex].pinned;
    }
};

#endif  // MNT_DOMAIN_HPP
