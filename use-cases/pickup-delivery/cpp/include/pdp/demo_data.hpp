#ifndef PDP_DEMO_DATA_HPP
#define PDP_DEMO_DATA_HPP

#include <cstdint>

#include "pdp/domain.hpp"

namespace pdp
{

inline Seconds hourMinute(int hour, int minute)
{
    return static_cast<Seconds>(hour) * 3600
           + static_cast<Seconds>(minute) * 60;
}

int addLocation(PdpRoutePlan &plan, double latitude, double longitude);

// A randomly generated PDPTW instance in a Firenze-like bounding box: each
// request is a pickup somewhere in the box and a dropoff elsewhere in it, with
// a shared time window wide enough that the instance is solvable but tight
// enough that sequencing matters.
PdpRoutePlan
buildRandomInstance(int requestCount, int vehicleCount, std::uint64_t seed);

}  // namespace pdp

#endif  // PDP_DEMO_DATA_HPP
