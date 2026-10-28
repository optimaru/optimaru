#include "vrp/score_calculator.hpp"

#include <algorithm>

namespace vrp {

std::int64_t ScoreCalculator::ceilToMinutes(Seconds seconds) {
    if (seconds <= 0) return 0;
    return (seconds + 59) / 60;
}

void ScoreCalculator::updateShadowVariables(VehicleRoutePlan& plan) {
    // Reset all visits to unassigned (vehicle == null, arrivalTime == null).
    for (Visit& visit : plan.visits) {
        visit.vehicleIndex = -1;
        visit.arrivalTime = NULL_TIME;
    }

    // Walk each route, propagating arrival times exactly like the Java
    // arrivalTimeSupplier / getDepartureTime chain:
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

Score ScoreCalculator::calculateScore(VehicleRoutePlan& plan) {
    updateShadowVariables(plan);

    Score score;

    // --- Hard: vehicleCapacity ---
    // penalize (totalDemand - capacity) when a vehicle is overloaded.
    for (const Vehicle& vehicle : plan.vehicles) {
        int totalDemand = 0;
        for (int visitIdx : vehicle.visitIndices) {
            totalDemand += plan.visits[visitIdx].demand;
        }
        if (totalDemand > vehicle.capacity) {
            score.hard -= (totalDemand - vehicle.capacity);
        }
    }

    // --- Hard: serviceFinishedAfterMaxEndTime ---
    // A visit is late when arrivalTime + serviceDuration is after maxEndTime;
    // penalize by the overshoot rounded up to whole minutes. (Faithful to the
    // Java quickstart, which measures from arrivalTime, not startServiceTime.)
    for (const Visit& visit : plan.visits) {
        if (visit.vehicleIndex < 0) continue;  // unassigned: no arrival time
        Seconds serviceFinished = visit.arrivalTime + visit.serviceDuration;
        if (serviceFinished > visit.maxEndTime) {
            score.hard -= ceilToMinutes(serviceFinished - visit.maxEndTime);
        }
    }

    // --- Medium: maximizeVisitsAssigned ---
    // penalize each unassigned visit by its unassigned penalty (by default
    // its service duration in minutes -- see Visit::unassignedPenalty).
    for (const Visit& visit : plan.visits) {
        if (visit.vehicleIndex < 0) {
            score.medium -= visit.unassignedPenalty();
        }
    }

    // --- Soft: minimizeTravelTime ---
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
        score.soft -= total;
    }

    plan.score = score;
    return score;
}

}  // namespace vrp
