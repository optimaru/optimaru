#include "pdp/score_calculator.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "pdp/constraints/max_end_time_cost.hpp"
#include "pdp/constraints/paired_request_cost.hpp"
#include "pdp/constraints/pickup_before_dropoff_cost.hpp"
#include "pdp/constraints/same_vehicle_cost.hpp"
#include "pdp/constraints/travel_time_cost.hpp"
#include "pdp/constraints/unassigned_requests_cost.hpp"
#include "pdp/constraints/vehicle_capacity_cost.hpp"

namespace pdp
{

namespace
{

// The monolithic score is the core of the default assembly, so it reads the
// same constants the components carry. Integral, so every level is computed
// in int64 (the medium weight is in medium units; toEngineScore folds it).
constexpr auto kCapacityWeight
    = static_cast<std::int64_t>(VehicleCapacityCost::kDefaultWeight);
constexpr auto kTimeWindowWeight
    = static_cast<std::int64_t>(MaxEndTimeCost::kDefaultWeight);
constexpr auto kPairedWeight
    = static_cast<std::int64_t>(PairedRequestCost::kDefaultWeight);
constexpr auto kSameVehicleWeight
    = static_cast<std::int64_t>(SameVehicleCost::kDefaultWeight);
constexpr auto kPrecedenceWeight
    = static_cast<std::int64_t>(PickupBeforeDropoffCost::kDefaultWeight);
constexpr auto kUnassignedWeight
    = static_cast<std::int64_t>(UnassignedRequestsCost::kDefaultWeight);
constexpr auto kTravelWeight
    = static_cast<std::int64_t>(TravelTimeCost::kDefaultWeight);
static_assert(kCapacityWeight == VehicleCapacityCost::kDefaultWeight
                  && kTimeWindowWeight == MaxEndTimeCost::kDefaultWeight
                  && kPairedWeight == PairedRequestCost::kDefaultWeight
                  && kSameVehicleWeight == SameVehicleCost::kDefaultWeight
                  && kPrecedenceWeight
                         == PickupBeforeDropoffCost::kDefaultWeight
                  && kUnassignedWeight == UnassignedRequestsCost::kDefaultWeight
                  && kTravelWeight == TravelTimeCost::kDefaultWeight,
              "the monolithic calculator assumes integral default weights");
static_assert(VehicleCapacityCost::kDefaultLevel == optimaru::Level::Hard
                  && MaxEndTimeCost::kDefaultLevel == optimaru::Level::Hard
                  && PairedRequestCost::kDefaultLevel == optimaru::Level::Hard
                  && SameVehicleCost::kDefaultLevel == optimaru::Level::Hard
                  && PickupBeforeDropoffCost::kDefaultLevel
                         == optimaru::Level::Hard
                  && UnassignedRequestsCost::kDefaultLevel
                         == optimaru::Level::Medium
                  && TravelTimeCost::kDefaultLevel == optimaru::Level::Soft,
              "the monolithic calculator assumes the components' levels");

}  // namespace

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

Score ScoreCalculator::score(PdpRoutePlan &plan)
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

    // --- Hard: vehicleCapacityCost, running load along each route ---
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
                hardPenalty += kCapacityWeight * (load - vehicle.capacity);
        }
    }

    // --- Hard: maxEndTimeCost, time windows ---
    for (const Node &node : plan.nodes)
    {
        if (node.vehicleIndex < 0)
            continue;
        const Seconds serviceFinished = node.arrivalTime + node.serviceDuration;
        if (serviceFinished > node.maxEndTime)
            hardPenalty += kTimeWindowWeight
                           * ceilToMinutes(serviceFinished - node.maxEndTime);
    }

    // --- Hard: request coupling (pairedRequestCost / sameVehicleCost /
    // pickupBeforeDropoffCost) ---
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
            hardPenalty += kPairedWeight;  // half-assigned request
            continue;
        }
        if (!pickupRouted)
        {
            // Both unassigned: a dropped request, penalized on medium below.
            continue;
        }
        if (pickup.vehicleIndex != dropoff.vehicleIndex)
        {
            hardPenalty += kSameVehicleWeight;  // split across two vehicles
            continue;
        }
        if (position[static_cast<std::size_t>(delivery.pickupNodeIndex)]
            > position[static_cast<std::size_t>(delivery.dropoffNodeIndex)])
        {
            hardPenalty += kPrecedenceWeight;  // dropoff before pickup
        }
    }
    score.hard = -hardPenalty;

    // --- Medium: unassignedRequestsCost, requests left unrouted ---
    for (const Delivery &delivery : plan.deliveries)
    {
        const Node &pickup
            = plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)];
        const Node &dropoff
            = plan.nodes[static_cast<std::size_t>(delivery.dropoffNodeIndex)];
        if (pickup.vehicleIndex < 0 && dropoff.vehicleIndex < 0)
        {
            score.medium
                -= kUnassignedWeight
                   * ((pickup.serviceDuration + dropoff.serviceDuration) / 60);
        }
    }

    // --- Soft: travelTimeCost, total driving time ---
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
        score.soft -= kTravelWeight * total;
    }

    plan.score = score;
    return score;
}

}  // namespace pdp
