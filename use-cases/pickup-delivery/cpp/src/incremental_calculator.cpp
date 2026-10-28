#include "pdp/incremental_calculator.hpp"

#include <algorithm>

#include "pdp/score_calculator.hpp"

namespace pdp
{

void PdpIncrementalScoreCalculator::resetWorkingSolution(
    PdpRoutePlan &workingSolution)
{
    plan_ = &workingSolution;
    vehicleCount_ = static_cast<int>(plan_->vehicles.size());
    const std::size_t nodeCount = plan_->nodes.size();
    const std::size_t deliveryCount = plan_->deliveries.size();

    // Rebuild the node index. nodeList_ holds the TRUE list index, pool
    // included — not "-1 for unassigned". Ownership has to be recorded
    // literally so afterListChanged can tell "this node left my list" from
    // "this node was already re-homed by an earlier hook pair" (see there).
    // routeOf() is what translates an index back into a vehicle or "unrouted".
    nodeList_.assign(nodeCount, -1);
    nodePos_.assign(nodeCount, -1);
    for (int l = 0; l <= vehicleCount_; ++l)
    {
        const std::vector<int> &members
            = optimaru::SolutionDescriptor<PdpRoutePlan>::listOf(*plan_, l);
        for (std::size_t p = 0; p < members.size(); ++p)
        {
            nodeList_[static_cast<std::size_t>(members[p])] = l;
            nodePos_[static_cast<std::size_t>(members[p])]
                = static_cast<int>(p);
        }
    }

    terms_.assign(static_cast<std::size_t>(vehicleCount_), RouteTerms{});
    capacityTotal_ = 0;
    latenessTotal_ = 0;
    drivingTotal_ = 0;
    for (int v = 0; v < vehicleCount_; ++v)
    {
        terms_[static_cast<std::size_t>(v)] = evaluateRoute(v);
        const RouteTerms &t = terms_[static_cast<std::size_t>(v)];
        capacityTotal_ += t.capacityExcess;
        latenessTotal_ += t.lateness;
        drivingTotal_ += t.driving;
    }

    pairPenalty_.assign(deliveryCount, 0);
    unassignedPenalty_.assign(deliveryCount, 0);
    couplingTotal_ = 0;
    unassignedTotal_ = 0;
    for (std::size_t d = 0; d < deliveryCount; ++d)
    {
        pairPenalty_[d] = couplingPenalty(static_cast<int>(d));
        unassignedPenalty_[d] = unassignedMinutes(static_cast<int>(d));
        couplingTotal_ += pairPenalty_[d];
        unassignedTotal_ += unassignedPenalty_[d];
    }

    pendingMark_.assign(deliveryCount, 0);
    pendingDeliveries_.clear();
    oldMembers_.clear();
}

void PdpIncrementalScoreCalculator::markDelivery(int deliveryIndex)
{
    const auto d = static_cast<std::size_t>(deliveryIndex);
    if (pendingMark_[d])
        return;
    pendingMark_[d] = 1;
    pendingDeliveries_.push_back(deliveryIndex);
    couplingTotal_ -= pairPenalty_[d];
    unassignedTotal_ -= unassignedPenalty_[d];
}

void PdpIncrementalScoreCalculator::flushDeliveries()
{
    for (int deliveryIndex : pendingDeliveries_)
    {
        const auto d = static_cast<std::size_t>(deliveryIndex);
        pairPenalty_[d] = couplingPenalty(deliveryIndex);
        unassignedPenalty_[d] = unassignedMinutes(deliveryIndex);
        couplingTotal_ += pairPenalty_[d];
        unassignedTotal_ += unassignedPenalty_[d];
        pendingMark_[d] = 0;
    }
    pendingDeliveries_.clear();
}

void PdpIncrementalScoreCalculator::beforeListChanged(int listIndex)
{
    using Descriptor = optimaru::SolutionDescriptor<PdpRoutePlan>;

    // Snapshot the old membership so afterListChanged can tell which nodes
    // left the list (they become unassigned) from which merely moved within it.
    const std::vector<int> &members = Descriptor::listOf(*plan_, listIndex);
    oldMembers_.assign(members.begin(), members.end());

    for (int nodeIndex : oldMembers_)
        markDelivery(
            plan_->nodes[static_cast<std::size_t>(nodeIndex)].deliveryIndex);

    if (listIndex < vehicleCount_)
    {
        const RouteTerms &t = terms_[static_cast<std::size_t>(listIndex)];
        capacityTotal_ -= t.capacityExcess;
        latenessTotal_ -= t.lateness;
        drivingTotal_ -= t.driving;
    }
}

void PdpIncrementalScoreCalculator::afterListChanged(int listIndex)
{
    using Descriptor = optimaru::SolutionDescriptor<PdpRoutePlan>;

    // Release the nodes that were here — but ONLY those this list still owns.
    // A composite move (the engine's cross-list element swap, or the pair move
    // below) re-homes an element with the *destination* list's hook pair,
    // which runs before the source list's. By then nodeList_ already points at
    // the destination, and blindly clearing it here would strand a node that
    // is in fact routed. Checking ownership first is what keeps the index
    // truthful through every move shape.
    for (int nodeIndex : oldMembers_)
    {
        const auto n = static_cast<std::size_t>(nodeIndex);
        if (nodeList_[n] != listIndex)
            continue;  // already re-homed by an earlier hook pair
        nodeList_[n] = -1;
        nodePos_[n] = -1;
    }
    oldMembers_.clear();

    const std::vector<int> &members = Descriptor::listOf(*plan_, listIndex);
    for (std::size_t p = 0; p < members.size(); ++p)
    {
        const auto n = static_cast<std::size_t>(members[p]);
        nodeList_[n] = listIndex;
        nodePos_[n] = static_cast<int>(p);
        markDelivery(plan_->nodes[n].deliveryIndex);
    }

    if (listIndex < vehicleCount_)
    {
        RouteTerms t = evaluateRoute(listIndex);
        capacityTotal_ += t.capacityExcess;
        latenessTotal_ += t.lateness;
        drivingTotal_ += t.driving;
        terms_[static_cast<std::size_t>(listIndex)] = t;
    }

    flushDeliveries();
}

// One walk of the route derives all three per-vehicle terms.
PdpIncrementalScoreCalculator::RouteTerms
PdpIncrementalScoreCalculator::evaluateRoute(int vehicleIndex) const
{
    const Vehicle &vehicle
        = plan_->vehicles[static_cast<std::size_t>(vehicleIndex)];
    RouteTerms terms;
    if (vehicle.nodeIndices.empty())
        return terms;

    std::int64_t load = 0;
    Seconds previousDeparture = vehicle.departureTime;
    int previousLocation = vehicle.homeLocationIndex;

    for (int nodeIndex : vehicle.nodeIndices)
    {
        const Node &node = plan_->nodes[static_cast<std::size_t>(nodeIndex)];
        const Delivery &delivery
            = plan_->deliveries[static_cast<std::size_t>(node.deliveryIndex)];

        load += node.pickup ? delivery.demand : -delivery.demand;
        if (load > vehicle.capacity)
            terms.capacityExcess += load - vehicle.capacity;

        const Seconds drive
            = plan_->drivingTime(previousLocation, node.locationIndex);
        terms.driving += drive;

        const Seconds arrival = previousDeparture + drive;
        const Seconds serviceFinished = arrival + node.serviceDuration;
        if (serviceFinished > node.maxEndTime)
        {
            terms.lateness += ScoreCalculator::ceilToMinutes(serviceFinished
                                                             - node.maxEndTime);
        }

        previousDeparture
            = std::max(arrival, node.minStartTime) + node.serviceDuration;
        previousLocation = node.locationIndex;
    }
    terms.driving += vehicle.homeLeg(plan_->drivingSeconds, previousLocation);
    return terms;
}

std::int64_t
PdpIncrementalScoreCalculator::couplingPenalty(int deliveryIndex) const
{
    const Delivery &delivery
        = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
    const int pickupRoute = routeOf(delivery.pickupNodeIndex);
    const int dropoffRoute = routeOf(delivery.dropoffNodeIndex);

    if ((pickupRoute >= 0) != (dropoffRoute >= 0))
        return ScoreCalculator::kCouplingPenalty;  // half-assigned
    if (pickupRoute < 0)
        return 0;  // both unrouted: a dropped request (medium level)
    if (pickupRoute != dropoffRoute)
        return ScoreCalculator::kCouplingPenalty;  // split across vehicles
    if (nodePos_[static_cast<std::size_t>(delivery.pickupNodeIndex)]
        > nodePos_[static_cast<std::size_t>(delivery.dropoffNodeIndex)])
        return ScoreCalculator::kCouplingPenalty;  // dropoff before pickup
    return 0;
}

std::int64_t
PdpIncrementalScoreCalculator::unassignedMinutes(int deliveryIndex) const
{
    const Delivery &delivery
        = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
    if (routeOf(delivery.pickupNodeIndex) >= 0
        || routeOf(delivery.dropoffNodeIndex) >= 0)
        return 0;
    const Node &pickup
        = plan_->nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)];
    const Node &dropoff
        = plan_->nodes[static_cast<std::size_t>(delivery.dropoffNodeIndex)];
    return (pickup.serviceDuration + dropoff.serviceDuration) / 60;
}

Score PdpIncrementalScoreCalculator::currentScore() const
{
    return Score{-(capacityTotal_ + latenessTotal_ + couplingTotal_),
                 -unassignedTotal_,
                 -drivingTotal_};
}

optimaru::HardSoftScore PdpIncrementalScoreCalculator::calculateScore()
{
    return toEngineScore(currentScore());
}

}  // namespace pdp
