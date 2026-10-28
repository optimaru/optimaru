#include "pdp/demo_data.hpp"

#include <random>
#include <string>

#include "pdp/haversine.hpp"

namespace pdp
{

int addLocation(PdpRoutePlan &plan, double latitude, double longitude)
{
    const int index = static_cast<int>(plan.locations.size());
    plan.locations.push_back({latitude, longitude, index});
    return index;
}

PdpRoutePlan
buildRandomInstance(int requestCount, int vehicleCount, std::uint64_t seed)
{
    PdpRoutePlan plan;
    plan.name = "random-pdp-demo";
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> latitude(43.751466, 43.809291);
    std::uniform_real_distribution<double> longitude(11.177210, 11.290195);
    std::uniform_int_distribution<int> demand(1, 4);
    std::uniform_int_distribution<int> capacity(8, 14);
    std::bernoulli_distribution morning(0.5);
    const Seconds serviceDuration = 5 * 60;

    for (int v = 0; v < vehicleCount; ++v)
    {
        const int home = addLocation(plan, latitude(rng), longitude(rng));
        plan.vehicles.push_back(Vehicle{
            std::to_string(v + 1), capacity(rng), home, hourMinute(7, 30), {}});
    }

    for (int r = 0; r < requestCount; ++r)
    {
        const int pickupLocation
            = addLocation(plan, latitude(rng), longitude(rng));
        const int dropoffLocation
            = addLocation(plan, latitude(rng), longitude(rng));

        // Both stops of a request share a half-day window: the pickup may
        // happen any time in it, the dropoff must follow within the same one.
        const bool isMorning = morning(rng);
        const Seconds windowOpen
            = isMorning ? hourMinute(8, 0) : hourMinute(13, 0);
        const Seconds windowClose
            = isMorning ? hourMinute(12, 30) : hourMinute(18, 0);

        const int pickupIndex = static_cast<int>(plan.nodes.size());
        const int dropoffIndex = pickupIndex + 1;
        const int deliveryIndex = static_cast<int>(plan.deliveries.size());

        plan.nodes.push_back(Node{"p" + std::to_string(r + 1),
                                  pickupLocation,
                                  windowOpen,
                                  windowClose,
                                  serviceDuration,
                                  deliveryIndex,
                                  true,
                                  -1,
                                  NULL_TIME});
        plan.nodes.push_back(Node{"d" + std::to_string(r + 1),
                                  dropoffLocation,
                                  windowOpen,
                                  windowClose,
                                  serviceDuration,
                                  deliveryIndex,
                                  false,
                                  -1,
                                  NULL_TIME});
        plan.deliveries.push_back(Delivery{"r" + std::to_string(r + 1),
                                           pickupIndex,
                                           dropoffIndex,
                                           demand(rng)});
    }

    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan);
    return plan;
}

}  // namespace pdp
