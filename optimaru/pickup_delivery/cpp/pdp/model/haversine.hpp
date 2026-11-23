#ifndef PDP_HAVERSINE_HPP
#define PDP_HAVERSINE_HPP

#include "pdp/model/domain.hpp"

namespace pdp {

// The same straight-line fallback as the vehicle-routing use case's
// HaversineDrivingTimeCalculator: driving meters = the haversine distance,
// driving seconds = those meters at a fixed average speed. Duplicated rather
// than shared so the two use cases stay independently buildable, exactly like
// every other pair of use cases in this repo.
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
    static void fillMissingMatrices(PdpRoutePlan& plan);

    static Seconds metersToDrivingSeconds(long meters);
};

}  // namespace pdp

#endif  // PDP_HAVERSINE_HPP
