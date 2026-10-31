#include "qap/incremental_calculator.hpp"

#include <cstdint>

#include "qap/constraints/assignment_cost.hpp"
#include "qap/constraints/flow_distance_cost.hpp"
#include "qap/constraints/forbidden_assignments_cost.hpp"
#include "qap/constraints/location_capacity_cost.hpp"
#include "qap/constraints/pinned_assignments_cost.hpp"
#include "qap/score_calculator.hpp"

namespace qap
{

namespace
{

// The same constants the components and ScoreCalculator read (integral; see
// score_calculator.cpp).
constexpr auto kPinnedWeight = static_cast<std::int64_t>(PinnedAssignmentsCost::kDefaultWeight);
constexpr auto kForbiddenWeight = static_cast<std::int64_t>(ForbiddenAssignmentsCost::kDefaultWeight);
constexpr auto kCapacityWeight = static_cast<std::int64_t>(LocationCapacityCost::kDefaultWeight);
constexpr auto kAssignmentWeight = static_cast<std::int64_t>(AssignmentCost::kDefaultWeight);
constexpr auto kFlowWeight = static_cast<std::int64_t>(FlowDistanceCost::kDefaultWeight);

}  // namespace

void QuadraticAssignmentIncrementalScoreCalculator::resetWorkingSolution(
    QuadraticAssignmentProblem &workingSolution)
{
    ScoreCalculator::prepare(workingSolution);
    problem_ = &workingSolution;
    const int n = workingSolution.n;
    location_.assign(static_cast<std::size_t>(n), -1);
    load_.assign(static_cast<std::size_t>(n), 0);
    forbidden_ = 0;
    overCapacity_ = 0;
    direct_ = 0;
    flow_ = 0;

    // Every fixed location starts violated (nothing stands anywhere yet);
    // each insert of its facility there repairs it.
    pinned_ = static_cast<std::int64_t>(workingSolution.fixedLocations.size());
    for (int f = 0; f < n; ++f)
    {
        const int l = workingSolution.facilities[f].location;
        if (l >= 0)
            insert(f, l);
    }
}

std::int64_t QuadraticAssignmentIncrementalScoreCalculator::capacityPenalty(int location) const
{
    if (!problem_->hasCapacity[location])
        return 0;
    const std::int64_t over = load_[location] - problem_->capacityOf[location];
    return over > 0 ? over : 0;
}

std::int64_t QuadraticAssignmentIncrementalScoreCalculator::flowContribution(int facility, int location) const
{
    const QuadraticAssignmentProblem &p = *problem_;
    const int n = p.n;
    const std::int64_t *flowRow = p.flow.data() + static_cast<std::size_t>(facility) * n;
    const std::int64_t *distanceRow = p.distance.data() + static_cast<std::size_t>(location) * n;
    std::int64_t sum = flowRow[facility] * distanceRow[location];
    for (int other = 0; other < n; ++other)
    {
        const int at = location_[other];
        if (other == facility || at < 0)
            continue;
        sum += flowRow[other] * distanceRow[at]
               + p.flow[static_cast<std::size_t>(other) * n + facility]
                     * p.distance[static_cast<std::size_t>(at) * n + location];
    }
    return sum;
}

// Adds facility `facility` at `location`.
void QuadraticAssignmentIncrementalScoreCalculator::insert(int facility, int location)
{
    const QuadraticAssignmentProblem &p = *problem_;
    location_[facility] = location;
    // hard 1: a pin of this facility on this location is now kept.
    for (int pinned : p.fixedLocationsOf[facility])
    {
        if (pinned == location)
            --pinned_;
    }
    // hard 2
    if (p.isForbidden(facility, location))
        ++forbidden_;
    // hard 3: replace this location's penalty term.
    overCapacity_ -= capacityPenalty(location);
    load_[location] += p.demandOf[facility];
    overCapacity_ += capacityPenalty(location);
    // soft 4
    direct_ += p.directCostOf(facility, location);
    // soft 5 (location_[facility] already set: the diagonal term is counted
    // once, the other facilities through the mirror)
    flow_ += flowContribution(facility, location);
}

// Exact mirror of insert.
void QuadraticAssignmentIncrementalScoreCalculator::retract(int facility, int location)
{
    const QuadraticAssignmentProblem &p = *problem_;
    flow_ -= flowContribution(facility, location);
    direct_ -= p.directCostOf(facility, location);
    overCapacity_ -= capacityPenalty(location);
    load_[location] -= p.demandOf[facility];
    overCapacity_ += capacityPenalty(location);
    if (p.isForbidden(facility, location))
        --forbidden_;
    for (int pinned : p.fixedLocationsOf[facility])
    {
        if (pinned == location)
            ++pinned_;
    }
    location_[facility] = -1;
}

void QuadraticAssignmentIncrementalScoreCalculator::beforeVariableChanged(int facilityIndex)
{
    const int old = location_[facilityIndex];
    if (old >= 0)
        retract(facilityIndex, old);  // unassigned: nothing to retract
}

void QuadraticAssignmentIncrementalScoreCalculator::afterVariableChanged(int facilityIndex)
{
    const int now = problem_->facilities[facilityIndex].location;  // the new value
    if (now >= 0)
        insert(facilityIndex, now);  // now unassigned: nothing to insert
}

Score QuadraticAssignmentIncrementalScoreCalculator::calculateScore()
{
    Score score;
    score.hard = -(kPinnedWeight * pinned_ + kForbiddenWeight * forbidden_ + kCapacityWeight * overCapacity_);
    // Negate in integer space so a zero penalty prints as 0, not -0.
    score.soft = static_cast<double>(-(kAssignmentWeight * direct_ + kFlowWeight * flow_));
    return score;
}

}  // namespace qap
