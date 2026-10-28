#ifndef VRP_HAVERSINE_HPP
#define VRP_HAVERSINE_HPP

#include "vrp/domain.hpp"

namespace vrp {

// Port of org.acme.vehiclerouting.domain.geo.HaversineDrivingTimeCalculator.
//
// Driving time (seconds) between two locations = Haversine distance in meters
// at an average speed of AVERAGE_SPEED_KMPH.
class HaversineDrivingTimeCalculator {
public:
    static constexpr int AVERAGE_SPEED_KMPH = 50;
    static constexpr long EARTH_RADIUS_IN_M = 6371000;
    static constexpr long TWICE_EARTH_RADIUS_IN_M = 2 * EARTH_RADIUS_IN_M;

    // Driving time between two individual locations, in seconds.
    static Seconds calculateDrivingTime(const Location& from, const Location& to);

    // Fills plan.drivingSeconds with an N x N matrix over plan.locations,
    // mirroring DrivingTimeCalculator::initDrivingTimeMaps.
    static void initDrivingTimeMatrix(VehicleRoutePlan& plan);

    static Seconds metersToDrivingSeconds(long meters);
};

}  // namespace vrp

#endif  // VRP_HAVERSINE_HPP
