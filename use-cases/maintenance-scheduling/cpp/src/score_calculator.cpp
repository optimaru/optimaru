#include "mnt/score_calculator.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "mnt/constraints/after_ideal_end_date_cost.hpp"
#include "mnt/constraints/before_ideal_end_date_cost.hpp"
#include "mnt/constraints/crew_conflict_cost.hpp"
#include "mnt/constraints/max_end_date_cost.hpp"
#include "mnt/constraints/min_start_date_cost.hpp"
#include "mnt/constraints/tag_conflict_cost.hpp"
#include "optimaru/levels.hpp"

namespace mnt
{

namespace
{

// The monolithic score is the default assembly, so it reads the same
// constants the components carry. Integral, so both levels are computed in
// int64 exactly like the Java long arithmetic.
constexpr auto kCrewWeight
    = static_cast<std::int64_t>(CrewConflictCost::kDefaultWeight);
constexpr auto kMinStartWeight
    = static_cast<std::int64_t>(MinStartDateCost::kDefaultWeight);
constexpr auto kMaxEndWeight
    = static_cast<std::int64_t>(MaxEndDateCost::kDefaultWeight);
constexpr auto kBeforeIdealWeight
    = static_cast<std::int64_t>(BeforeIdealEndDateCost::kDefaultWeight);
constexpr auto kAfterIdealWeight
    = static_cast<std::int64_t>(AfterIdealEndDateCost::kDefaultWeight);
constexpr auto kTagWeight
    = static_cast<std::int64_t>(TagConflictCost::kDefaultWeight);
static_assert(
    kCrewWeight == CrewConflictCost::kDefaultWeight
        && kMinStartWeight == MinStartDateCost::kDefaultWeight
        && kMaxEndWeight == MaxEndDateCost::kDefaultWeight
        && kBeforeIdealWeight == BeforeIdealEndDateCost::kDefaultWeight
        && kAfterIdealWeight == AfterIdealEndDateCost::kDefaultWeight
        && kTagWeight == TagConflictCost::kDefaultWeight,
    "the monolithic calculator assumes integral default weights");
static_assert(CrewConflictCost::kDefaultLevel == optimaru::Level::Hard
                  && MinStartDateCost::kDefaultLevel == optimaru::Level::Hard
                  && MaxEndDateCost::kDefaultLevel == optimaru::Level::Hard
                  && BeforeIdealEndDateCost::kDefaultLevel
                         == optimaru::Level::Soft
                  && AfterIdealEndDateCost::kDefaultLevel
                         == optimaru::Level::Soft
                  && TagConflictCost::kDefaultLevel == optimaru::Level::Soft,
              "the monolithic calculator assumes the default levels");

}  // namespace

void ScoreCalculator::prepare(MaintenanceSchedule &schedule)
{
    if (schedule.prepared)
        return;

    // Intern tags into dense ids so the scoring loops index flat count
    // arrays instead of hashing strings. Java's SequencedSet is unique, but
    // the C++ tags vector is deduplicated defensively.
    std::unordered_map<std::string, int> tagId;
    for (Job &job : schedule.jobs)
    {
        job.tagIds.clear();
        for (const std::string &tag : job.tags)
        {
            auto it = tagId.find(tag);
            int id;
            if (it != tagId.end())
            {
                id = it->second;
            }
            else
            {
                id = static_cast<int>(tagId.size());
                tagId.emplace(tag, id);
            }
            if (std::find(job.tagIds.begin(), job.tagIds.end(), id)
                == job.tagIds.end())
            {
                job.tagIds.push_back(id);
            }
        }
    }
    schedule.tagCount = static_cast<int>(tagId.size());

    // The date half of the value range: every non-weekend day in
    // [fromDate, toDate) — Java's MaintenanceSchedule.createStartDateList().
    schedule.startDates.clear();
    for (std::int64_t day = schedule.fromDate; day < schedule.toDate; ++day)
    {
        if (!isWeekend(day))
            schedule.startDates.push_back(day);
    }

    // The endDate shadow table: end for every (duration, startDateIndex).
    schedule.maxDuration = 0;
    for (const Job &job : schedule.jobs)
    {
        schedule.maxDuration
            = std::max(schedule.maxDuration, job.durationInDays);
    }
    const int dateCount = schedule.dateCount();
    schedule.endDateByDur.assign(
        static_cast<std::size_t>(schedule.maxDuration + 1) * dateCount, 0);
    for (int dur = 0; dur <= schedule.maxDuration; ++dur)
    {
        for (int i = 0; i < dateCount; ++i)
        {
            schedule.endDateByDur[static_cast<std::size_t>(dur) * dateCount + i]
                = plusBusinessDays(schedule.startDates[i], dur);
        }
    }

    // Day horizon: every day any assignment can cover. End dates are
    // monotonic in both start index and duration, so the horizon ends at the
    // last start's end under the maximum duration.
    schedule.dayBase
        = dateCount > 0 ? schedule.startDates.front() : schedule.fromDate;
    const std::int64_t dayEnd
        = dateCount > 0
              ? schedule.endDateFor(schedule.maxDuration, dateCount - 1)
              : schedule.fromDate;
    schedule.dayCount = static_cast<int>(dayEnd - schedule.dayBase);

    schedule.prepared = true;
}

Score ScoreCalculator::score(MaintenanceSchedule &schedule)
{
    prepare(schedule);
    Score score;
    const int dayCount = schedule.dayCount;

    // Per-day occupancy counters for the two pairwise constraints.
    std::vector<int> crewDayCnt(
        static_cast<std::size_t>(schedule.crewCount()) * dayCount, 0);
    std::vector<int> tagDayCnt(
        static_cast<std::size_t>(schedule.tagCount) * dayCount, 0);

    std::int64_t minStartViolation = 0;  // hard 2: minStartDateCost (days)
    std::int64_t maxEndViolation = 0;    // hard 3: maxEndDateCost (days)
    std::int64_t beforeIdeal = 0;  // soft 4: beforeIdealEndDateCost (days)
    std::int64_t afterIdeal = 0;   // soft 5: afterIdealEndDateCost (days)

    for (const Job &job : schedule.jobs)
    {
        if (job.crewDate < 0)
            continue;  // unassigned: contributes nothing
        const int dateIndex = schedule.dateIndexOf(job.crewDate);
        const int crewIndex = schedule.crewIndexOf(job.crewDate);
        const std::int64_t startDate = schedule.startDates[dateIndex];
        const std::int64_t endDate
            = schedule.endDateFor(job.durationInDays, dateIndex);

        if (job.minStartDate != kNoDate && startDate < job.minStartDate)
            minStartViolation += job.minStartDate - startDate;
        if (job.maxEndDate != kNoDate && endDate > job.maxEndDate)
            maxEndViolation += endDate - job.maxEndDate;
        if (job.idealEndDate != kNoDate)
        {
            if (endDate < job.idealEndDate)
                beforeIdeal += job.idealEndDate - endDate;
            else if (endDate > job.idealEndDate)
                afterIdeal += endDate - job.idealEndDate;
        }

        for (std::int64_t day = startDate; day < endDate; ++day)
        {
            const int offset = static_cast<int>(day - schedule.dayBase);
            ++crewDayCnt[static_cast<std::size_t>(crewIndex) * dayCount
                         + offset];
            for (int tag : job.tagIds)
            {
                ++tagDayCnt[static_cast<std::size_t>(tag) * dayCount + offset];
            }
        }
    }

    // hard 1: crewConflictCost — summed pairwise overlap in days.
    std::int64_t crewOverlap = 0;
    for (int c : crewDayCnt)
        crewOverlap += pairs(c);
    // soft 6: tagConflictCost — summed pairwise overlap in (shared tag, day).
    std::int64_t tagOverlap = 0;
    for (int c : tagDayCnt)
        tagOverlap += pairs(c);

    score.hard = -(kCrewWeight * crewOverlap
                   + kMinStartWeight * minStartViolation
                   + kMaxEndWeight * maxEndViolation);
    score.soft = optimaru::without_negative_zero(-static_cast<double>(
        kBeforeIdealWeight * beforeIdeal
        + kTagWeight * tagOverlap
        + kAfterIdealWeight * afterIdeal));

    schedule.score = score;
    return score;
}

}  // namespace mnt
