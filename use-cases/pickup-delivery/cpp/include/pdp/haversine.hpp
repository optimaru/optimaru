#ifndef PDP_HAVERSINE_HPP
#define PDP_HAVERSINE_HPP

#include "pdp/domain.hpp"

namespace pdp
{

// The same driving-time model as the vehicle-routing port's
// HaversineDrivingTimeCalculator (a port of
// org.acme.vehiclerouting.domain.geo.HaversineDrivingTimeCalculator):
// haversine distance in meters at a fixed average speed. Duplicated rather
// than shared so the two use cases stay independently buildable, exactly like
// every other pair of quickstarts in this repo.
class HaversineDrivingTimeCalculator
{
public:
    static constexpr int AVERAGE_SPEED_KMPH = 50;
    static constexpr long EARTH_RADIUS_IN_M = 6371000;
    static constexpr long TWICE_EARTH_RADIUS_IN_M = 2 * EARTH_RADIUS_IN_M;

    static Seconds calculateDrivingTime(const Location &from,
                                        const Location &to);
    static void initDrivingTimeMatrix(PdpRoutePlan &plan);
    static Seconds metersToDrivingSeconds(long meters);
};

}  // namespace pdp

#endif  // PDP_HAVERSINE_HPP
