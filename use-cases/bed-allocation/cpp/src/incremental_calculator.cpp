#include "ba/incremental_calculator.hpp"

#include <algorithm>
#include <limits>

#include "ba/score_calculator.hpp"

namespace ba
{

void BedAllocationIncrementalScoreCalculator::resetWorkingSolution(
    BedPlan &workingSolution)
{
    plan_ = &workingSolution;
    ScoreCalculator::prepare(workingSolution);

    // Planning horizon: only the days some stay can occupy are tracked.
    // Stays with departureDay < arrivalDay occupy nothing (sameNightCount
    // clamps at 0), so they are skipped everywhere below.
    int minDay = std::numeric_limits<int>::max();
    int maxDay = std::numeric_limits<int>::min();
    for (const Stay &stay : plan_->stays)
    {
        if (stay.departureDay < stay.arrivalDay)
            continue;
        minDay = std::min(minDay, stay.arrivalDay);
        maxDay = std::max(maxDay, stay.departureDay);
    }
    if (minDay > maxDay)
    {
        dayOffset_ = 0;
        dayCount_ = 0;
    }
    else
    {
        dayOffset_ = minDay;
        dayCount_ = maxDay - minDay + 1;
    }

    // Single-stay impact table: one call to the reference singleStayImpact
    // per (stay, room) — the impact of a bed depends on the bed only through
    // its room — plus one per stay for the sentinel. All inputs are problem
    // facts, so the table is valid for the whole solve.
    roomCount_ = static_cast<int>(plan_->rooms.size());
    const std::size_t stayCount = plan_->stays.size();
    singleHard_.assign(stayCount * static_cast<std::size_t>(roomCount_), 0);
    singleSoft_.assign(stayCount * static_cast<std::size_t>(roomCount_), 0);
    sentinelMedium_.assign(stayCount, 0);
    std::vector<int> firstBedOfRoom(plan_->rooms.size(), -1);
    for (std::size_t b = 0; b < plan_->beds.size(); ++b)
    {
        int &first = firstBedOfRoom[plan_->beds[b].room];
        if (first < 0)
            first = static_cast<int>(b);
    }
    for (std::size_t s = 0; s < stayCount; ++s)
    {
        sentinelMedium_[s]
            = ScoreCalculator::singleStayImpact(
                  *plan_, static_cast<int>(s), plan_->noBedValue())
                  .medium;
        for (int r = 0; r < roomCount_; ++r)
        {
            if (firstBedOfRoom[r] < 0)
                continue;  // bed-less room: no planning value maps to it
            const ScoreCalculator::SingleImpact impact
                = ScoreCalculator::singleStayImpact(
                    *plan_, static_cast<int>(s), firstBedOfRoom[r]);
            const std::size_t index = s * static_cast<std::size_t>(roomCount_)
                                      + static_cast<std::size_t>(r);
            singleHard_[index] = impact.hard;
            singleSoft_[index] = impact.soft;
        }
    }

    assign_.assign(plan_->stays.size(), -1);
    bedDay_.assign(plan_->beds.size() * static_cast<std::size_t>(dayCount_), 0);
    roomDay_.assign(
        plan_->rooms.size() * 2 * static_cast<std::size_t>(dayCount_), 0);
    hardSingles_ = 0;
    hardBedPairs_ = 0;
    hardRoomPairs_ = 0;
    medium_ = 0;
    softSingles_ = 0;

    for (std::size_t s = 0; s < plan_->stays.size(); ++s)
        insert(static_cast<int>(s), plan_->stays[s].bed);
}

void BedAllocationIncrementalScoreCalculator::beforeVariableChanged(
    int stayIndex)
{
    retract(stayIndex, assign_[stayIndex]);
}

void BedAllocationIncrementalScoreCalculator::afterVariableChanged(
    int stayIndex)
{
    insert(stayIndex, plan_->stays[stayIndex].bed);
}

void BedAllocationIncrementalScoreCalculator::insert(int stayIndex, int value)
{
    assign_[stayIndex] = value;
    if (value < 0)
        return;

    if (value >= plan_->bedCount())
    {
        // "no bed" sentinel: medium only, no pair terms.
        medium_ += sentinelMedium_[stayIndex];
        return;
    }
    const int impactRoom = plan_->beds[value].room;
    const std::size_t impactIndex = static_cast<std::size_t>(stayIndex)
                                        * static_cast<std::size_t>(roomCount_)
                                    + static_cast<std::size_t>(impactRoom);
    hardSingles_ += singleHard_[impactIndex];
    softSingles_ += singleSoft_[impactIndex];

    const Stay &stay = plan_->stays[stayIndex];
    if (stay.departureDay < stay.arrivalDay)
        return;
    const int first = stay.arrivalDay - dayOffset_;
    const int last = stay.departureDay - dayOffset_;
    const int roomIndex = plan_->beds[value].room;

    // hard 1: each already-present occupant of this bed on this night is one
    // new penalized pair.
    std::int32_t *bedDays
        = bedDay_.data() + static_cast<std::size_t>(value) * dayCount_;
    for (int day = first; day <= last; ++day)
        hardBedPairs_ += 1000LL * bedDays[day]++;

    if (plan_->rooms[roomIndex].genderLimitation
        == GenderLimitation::SAME_GENDER)
    {
        // hard 4: pairs with the opposite-gender occupants of the room.
        const int own = stay.patientGender == Gender::MALE ? 0 : 1;
        std::int32_t *ownDays
            = roomDay_.data()
              + (static_cast<std::size_t>(roomIndex) * 2 + own) * dayCount_;
        const std::int32_t *otherDays
            = roomDay_.data()
              + (static_cast<std::size_t>(roomIndex) * 2 + (1 - own))
                    * dayCount_;
        for (int day = first; day <= last; ++day)
        {
            hardRoomPairs_ += 1000LL * otherDays[day];
            ++ownDays[day];
        }
    }
}

void BedAllocationIncrementalScoreCalculator::retract(int stayIndex, int value)
{
    assign_[stayIndex] = -1;
    if (value < 0)
        return;

    if (value >= plan_->bedCount())
    {
        medium_ -= sentinelMedium_[stayIndex];
        return;
    }
    const int impactRoom = plan_->beds[value].room;
    const std::size_t impactIndex = static_cast<std::size_t>(stayIndex)
                                        * static_cast<std::size_t>(roomCount_)
                                    + static_cast<std::size_t>(impactRoom);
    hardSingles_ -= singleHard_[impactIndex];
    softSingles_ -= singleSoft_[impactIndex];

    const Stay &stay = plan_->stays[stayIndex];
    if (stay.departureDay < stay.arrivalDay)
        return;
    const int first = stay.arrivalDay - dayOffset_;
    const int last = stay.departureDay - dayOffset_;
    const int roomIndex = plan_->beds[value].room;

    std::int32_t *bedDays
        = bedDay_.data() + static_cast<std::size_t>(value) * dayCount_;
    for (int day = first; day <= last; ++day)
        hardBedPairs_ -= 1000LL * --bedDays[day];

    if (plan_->rooms[roomIndex].genderLimitation
        == GenderLimitation::SAME_GENDER)
    {
        const int own = stay.patientGender == Gender::MALE ? 0 : 1;
        std::int32_t *ownDays
            = roomDay_.data()
              + (static_cast<std::size_t>(roomIndex) * 2 + own) * dayCount_;
        const std::int32_t *otherDays
            = roomDay_.data()
              + (static_cast<std::size_t>(roomIndex) * 2 + (1 - own))
                    * dayCount_;
        for (int day = first; day <= last; ++day)
        {
            --ownDays[day];
            hardRoomPairs_ -= 1000LL * otherDays[day];
        }
    }
}

optimaru::HardSoftScore
BedAllocationIncrementalScoreCalculator::calculateScore()
{
    return toEngineScore(Score{-(hardSingles_ + hardBedPairs_ + hardRoomPairs_),
                               -medium_,
                               -softSingles_});
}

}  // namespace ba
