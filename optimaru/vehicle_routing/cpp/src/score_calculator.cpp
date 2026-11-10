#include "vrp/score_calculator.hpp"

#include <algorithm>
#include <cstdint>

#include "vrp/constraints/max_end_time_cost.hpp"
#include "vrp/constraints/travel_time_cost.hpp"
#include "vrp/constraints/unassigned_visits_cost.hpp"
#include "vrp/constraints/vehicle_capacity_cost.hpp"

namespace vrp {

namespace {

// The monolithic score is the core of the default assembly, so it reads the
// same constants the components carry. Integral, so every level is computed
// in int64 (the medium weight is in medium units; toEngineScore folds it).
constexpr auto kCapacityWeight = static_cast<std::int64_t>(VehicleCapacityCost::kDefaultWeight);
constexpr auto kTimeWindowWeight = static_cast<std::int64_t>(MaxEndTimeCost::kDefaultWeight);
constexpr auto kUnassignedWeight = static_cast<std::int64_t>(UnassignedVisitsCost::kDefaultWeight);
constexpr auto kTravelWeight = static_cast<std::int64_t>(TravelTimeCost::kDefaultWeight);
static_assert(kCapacityWeight == VehicleCapacityCost::kDefaultWeight &&
                      kTimeWindowWeight == MaxEndTimeCost::kDefaultWeight &&
                      kUnassignedWeight == UnassignedVisitsCost::kDefaultWeight &&
                      kTravelWeight == TravelTimeCost::kDefaultWeight,
              "the monolithic calculator assumes integral default weights");
static_assert(VehicleCapacityCost::kDefaultLevel == optimaru::Level::Hard &&
                      MaxEndTimeCost::kDefaultLevel == optimaru::Level::Hard &&
                      UnassignedVisitsCost::kDefaultLevel == optimaru::Level::Medium &&
                      TravelTimeCost::kDefaultLevel == optimaru::Level::Soft,
              "the monolithic calculator assumes the components' levels");

}  // namespace

std::int64_t ScoreCalculator::ceilToMinutes(Seconds seconds) {
    if (seconds <= 0) return 0;
    return (seconds + 59) / 60;
}

void ScoreCalculator::updateShadowVariables(VehicleRoutePlan& plan) {
    // Reset all visits to unassigned (no vehicle, no arrival time).
    for (Visit& visit : plan.visits) {
        visit.vehicleIndex = -1;
        visit.arrivalTime = NULL_TIME;
    }

    // Walk each route, propagating arrival times along the chain:
    //   arrival        = previousDeparture + drivingTime(prevLoc -> loc)
    //   startService   = max(arrival, minStartTime)
    //   departure      = startService + serviceDuration
    for (std::size_t v = 0; v < plan.vehicles.size(); ++v) {
        const Vehicle& vehicle = plan.vehicles[v];
        Seconds prevDeparture = vehicle.departureTime;
        int prevLocation = vehicle.homeLocationIndex;

        for (int visitIdx : vehicle.visitIndices) {
            Visit& visit = plan.visits[visitIdx];
            visit.vehicleIndex = static_cast<int>(v);

            Seconds arrival = prevDeparture + plan.drivingTime(prevLocation, visit.locationIndex);
            visit.arrivalTime = arrival;

            Seconds startService = std::max(arrival, visit.minStartTime);
            Seconds departure = startService + visit.serviceDuration;

            prevDeparture = departure;
            prevLocation = visit.locationIndex;
        }
    }
}

Score ScoreCalculator::score(VehicleRoutePlan& plan) {
    updateShadowVariables(plan);

    Score score;

    // --- Hard: vehicleCapacityCost ---
    // penalize (totalDemand - capacity) when a vehicle is overloaded.
    for (const Vehicle& vehicle : plan.vehicles) {
        int totalDemand = 0;
        for (int visitIdx : vehicle.visitIndices) {
            totalDemand += plan.visits[visitIdx].demand;
        }
        if (totalDemand > vehicle.capacity) {
            score.hard -= kCapacityWeight * (totalDemand - vehicle.capacity);
        }
    }

    // --- Hard: maxEndTimeCost ---
    // A visit is late when arrivalTime + serviceDuration is after maxEndTime;
    // penalize by the overshoot rounded up to whole minutes. (Measured
    // from arrivalTime, not the start of service.)
    for (const Visit& visit : plan.visits) {
        if (visit.vehicleIndex < 0) continue;  // unassigned: no arrival time
        Seconds serviceFinished = visit.arrivalTime + visit.serviceDuration;
        if (serviceFinished > visit.maxEndTime) {
            score.hard -= kTimeWindowWeight * ceilToMinutes(serviceFinished - visit.maxEndTime);
        }
    }

    // --- Medium: unassignedVisitsCost ---
    // penalize each unassigned visit by its unassigned penalty (by default
    // its service duration in minutes -- see Visit::unassignedPenalty), in
    // medium units: toEngineScore folds them into soft x 10^9.
    for (const Visit& visit : plan.visits) {
        if (visit.vehicleIndex < 0) {
            score.medium -= kUnassignedWeight * visit.unassignedPenalty();
        }
    }

    // --- Soft: travelTimeCost ---
    // penalize each vehicle's total driving time: home -> visits..., plus the
    // leg back home when the vehicle returns to its depot.
    for (const Vehicle& vehicle : plan.vehicles) {
        if (vehicle.visitIndices.empty()) continue;
        Seconds total = 0;
        int prevLocation = vehicle.homeLocationIndex;
        for (int visitIdx : vehicle.visitIndices) {
            int loc = plan.visits[visitIdx].locationIndex;
            total += plan.drivingTime(prevLocation, loc);
            prevLocation = loc;
        }
        total += vehicle.homeLeg(plan.drivingSeconds, prevLocation);
        score.soft -= kTravelWeight * total;
    }

    plan.score = score;
    return score;
}

}  // namespace vrp
