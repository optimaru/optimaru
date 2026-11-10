#include "mnt/incremental_calculator.hpp"

#include <cstddef>
#include <cstdint>

#include "mnt/constraints/after_ideal_end_date_cost.hpp"
#include "mnt/constraints/before_ideal_end_date_cost.hpp"
#include "mnt/constraints/crew_conflict_cost.hpp"
#include "mnt/constraints/max_end_date_cost.hpp"
#include "mnt/constraints/min_start_date_cost.hpp"
#include "mnt/constraints/tag_conflict_cost.hpp"
#include "mnt/score_calculator.hpp"
#include "optimaru/score/levels.hpp"

namespace mnt {

namespace {

// The same constants the components and ScoreCalculator read. Integral, so
// both levels are computed in int64 exactly like the full calculator.
constexpr auto kCrewWeight = static_cast<std::int64_t>(CrewConflictCost::kDefaultWeight);
constexpr auto kMinStartWeight = static_cast<std::int64_t>(MinStartDateCost::kDefaultWeight);
constexpr auto kMaxEndWeight = static_cast<std::int64_t>(MaxEndDateCost::kDefaultWeight);
constexpr auto kBeforeIdealWeight =
        static_cast<std::int64_t>(BeforeIdealEndDateCost::kDefaultWeight);
constexpr auto kAfterIdealWeight = static_cast<std::int64_t>(AfterIdealEndDateCost::kDefaultWeight);
constexpr auto kTagWeight = static_cast<std::int64_t>(TagConflictCost::kDefaultWeight);
static_assert(kCrewWeight == CrewConflictCost::kDefaultWeight &&
                      kMinStartWeight == MinStartDateCost::kDefaultWeight &&
                      kMaxEndWeight == MaxEndDateCost::kDefaultWeight &&
                      kBeforeIdealWeight == BeforeIdealEndDateCost::kDefaultWeight &&
                      kAfterIdealWeight == AfterIdealEndDateCost::kDefaultWeight &&
                      kTagWeight == TagConflictCost::kDefaultWeight,
              "the incremental calculator assumes integral default weights");
static_assert(CrewConflictCost::kDefaultLevel == optimaru::Level::Hard &&
                      MinStartDateCost::kDefaultLevel == optimaru::Level::Hard &&
                      MaxEndDateCost::kDefaultLevel == optimaru::Level::Hard &&
                      BeforeIdealEndDateCost::kDefaultLevel == optimaru::Level::Soft &&
                      AfterIdealEndDateCost::kDefaultLevel == optimaru::Level::Soft &&
                      TagConflictCost::kDefaultLevel == optimaru::Level::Soft,
              "the incremental calculator assumes the default levels");

}  // namespace

void MaintenanceSchedulingIncrementalScoreCalculator::rebuild(MaintenanceSchedule& solution) {
    ScoreCalculator::prepare(solution);
    schedule_ = &solution;
    dayCount_ = solution.dayCount;

    crewDayCnt_.assign(static_cast<std::size_t>(solution.crewCount()) * dayCount_, 0);
    tagDayCnt_.assign(static_cast<std::size_t>(solution.tagCount) * dayCount_, 0);
    hotCells_.clear();
    hotCellPos_.assign(static_cast<std::size_t>(solution.tagCount) * dayCount_, -1);

    crewOverlap_ = 0;
    minStartViolation_ = 0;
    maxEndViolation_ = 0;
    beforeIdeal_ = 0;
    afterIdeal_ = 0;
    tagOverlap_ = 0;

    assign_.assign(solution.jobs.size(), -1);
    for (int i = 0; i < static_cast<int>(solution.jobs.size()); ++i) {
        const int crewDate = solution.jobs[i].crewDate;
        assign_[i] = crewDate;
        if (crewDate >= 0) insert(i, crewDate);
    }
}

// Adds job `jobIndex` at `crewDate` to every counter, accumulating the pair
// deltas against the OTHER members already counted. All counts are read
// before the job's own increment, so each delta is exactly the number of new
// overlap pairs the job forms.
void MaintenanceSchedulingIncrementalScoreCalculator::insert(int jobIndex, int crewDate) {
    const Job& job = schedule_->jobs[jobIndex];
    const int dateIndex = schedule_->dateIndexOf(crewDate);
    const int crewIndex = schedule_->crewIndexOf(crewDate);
    const std::int64_t startDate = schedule_->startDates[dateIndex];
    const std::int64_t endDate = schedule_->endDateFor(job.durationInDays, dateIndex);

    // hard 2/3 + soft 4/5: unary terms of the job's own dates.
    if (job.minStartDate != kNoDate && startDate < job.minStartDate)
        minStartViolation_ += job.minStartDate - startDate;
    if (job.maxEndDate != kNoDate && endDate > job.maxEndDate)
        maxEndViolation_ += endDate - job.maxEndDate;
    if (job.idealEndDate != kNoDate) {
        if (endDate < job.idealEndDate)
            beforeIdeal_ += job.idealEndDate - endDate;
        else if (endDate > job.idealEndDate)
            afterIdeal_ += endDate - job.idealEndDate;
    }

    // hard 1 + soft 6: per-day pair counts against the current occupants.
    for (std::int64_t day = startDate; day < endDate; ++day) {
        const int offset = static_cast<int>(day - schedule_->dayBase);
        crewOverlap_ += crewDayCnt_[crewDayOffset(crewIndex, offset)]++;
        for (int tag : job.tagIds) {
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
void MaintenanceSchedulingIncrementalScoreCalculator::retract(int jobIndex, int crewDate) {
    const Job& job = schedule_->jobs[jobIndex];
    const int dateIndex = schedule_->dateIndexOf(crewDate);
    const int crewIndex = schedule_->crewIndexOf(crewDate);
    const std::int64_t startDate = schedule_->startDates[dateIndex];
    const std::int64_t endDate = schedule_->endDateFor(job.durationInDays, dateIndex);

    if (job.minStartDate != kNoDate && startDate < job.minStartDate)
        minStartViolation_ -= job.minStartDate - startDate;
    if (job.maxEndDate != kNoDate && endDate > job.maxEndDate)
        maxEndViolation_ -= endDate - job.maxEndDate;
    if (job.idealEndDate != kNoDate) {
        if (endDate < job.idealEndDate)
            beforeIdeal_ -= job.idealEndDate - endDate;
        else if (endDate > job.idealEndDate)
            afterIdeal_ -= endDate - job.idealEndDate;
    }

    for (std::int64_t day = startDate; day < endDate; ++day) {
        const int offset = static_cast<int>(day - schedule_->dayBase);
        crewOverlap_ -= --crewDayCnt_[crewDayOffset(crewIndex, offset)];
        for (int tag : job.tagIds) {
            const std::size_t cell = tagDayOffset(tag, offset);
            const int count = --tagDayCnt_[cell];
            tagOverlap_ -= count;
            if (count == 1)  // 2 -> 1: the cell is no longer a conflict
                hotRemove(cell);
        }
    }
}

void MaintenanceSchedulingIncrementalScoreCalculator::before_assign(int jobIndex) {
    const int old = assign_[jobIndex];
    if (old >= 0) retract(jobIndex, old);  // unassigned: nothing to retract
    assign_[jobIndex] = -1;
}

void MaintenanceSchedulingIncrementalScoreCalculator::after_assign(int jobIndex) {
    const int now = schedule_->jobs[jobIndex].crewDate;  // the new value
    assign_[jobIndex] = now;
    if (now >= 0) insert(jobIndex, now);  // now unassigned: nothing to insert
}

Score MaintenanceSchedulingIncrementalScoreCalculator::score() {
    Score score;
    score.hard = -(kCrewWeight * crewOverlap_ + kMinStartWeight * minStartViolation_ +
                   kMaxEndWeight * maxEndViolation_);
    score.soft = optimaru::without_negative_zero(
            -static_cast<double>(kBeforeIdealWeight * beforeIdeal_ + kTagWeight * tagOverlap_ +
                                 kAfterIdealWeight * afterIdeal_));
    return score;
}

}  // namespace mnt
