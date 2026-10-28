#ifndef MNT_DOMAIN_HPP
#define MNT_DOMAIN_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace mnt
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The Java quickstart already uses HardSoftScore, so the engine's
// HardSoftScore (int64 hard, double soft) maps 1:1 — no level remapping is
// needed. The soft weights are kept verbatim from the Java
// ConstraintProvider: beforeIdealEndDate x1, tagConflict x1'000,
// afterIdealEndDate x1'000'000 per day. All contributions are integral, so
// the double soft field stays exact.
using Score = optimaru::HardSoftScore;

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------
// Java's LocalDate becomes an epoch day: days since 1970-01-01 (a Thursday),
// exactly LocalDate::toEpochDay / Python date.toordinal() - 719163. kNoDate
// stands in for Java null on the optional min/max/ideal date fields.
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

// Port of Job.calculateEndDate / plusBusinessDays: walk forward from
// startDate counting only non-weekend days; the result is the EXCLUSIVE end
// date after `businessDays` working days (it may land on a Saturday).
// Matches the Java loop exactly, including businessDays == 0 -> startDate.
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
// Crew  (problem fact, @ValueRangeProvider half #1)
// ---------------------------------------------------------------------------
struct Crew
{
    std::string id;
    std::string name;
};

// ---------------------------------------------------------------------------
// Job  (@PlanningEntity)
// ---------------------------------------------------------------------------
// The Java entity has TWO planning variables (crew, startDate — the TimeGrain
// pattern) plus a shadow endDate. The engine supports one BASIC int variable
// per entity, so both are composed into a single value (the school-
// timetabling slotRoom pattern):
//   crewDate = startDateIndex * crewCount + crewIndex
// where startDateIndex indexes MaintenanceSchedule::startDates (the business
// days of the work calendar — Java's createStartDateList()). -1 means
// unassigned (both Java variables null). The shadow endDate is a pure
// function of (startDate, durationInDays) and is precomputed per (duration,
// startDateIndex) by ScoreCalculator::prepare, so it needs no listener.
struct Job
{
    std::string id;
    std::string name;
    int durationInDays = 1;  // business days, weekends skipped

    std::int64_t minStartDate = kNoDate;  // inclusive; kNoDate = none
    std::int64_t maxEndDate = kNoDate;    // exclusive; kNoDate = none
    std::int64_t idealEndDate = kNoDate;  // exclusive; kNoDate = none

    std::vector<std::string> tags;  // Java SequencedSet<String>

    int crewDate = -1;  // planning variable; -1 == unassigned

    // Extension over the quickstart (engine feature): pinned jobs keep their
    // crewDate fixed — scored like any job but never moved by the solver.
    bool pinned = false;

    // Precomputed by ScoreCalculator::prepare (interned, deduplicated tags).
    std::vector<int> tagIds;
};

// ---------------------------------------------------------------------------
// MaintenanceSchedule  (@PlanningSolution)
// ---------------------------------------------------------------------------
// WorkCalendar is inlined as fromDate/toDate (its only fields besides id).
struct MaintenanceSchedule
{
    std::int64_t fromDate = 0;  // inclusive
    std::int64_t toDate = 0;    // exclusive

    std::vector<Crew> crews;
    std::vector<Job> jobs;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against rebuilding the
    // derived tables on every calculateScore call.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int tagCount = 0;
    // Value range of the date half: every non-weekend day in
    // [fromDate, toDate) — Java's createStartDateList().
    std::vector<std::int64_t> startDates;
    int maxDuration = 0;
    // endDateByDur[dur * dateCount() + i] = plusBusinessDays(startDates[i],
    // dur): the shadow endDate for any (duration, startDateIndex) pair.
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

    // Shadow endDate lookup (valid after prepare).
    std::int64_t endDateFor(int durationInDays, int startDateIndex) const
    {
        return endDateByDur[static_cast<std::size_t>(durationInDays)
                                * dateCount()
                            + startDateIndex];
    }
};

}  // namespace mnt

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are jobs, the planning variable is
// Job::crewDate, the value range is the (startDate x crew) grid.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<mnt::MaintenanceSchedule>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const mnt::MaintenanceSchedule &s)
    {
        return static_cast<int>(s.jobs.size());
    }
    static int valueCount(const mnt::MaintenanceSchedule &s)
    {
        return s.crewDateCount();
    }
    static int getVariable(const mnt::MaintenanceSchedule &s, int jobIndex)
    {
        return s.jobs[jobIndex].crewDate;
    }
    static void
    setVariable(mnt::MaintenanceSchedule &s, int jobIndex, int crewDate)
    {
        s.jobs[jobIndex].crewDate = crewDate;
    }
    static bool isPinned(const mnt::MaintenanceSchedule &s, int jobIndex)
    {
        return s.jobs[jobIndex].pinned;
    }
};

#endif  // MNT_DOMAIN_HPP
