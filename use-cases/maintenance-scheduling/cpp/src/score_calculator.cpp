#include "mnt/score_calculator.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>

namespace mnt
{

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

Score ScoreCalculator::calculateScore(MaintenanceSchedule &schedule)
{
    prepare(schedule);
    Score score;
    const int dayCount = schedule.dayCount;

    // Per-day occupancy counters for the two pairwise constraints.
    std::vector<int> crewDayCnt(
        static_cast<std::size_t>(schedule.crewCount()) * dayCount, 0);
    std::vector<int> tagDayCnt(
        static_cast<std::size_t>(schedule.tagCount) * dayCount, 0);

    std::int64_t minStartViolation = 0;  // hard 2 (days)
    std::int64_t maxEndViolation = 0;    // hard 3 (days)
    std::int64_t beforeIdeal = 0;        // soft 4 (days)
    std::int64_t afterIdeal = 0;         // soft 5 (days)

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

    // hard 1: crewConflict — summed pairwise overlap in days.
    std::int64_t crewOverlap = 0;
    for (int c : crewDayCnt)
        crewOverlap += pairs(c);
    // soft 6: tagConflict — summed pairwise overlap in (shared tag, day).
    std::int64_t tagOverlap = 0;
    for (int c : tagDayCnt)
        tagOverlap += pairs(c);

    score.hard = -(crewOverlap + minStartViolation + maxEndViolation);
    score.soft = -static_cast<double>(beforeIdeal + 1'000 * tagOverlap
                                      + 1'000'000 * afterIdeal);

    schedule.score = score;
    return score;
}

}  // namespace mnt
