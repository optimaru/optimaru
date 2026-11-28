#include "vrp/model/demo_data.hpp"

#include <random>
#include <string>
#include <utility>

#include "vrp/model/haversine.hpp"
#include "optimaru/utils/random.hpp"

namespace vrp {

int addLocation(VehicleRoutePlan& plan, double latitude, double longitude) {
    const int index = static_cast<int>(plan.locations.size());
    plan.locations.push_back({latitude, longitude, index});
    return index;
}

// A uniform location in the demo's box, latitude drawn first: two statements,
// because the order a call's arguments are evaluated in is the compiler's.
int addRandomLocation(VehicleRoutePlan& plan, std::mt19937_64& rng) {
    const double latitude = optimaru::random_real(rng, 43.751466, 43.809291);
    const double longitude = optimaru::random_real(rng, 11.177210, 11.290195);
    return addLocation(plan, latitude, longitude);
}

VehicleRoutePlan buildFixturePlan() {
    VehicleRoutePlan plan;
    plan.name = "fixture";
    addLocation(plan, 49.288087, 16.562172);                 // LOCATION_1 (depot)
    addLocation(plan, 49.190922, 16.624466);                 // LOCATION_2
    addLocation(plan, 49.1767533245638, 16.50422914190477);  // LOCATION_3
    HaversineDrivingTimeCalculator::fillMissingMatrices(plan);
    return plan;
}

VehicleRoutePlan buildRandomInstance(int visitCount, int vehicleCount, std::uint64_t seed) {
    VehicleRoutePlan plan;
    plan.name = "random-demo";
    std::mt19937_64 rng(seed);
    // Bounding box around Firenze.
    const int serviceMinutes[] = {10, 20, 30, 40};

    for (int v = 0; v < vehicleCount; ++v) {
        const int depot = addRandomLocation(plan, rng);
        Vehicle vehicle;
        vehicle.id = std::to_string(v + 1);
        vehicle.capacity = optimaru::random_int(rng, 20, 40);
        vehicle.startLocationIndex = depot;
        vehicle.departureTime = hourMinute(7, 30);
        plan.vehicles.push_back(std::move(vehicle));
    }
    for (int i = 0; i < visitCount; ++i) {
        const int location = addRandomLocation(plan, rng);
        const bool isMorning = optimaru::random_bool(rng, 0.5);
        const Seconds minStart = isMorning ? hourMinute(8, 0) : hourMinute(13, 0);
        const Seconds maxEnd = isMorning ? hourMinute(12, 0) : hourMinute(18, 0);
        // The fields past the service time keep their defaults (automatic
        // unassigned penalty, no group, no order).
        plan.visits.push_back(Visit{std::to_string(i + 1), "visit" + std::to_string(i + 1),
                                    location, optimaru::random_int(rng, 1, 2), minStart, maxEnd,
                                    serviceMinutes[optimaru::random_int(rng, 0, 3)] * 60LL});
    }
    HaversineDrivingTimeCalculator::fillMissingMatrices(plan);
    return plan;
}

}  // namespace vrp
