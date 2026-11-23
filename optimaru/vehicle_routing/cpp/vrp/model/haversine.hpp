#ifndef VRP_HAVERSINE_HPP
#define VRP_HAVERSINE_HPP

#include "vrp/model/domain.hpp"

namespace vrp {

// The straight-line fallback for a plan given only coordinates: driving meters
// = the Haversine distance, driving seconds = those meters at an average speed
// of AVERAGE_SPEED_KMPH.
class HaversineDrivingTimeCalculator {
public:
    static constexpr int AVERAGE_SPEED_KMPH = 50;
    static constexpr long EARTH_RADIUS_IN_M = 6371000;
    static constexpr long TWICE_EARTH_RADIUS_IN_M = 2 * EARTH_RADIUS_IN_M;

    // Driving distance and time between two individual locations.
    static Meters calculateDistance(const Location& from, const Location& to);
    static Seconds calculateDrivingTime(const Location& from, const Location& to);

    // Gives the plan as many profiles as its longer matrix list holds (one at
    // least), and fills every matrix left empty from the coordinates. Matrices
    // already there are kept as they are.
    static void fillMissingMatrices(VehicleRoutePlan& plan);

    static Seconds metersToDrivingSeconds(long meters);
};

}  // namespace vrp

#endif  // VRP_HAVERSINE_HPP
