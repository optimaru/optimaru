#include "fl/incremental_calculator.hpp"

#include "fl/score_calculator.hpp"

namespace fl
{

void FacilityLocationIncrementalScoreCalculator::resetWorkingSolution(
    FacilityLocationProblem &workingSolution)
{
    ScoreCalculator::prepare(workingSolution);  // distance matrix
    problem_ = &workingSolution;

    usedDemand_.assign(workingSolution.facilities.size(), 0);
    consumerCnt_.assign(workingSolution.facilities.size(), 0);
    overCapacity_ = 0;
    usedSetupCost_ = 0;
    distance_ = 0;

    assign_.assign(workingSolution.consumers.size(), -1);
    for (int c = 0; c < workingSolution.consumerCount(); ++c)
    {
        const int facility = workingSolution.consumers[c].facility;
        assign_[c] = facility;
        if (facility >= 0)
            insert(c, facility);
    }
}

// Adds consumer `consumerIndex` to facility `facilityIndex`, updating each
// aggregate and the delta of that facility's capacity penalty.
void FacilityLocationIncrementalScoreCalculator::insert(int consumerIndex,
                                                        int facilityIndex)
{
    const Consumer &consumer = problem_->consumers[consumerIndex];

    // hard 1: facilityCapacity — replace this facility's penalty term.
    overCapacity_ -= capacityPenalty(facilityIndex);
    usedDemand_[facilityIndex] += consumer.demand;
    ++consumerCnt_[facilityIndex];
    overCapacity_ += capacityPenalty(facilityIndex);

    // soft 2: facilitySetupCost — first consumer switches the facility on.
    if (consumerCnt_[facilityIndex] == 1)
        usedSetupCost_ += problem_->facilities[facilityIndex].setupCost;

    // soft 3: distanceFromFacility.
    distance_ += problem_->distanceOf(consumerIndex, facilityIndex);
}

// Exact mirror of insert.
void FacilityLocationIncrementalScoreCalculator::retract(int consumerIndex,
                                                         int facilityIndex)
{
    const Consumer &consumer = problem_->consumers[consumerIndex];

    overCapacity_ -= capacityPenalty(facilityIndex);
    usedDemand_[facilityIndex] -= consumer.demand;
    --consumerCnt_[facilityIndex];
    overCapacity_ += capacityPenalty(facilityIndex);

    if (consumerCnt_[facilityIndex] == 0)
        usedSetupCost_ -= problem_->facilities[facilityIndex].setupCost;

    distance_ -= problem_->distanceOf(consumerIndex, facilityIndex);
}

void FacilityLocationIncrementalScoreCalculator::beforeVariableChanged(
    int consumerIndex)
{
    const int old = assign_[consumerIndex];
    if (old >= 0)
        retract(consumerIndex, old);  // unassigned: nothing to retract
    assign_[consumerIndex] = -1;
}

void FacilityLocationIncrementalScoreCalculator::afterVariableChanged(
    int consumerIndex)
{
    const int now
        = problem_->consumers[consumerIndex].facility;  // the new value
    assign_[consumerIndex] = now;
    if (now >= 0)
        insert(consumerIndex, now);  // now unassigned: nothing to insert
}

Score FacilityLocationIncrementalScoreCalculator::calculateScore()
{
    Score score;
    score.hard = -overCapacity_;
    // Negate in integer space so a zero penalty prints as 0, not -0.
    score.soft = static_cast<double>(
        -(problem_->setupCostWeight * usedSetupCost_
          + problem_->distanceFromFacilityWeight * distance_));
    return score;
}

}  // namespace fl
