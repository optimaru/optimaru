#include "pdp/model/demo_data.hpp"

#include <random>
#include <string>
#include <utility>

#include "pdp/model/haversine.hpp"
#include "optimaru/utils/random.hpp"

namespace pdp {

int addLocation(PdpRoutePlan& plan, double latitude, double longitude) {
    const int index = static_cast<int>(plan.locations.size());
    plan.locations.push_back({latitude, longitude, index});
    return index;
}

// A uniform location in the demo's box, latitude drawn first: two statements,
// because the order a call's arguments are evaluated in is the compiler's.
int addRandomLocation(PdpRoutePlan& plan, std::mt19937_64& rng) {
    const double latitude = optimaru::random_real(rng, 43.751466, 43.809291);
    const double longitude = optimaru::random_real(rng, 11.177210, 11.290195);
    return addLocation(plan, latitude, longitude);
}

PdpRoutePlan buildRandomInstance(int requestCount, int vehicleCount, std::uint64_t seed) {
    PdpRoutePlan plan;
    plan.name = "random-pdp-demo";
    std::mt19937_64 rng(seed);
    const Seconds serviceDuration = 5 * 60;

    for (int v = 0; v < vehicleCount; ++v) {
        const int depot = addRandomLocation(plan, rng);
        Vehicle vehicle;
        vehicle.id = std::to_string(v + 1);
        vehicle.capacity = optimaru::random_int(rng, 8, 14);
        vehicle.startLocationIndex = depot;
        vehicle.departureTime = hourMinute(7, 30);
        plan.vehicles.push_back(std::move(vehicle));
    }

    for (int r = 0; r < requestCount; ++r) {
        const int pickupLocation = addRandomLocation(plan, rng);
        const int dropoffLocation = addRandomLocation(plan, rng);

        // Both stops of a request share a half-day window: the pickup may
        // happen any time in it, the dropoff must follow within the same one.
        const bool isMorning = optimaru::random_bool(rng, 0.5);
        const Seconds windowOpen = isMorning ? hourMinute(8, 0) : hourMinute(13, 0);
        const Seconds windowClose = isMorning ? hourMinute(12, 30) : hourMinute(18, 0);

        const int pickupIndex = static_cast<int>(plan.nodes.size());
        const int dropoffIndex = pickupIndex + 1;
        const int deliveryIndex = static_cast<int>(plan.deliveries.size());

        plan.nodes.push_back(Node{"p" + std::to_string(r + 1), pickupLocation, windowOpen,
                                  windowClose, serviceDuration, deliveryIndex, true, -1,
                                  NULL_TIME});
        plan.nodes.push_back(Node{"d" + std::to_string(r + 1), dropoffLocation, windowOpen,
                                  windowClose, serviceDuration, deliveryIndex, false, -1,
                                  NULL_TIME});
        plan.deliveries.push_back(Delivery{"r" + std::to_string(r + 1), pickupIndex, dropoffIndex,
                                           optimaru::random_int(rng, 1, 4)});
    }

    HaversineDrivingTimeCalculator::fillMissingMatrices(plan);
    return plan;
}

}  // namespace pdp
