#ifndef VRP_DEMO_DATA_HPP
#define VRP_DEMO_DATA_HPP

#include <cstdint>

#include "vrp/domain.hpp"

namespace vrp
{

// Seconds from midnight, so "07:30" == hourMinute(7, 30).
inline Seconds hourMinute(int hour, int minute)
{
    return static_cast<Seconds>(hour) * 3600 + static_cast<Seconds>(minute) * 60;
}

// Registers a location in the plan and returns its assigned index.
int addLocation(VehicleRoutePlan &plan, double latitude, double longitude);

// The three Brno locations from the Java VehicleRoutingConstraintProviderTest,
// with the driving-time matrix already initialized. The test fixtures assert
// the documented driving times 843 / 639 / 941 seconds against it.
VehicleRoutePlan buildJavaFixturePlan();

// A randomly generated instance in a Firenze-like bounding box (the Java
// DemoData FIRENZE preset's area), used by the demo and the benchmarks.
VehicleRoutePlan buildRandomInstance(int visitCount, int vehicleCount,
                                     std::uint64_t seed);

}  // namespace vrp

#endif  // VRP_DEMO_DATA_HPP
