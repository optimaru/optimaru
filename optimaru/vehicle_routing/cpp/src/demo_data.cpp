#include "vrp/demo_data.hpp"

#include <random>
#include <string>

#include "vrp/haversine.hpp"

namespace vrp
{

int addLocation(VehicleRoutePlan &plan, double latitude, double longitude)
{
    const int index = static_cast<int>(plan.locations.size());
    plan.locations.push_back({latitude, longitude, index});
    return index;
}

VehicleRoutePlan buildJavaFixturePlan()
{
    VehicleRoutePlan plan;
    plan.name = "java-fixture";
    addLocation(plan, 49.288087, 16.562172);              // LOCATION_1 (home)
    addLocation(plan, 49.190922, 16.624466);              // LOCATION_2
    addLocation(plan, 49.1767533245638, 16.50422914190477);  // LOCATION_3
    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan);
    return plan;
}

VehicleRoutePlan
buildRandomInstance(int visitCount, int vehicleCount, std::uint64_t seed)
{
    VehicleRoutePlan plan;
    plan.name = "random-demo";
    std::mt19937_64 rng(seed);
    // Bounding box around Firenze, like the Java DemoData FIRENZE preset.
    std::uniform_real_distribution<double> latitude(43.751466, 43.809291);
    std::uniform_real_distribution<double> longitude(11.177210, 11.290195);
    std::uniform_int_distribution<int> demand(1, 2);
    std::uniform_int_distribution<int> capacity(20, 40);
    std::uniform_int_distribution<int> service(0, 3);
    std::bernoulli_distribution morning(0.5);
    const int serviceMinutes[] = {10, 20, 30, 40};

    for (int v = 0; v < vehicleCount; ++v)
    {
        const int home = addLocation(plan, latitude(rng), longitude(rng));
        plan.vehicles.push_back(Vehicle{
            std::to_string(v + 1), capacity(rng), home, hourMinute(7, 30), {}});
    }
    for (int i = 0; i < visitCount; ++i)
    {
        const int location = addLocation(plan, latitude(rng), longitude(rng));
        const bool isMorning = morning(rng);
        const Seconds minStart = isMorning ? hourMinute(8, 0) : hourMinute(13, 0);
        const Seconds maxEnd = isMorning ? hourMinute(12, 0) : hourMinute(18, 0);
        plan.visits.push_back(Visit{std::to_string(i + 1),
                                    "visit" + std::to_string(i + 1),
                                    location,
                                    demand(rng),
                                    minStart,
                                    maxEnd,
                                    serviceMinutes[service(rng)] * 60LL,
                                    -1,
                                    NULL_TIME});
    }
    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan);
    return plan;
}

}  // namespace vrp
