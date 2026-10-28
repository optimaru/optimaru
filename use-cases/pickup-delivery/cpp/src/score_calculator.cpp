#include "pdp/score_calculator.hpp"

#include <algorithm>
#include <vector>

namespace pdp
{

void ScoreCalculator::updateShadowVariables(PdpRoutePlan &plan)
{
    for (Node &node : plan.nodes)
    {
        node.vehicleIndex = -1;
        node.arrivalTime = NULL_TIME;
    }

    for (std::size_t v = 0; v < plan.vehicles.size(); ++v)
    {
        const Vehicle &vehicle = plan.vehicles[v];
        Seconds previousDeparture = vehicle.departureTime;
        int previousLocation = vehicle.homeLocationIndex;

        for (int nodeIndex : vehicle.nodeIndices)
        {
            Node &node = plan.nodes[static_cast<std::size_t>(nodeIndex)];
            node.vehicleIndex = static_cast<int>(v);

            const Seconds arrival
                = previousDeparture
                  + plan.drivingTime(previousLocation, node.locationIndex);
            node.arrivalTime = arrival;

            previousDeparture
                = std::max(arrival, node.minStartTime) + node.serviceDuration;
            previousLocation = node.locationIndex;
        }
    }
}

Score ScoreCalculator::calculateScore(PdpRoutePlan &plan)
{
    updateShadowVariables(plan);

    Score score;
    std::int64_t hardPenalty = 0;

    // Position of each node within its route, for the precedence check.
    std::vector<int> position(plan.nodes.size(), -1);
    for (const Vehicle &vehicle : plan.vehicles)
    {
        for (std::size_t p = 0; p < vehicle.nodeIndices.size(); ++p)
            position[static_cast<std::size_t>(vehicle.nodeIndices[p])]
                = static_cast<int>(p);
    }

    // --- Hard: capacity, running load along each route ---
    // Load rises at a pickup and falls at the matching dropoff, so what must
    // fit is the peak in between — not the route total.
    for (const Vehicle &vehicle : plan.vehicles)
    {
        std::int64_t load = 0;
        for (int nodeIndex : vehicle.nodeIndices)
        {
            const Node &node = plan.nodes[static_cast<std::size_t>(nodeIndex)];
            const Delivery &delivery
                = plan.deliveries[static_cast<std::size_t>(node.deliveryIndex)];
            load += node.pickup ? delivery.demand : -delivery.demand;
            if (load > vehicle.capacity)
                hardPenalty += load - vehicle.capacity;
        }
    }

    // --- Hard: time windows ---
    for (const Node &node : plan.nodes)
    {
        if (node.vehicleIndex < 0)
            continue;
        const Seconds serviceFinished = node.arrivalTime + node.serviceDuration;
        if (serviceFinished > node.maxEndTime)
            hardPenalty += ceilToMinutes(serviceFinished - node.maxEndTime);
    }

    // --- Hard: request coupling (paired / same vehicle / pickup first) ---
    for (const Delivery &delivery : plan.deliveries)
    {
        const Node &pickup
            = plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)];
        const Node &dropoff
            = plan.nodes[static_cast<std::size_t>(delivery.dropoffNodeIndex)];

        const bool pickupRouted = pickup.vehicleIndex >= 0;
        const bool dropoffRouted = dropoff.vehicleIndex >= 0;
        if (pickupRouted != dropoffRouted)
        {
            hardPenalty += kCouplingPenalty;  // half-assigned request
            continue;
        }
        if (!pickupRouted)
        {
            // Both unassigned: a dropped request, penalized on medium below.
            continue;
        }
        if (pickup.vehicleIndex != dropoff.vehicleIndex)
        {
            hardPenalty += kCouplingPenalty;  // split across two vehicles
            continue;
        }
        if (position[static_cast<std::size_t>(delivery.pickupNodeIndex)]
            > position[static_cast<std::size_t>(delivery.dropoffNodeIndex)])
        {
            hardPenalty += kCouplingPenalty;  // dropoff scheduled before pickup
        }
    }
    score.hard = -hardPenalty;

    // --- Medium: requests left unrouted ---
    for (const Delivery &delivery : plan.deliveries)
    {
        const Node &pickup
            = plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)];
        const Node &dropoff
            = plan.nodes[static_cast<std::size_t>(delivery.dropoffNodeIndex)];
        if (pickup.vehicleIndex < 0 && dropoff.vehicleIndex < 0)
        {
            score.medium
                -= (pickup.serviceDuration + dropoff.serviceDuration) / 60;
        }
    }

    // --- Soft: total driving time ---
    // home -> stops..., plus the leg back home when the vehicle returns to
    // its depot.
    for (const Vehicle &vehicle : plan.vehicles)
    {
        if (vehicle.nodeIndices.empty())
            continue;
        Seconds total = 0;
        int previousLocation = vehicle.homeLocationIndex;
        for (int nodeIndex : vehicle.nodeIndices)
        {
            const int location
                = plan.nodes[static_cast<std::size_t>(nodeIndex)].locationIndex;
            total += plan.drivingTime(previousLocation, location);
            previousLocation = location;
        }
        total += vehicle.homeLeg(plan.drivingSeconds, previousLocation);
        score.soft -= total;
    }

    plan.score = score;
    return score;
}

}  // namespace pdp
