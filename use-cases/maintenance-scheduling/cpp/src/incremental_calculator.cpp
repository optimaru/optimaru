#include "mnt/incremental_calculator.hpp"

#include <cstddef>

#include "mnt/score_calculator.hpp"

namespace mnt
{

void MaintenanceSchedulingIncrementalScoreCalculator::resetWorkingSolution(
    MaintenanceSchedule &workingSolution)
{
    ScoreCalculator::prepare(workingSolution);
    schedule_ = &workingSolution;
    dayCount_ = workingSolution.dayCount;

    crewDayCnt_.assign(
        static_cast<std::size_t>(workingSolution.crewCount()) * dayCount_, 0);
    tagDayCnt_.assign(
        static_cast<std::size_t>(workingSolution.tagCount) * dayCount_, 0);
    hotCells_.clear();
    hotCellPos_.assign(
        static_cast<std::size_t>(workingSolution.tagCount) * dayCount_, -1);

    crewOverlap_ = 0;
    minStartViolation_ = 0;
    maxEndViolation_ = 0;
    beforeIdeal_ = 0;
    afterIdeal_ = 0;
    tagOverlap_ = 0;

    assign_.assign(workingSolution.jobs.size(), -1);
    for (int i = 0; i < static_cast<int>(workingSolution.jobs.size()); ++i)
    {
        const int crewDate = workingSolution.jobs[i].crewDate;
        assign_[i] = crewDate;
        if (crewDate >= 0)
            insert(i, crewDate);
    }
}

// Adds job `jobIndex` at `crewDate` to every counter, accumulating the pair
// deltas against the OTHER members already counted. All counts are read
// before the job's own increment, so each delta is exactly the number of new
// overlap pairs the job forms.
void MaintenanceSchedulingIncrementalScoreCalculator::insert(int jobIndex,
                                                             int crewDate)
{
    const Job &job = schedule_->jobs[jobIndex];
    const int dateIndex = schedule_->dateIndexOf(crewDate);
    const int crewIndex = schedule_->crewIndexOf(crewDate);
    const std::int64_t startDate = schedule_->startDates[dateIndex];
    const std::int64_t endDate
        = schedule_->endDateFor(job.durationInDays, dateIndex);

    // hard 2/3 + soft 4/5: unary terms of the job's own dates.
    if (job.minStartDate != kNoDate && startDate < job.minStartDate)
        minStartViolation_ += job.minStartDate - startDate;
    if (job.maxEndDate != kNoDate && endDate > job.maxEndDate)
        maxEndViolation_ += endDate - job.maxEndDate;
    if (job.idealEndDate != kNoDate)
    {
        if (endDate < job.idealEndDate)
            beforeIdeal_ += job.idealEndDate - endDate;
        else if (endDate > job.idealEndDate)
            afterIdeal_ += endDate - job.idealEndDate;
    }

    // hard 1 + soft 6: per-day pair counts against the current occupants.
    for (std::int64_t day = startDate; day < endDate; ++day)
    {
        const int offset = static_cast<int>(day - schedule_->dayBase);
        crewOverlap_ += crewDayCnt_[crewDayOffset(crewIndex, offset)]++;
        for (int tag : job.tagIds)
        {
            const std::size_t cell = tagDayOffset(tag, offset);
            const int count = tagDayCnt_[cell]++;
            tagOverlap_ += count;
            if (count == 1)  // 1 -> 2: the cell just became a conflict
                hotAdd(cell);
        }
    }
}

// Exact mirror of insert: decrement the job's own counts first, then
// subtract the deltas against the remaining members.
void MaintenanceSchedulingIncrementalScoreCalculator::retract(int jobIndex,
                                                              int crewDate)
{
    const Job &job = schedule_->jobs[jobIndex];
    const int dateIndex = schedule_->dateIndexOf(crewDate);
    const int crewIndex = schedule_->crewIndexOf(crewDate);
    const std::int64_t startDate = schedule_->startDates[dateIndex];
    const std::int64_t endDate
        = schedule_->endDateFor(job.durationInDays, dateIndex);

    if (job.minStartDate != kNoDate && startDate < job.minStartDate)
        minStartViolation_ -= job.minStartDate - startDate;
    if (job.maxEndDate != kNoDate && endDate > job.maxEndDate)
        maxEndViolation_ -= endDate - job.maxEndDate;
    if (job.idealEndDate != kNoDate)
    {
        if (endDate < job.idealEndDate)
            beforeIdeal_ -= job.idealEndDate - endDate;
        else if (endDate > job.idealEndDate)
            afterIdeal_ -= endDate - job.idealEndDate;
    }

    for (std::int64_t day = startDate; day < endDate; ++day)
    {
        const int offset = static_cast<int>(day - schedule_->dayBase);
        crewOverlap_ -= --crewDayCnt_[crewDayOffset(crewIndex, offset)];
        for (int tag : job.tagIds)
        {
            const std::size_t cell = tagDayOffset(tag, offset);
            const int count = --tagDayCnt_[cell];
            tagOverlap_ -= count;
            if (count == 1)  // 2 -> 1: the cell is no longer a conflict
                hotRemove(cell);
        }
    }
}

void MaintenanceSchedulingIncrementalScoreCalculator::beforeVariableChanged(
    int jobIndex)
{
    const int old = assign_[jobIndex];
    if (old >= 0)
        retract(jobIndex, old);  // unassigned: nothing to retract
    assign_[jobIndex] = -1;
}

void MaintenanceSchedulingIncrementalScoreCalculator::afterVariableChanged(
    int jobIndex)
{
    const int now = schedule_->jobs[jobIndex].crewDate;  // the new value
    assign_[jobIndex] = now;
    if (now >= 0)
        insert(jobIndex, now);  // now unassigned: nothing to insert
}

Score MaintenanceSchedulingIncrementalScoreCalculator::calculateScore()
{
    Score score;
    score.hard = -(crewOverlap_ + minStartViolation_ + maxEndViolation_);
    score.soft = -static_cast<double>(beforeIdeal_ + 1'000 * tagOverlap_
                                      + 1'000'000 * afterIdeal_);
    return score;
}

}  // namespace mnt
