#include "fp/incremental_calculator.hpp"

#include <algorithm>
#include <stdexcept>

namespace fp
{

namespace
{

Minutes
cleanupOrThrow(const PackagingSchedule &s, int previousProduct, int product)
{
    const Minutes cleanup = s.cleanup(previousProduct, product);
    if (cleanup == kMissingCleanup)
    {
        throw std::invalid_argument(
            "Cleanup duration previousProduct ("
            + s.products[previousProduct].name + ") to toProduct ("
            + s.products[product].name + ") is missing.");
    }
    return cleanup;
}

}  // namespace

void FoodPackagingIncrementalScoreCalculator::deriveLine(int l,
                                                         LineState &state) const
{
    const PackagingSchedule &s = *schedule_;
    const Line &line = s.lines[l];
    state.hardLate = 0;
    state.mediumLate = 0;
    state.makespanSq = 0;
    state.assignedDuration = 0;
    state.windows.clear();
    state.windows.reserve(line.jobs.size());

    Minutes previousEnd = line.startDateTime;
    int previousProduct = -1;
    for (const int j : line.jobs)
    {
        const Job &job = s.jobs[j];
        // The shadow chain (see Job in domain.hpp): the first job cleans and
        // produces from the line start; later jobs clean from the previous
        // end for cleanup(previous product -> product) minutes.
        const Minutes startCleaning = previousEnd;
        const Minutes startProduction
            = previousProduct < 0
                  ? line.startDateTime
                  : startCleaning
                        + cleanupOrThrow(s, previousProduct, job.productIndex);
        const Minutes end = startProduction + job.durationMinutes;

        if (job.maxEndTime < end)
            state.hardLate += end - job.maxEndTime;
        if (job.idealEndTime < end)
            state.mediumLate += end - job.idealEndTime;
        state.assignedDuration += job.durationMinutes;
        if (startCleaning < startProduction)
            state.windows.emplace_back(startCleaning, startProduction);

        previousEnd = end;
        previousProduct = job.productIndex;
    }
    if (!line.jobs.empty())
    {
        const std::int64_t makespan = previousEnd - line.startDateTime;
        state.makespanSq = makespan * makespan;
    }
}

std::int64_t FoodPackagingIncrementalScoreCalculator::pairOverlap(
    const std::vector<std::pair<Minutes, Minutes>> &a,
    const std::vector<std::pair<Minutes, Minutes>> &b)
{
    // Both window lists have non-decreasing starts AND ends (the chain is
    // sequential), so a sliding lower bound over b stays correct.
    std::int64_t total = 0;
    std::size_t lo = 0;
    for (const auto &[aStart, aEnd] : a)
    {
        while (lo < b.size() && b[lo].second <= aStart)
            ++lo;
        for (std::size_t i = lo; i < b.size() && b[i].first < aEnd; ++i)
        {
            const Minutes start = std::max(aStart, b[i].first);
            const Minutes end = std::min(aEnd, b[i].second);
            if (start < end)
                total += end - start;
        }
    }
    return total;
}

void FoodPackagingIncrementalScoreCalculator::resetWorkingSolution(
    PackagingSchedule &workingSolution)
{
    schedule_ = &workingSolution;
    const int lineCount = workingSolution.lineCount();

    // The operator assignment is fixed for the duration of one engine run
    // (fp::Solver re-optimizes it BETWEEN runs), so the same-operator line
    // groups can be precomputed here. Lines without an operator group
    // together, like Java's null-keyed Joiners.equal.
    sameOperatorLines_.assign(lineCount, {});
    for (int l = 0; l < lineCount; ++l)
    {
        for (int m = 0; m < lineCount; ++m)
        {
            if (m != l
                && workingSolution.lines[m].operatorIndex
                       == workingSolution.lines[l].operatorIndex)
                sameOperatorLines_[l].push_back(m);
        }
    }

    lineState_.assign(lineCount, {});
    pairOverlap_.assign(static_cast<std::size_t>(lineCount) * lineCount, 0);
    totalHardLate_ = 0;
    totalMediumLate_ = 0;
    totalMakespanSq_ = 0;
    conflictTotal_ = 0;
    assignedDuration_ = 0;
    totalDuration_ = 0;
    for (const Job &job : workingSolution.jobs)
        totalDuration_ += job.durationMinutes;

    for (int l = 0; l < lineCount; ++l)
    {
        deriveLine(l, lineState_[l]);
        totalHardLate_ += lineState_[l].hardLate;
        totalMediumLate_ += lineState_[l].mediumLate;
        totalMakespanSq_ += lineState_[l].makespanSq;
        assignedDuration_ += lineState_[l].assignedDuration;
    }
    for (int l = 0; l < lineCount; ++l)
    {
        for (const int m : sameOperatorLines_[l])
        {
            if (m < l)
                continue;  // each unordered pair once
            const std::int64_t overlap
                = pairOverlap(lineState_[l].windows, lineState_[m].windows);
            pairOverlap_[static_cast<std::size_t>(l) * lineCount + m] = overlap;
            pairOverlap_[static_cast<std::size_t>(m) * lineCount + l] = overlap;
            conflictTotal_ += overlap;
        }
    }
}

void FoodPackagingIncrementalScoreCalculator::beforeListChanged(int listIndex)
{
    if (listIndex >= schedule_->lineCount())
        return;  // pool: membership is scored via assignedDuration_ only
    const LineState &state = lineState_[listIndex];
    totalHardLate_ -= state.hardLate;
    totalMediumLate_ -= state.mediumLate;
    totalMakespanSq_ -= state.makespanSq;
    assignedDuration_ -= state.assignedDuration;
    const int lineCount = schedule_->lineCount();
    for (const int m : sameOperatorLines_[listIndex])
    {
        conflictTotal_
            -= pairOverlap_[static_cast<std::size_t>(listIndex) * lineCount
                            + m];
    }
}

void FoodPackagingIncrementalScoreCalculator::afterListChanged(int listIndex)
{
    if (listIndex >= schedule_->lineCount())
        return;
    LineState &state = lineState_[listIndex];
    deriveLine(listIndex, state);
    totalHardLate_ += state.hardLate;
    totalMediumLate_ += state.mediumLate;
    totalMakespanSq_ += state.makespanSq;
    assignedDuration_ += state.assignedDuration;
    const int lineCount = schedule_->lineCount();
    for (const int m : sameOperatorLines_[listIndex])
    {
        const std::int64_t overlap
            = pairOverlap(state.windows, lineState_[m].windows);
        pairOverlap_[static_cast<std::size_t>(listIndex) * lineCount + m]
            = overlap;
        pairOverlap_[static_cast<std::size_t>(m) * lineCount + listIndex]
            = overlap;
        conflictTotal_ += overlap;
    }
}

optimaru::HardSoftScore
FoodPackagingIncrementalScoreCalculator::calculateScore()
{
    return toEngineScore(currentScore());
}

}  // namespace fp
