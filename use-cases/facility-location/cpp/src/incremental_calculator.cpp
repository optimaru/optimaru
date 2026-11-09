#include "fl/incremental_calculator.hpp"

#include <cstdint>

#include "fl/constraints/distance_from_facility_cost.hpp"
#include "fl/constraints/facility_capacity_cost.hpp"
#include "fl/constraints/facility_setup_cost.hpp"
#include "fl/score_calculator.hpp"

namespace fl
{

namespace
{

// The same constants the components and ScoreCalculator read (integral; see
// score_calculator.cpp).
constexpr auto kCapacityWeight
    = static_cast<std::int64_t>(FacilityCapacityCost::kDefaultWeight);
constexpr auto kSetupWeight
    = static_cast<std::int64_t>(FacilitySetupCost::kDefaultWeight);
constexpr auto kDistanceWeight
    = static_cast<std::int64_t>(DistanceFromFacilityCost::kDefaultWeight);

}  // namespace

void FacilityLocationIncrementalScoreCalculator::rebuild(
    FacilityLocationProblem &solution)
{
    ScoreCalculator::prepare(solution);  // distance matrix
    problem_ = &solution;

    usedDemand_.assign(solution.facilities.size(), 0);
    consumerCnt_.assign(solution.facilities.size(), 0);
    overCapacity_ = 0;
    usedSetupCost_ = 0;
    distance_ = 0;

    assign_.assign(solution.consumers.size(), -1);
    for (int c = 0; c < solution.consumerCount(); ++c)
    {
        const int facility = solution.consumers[c].facility;
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

    // hard 1: facilityCapacityCost — replace this facility's penalty term.
    overCapacity_ -= capacityPenalty(facilityIndex);
    usedDemand_[facilityIndex] += consumer.demand;
    ++consumerCnt_[facilityIndex];
    overCapacity_ += capacityPenalty(facilityIndex);

    // soft 2: facilitySetupCost — first consumer switches the facility on.
    if (consumerCnt_[facilityIndex] == 1)
        usedSetupCost_ += problem_->facilities[facilityIndex].setupCost;

    // soft 3: distanceFromFacilityCost.
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

void FacilityLocationIncrementalScoreCalculator::before_assign(
    int consumerIndex)
{
    const int old = assign_[consumerIndex];
    if (old >= 0)
        retract(consumerIndex, old);  // unassigned: nothing to retract
    assign_[consumerIndex] = -1;
}

void FacilityLocationIncrementalScoreCalculator::after_assign(
    int consumerIndex)
{
    const int now
        = problem_->consumers[consumerIndex].facility;  // the new value
    assign_[consumerIndex] = now;
    if (now >= 0)
        insert(consumerIndex, now);  // now unassigned: nothing to insert
}

Score FacilityLocationIncrementalScoreCalculator::score()
{
    Score score;
    score.hard = -kCapacityWeight * overCapacity_;
    // Negate in integer space so a zero penalty prints as 0, not -0.
    score.soft = static_cast<double>(
        -(kSetupWeight * usedSetupCost_ + kDistanceWeight * distance_));
    return score;
}

}  // namespace fl
