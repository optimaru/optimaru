#include "pdp/haversine.hpp"

#include <cmath>

namespace pdp
{

namespace
{

struct CartesianCoordinate
{
    double x, y, z;
};

// Same value as POSIX M_PI, which MSVC's <cmath> does not define by default.
constexpr double kPi = 3.14159265358979323846;

double toRadians(double degrees) { return degrees * kPi / 180.0; }

// Java's Math.round(double): floor(x + 0.5), returned as long. All inputs here
// are non-negative, for which std::llround (round-half-away-from-zero) agrees.
long javaRound(double value) { return static_cast<long>(std::llround(value)); }

CartesianCoordinate locationToCartesian(const Location &location)
{
    double latRad = toRadians(location.latitude);
    double lonRad = toRadians(location.longitude);
    // Cartesian coordinates normalized for a sphere of diameter 1.0.
    return {0.5 * std::cos(latRad) * std::sin(lonRad),
            0.5 * std::cos(latRad) * std::cos(lonRad),
            0.5 * std::sin(latRad)};
}

long calculateDistanceMeters(const CartesianCoordinate &from,
                             const CartesianCoordinate &to)
{
    double dX = from.x - to.x;
    double dY = from.y - to.y;
    double dZ = from.z - to.z;
    double r = std::sqrt((dX * dX) + (dY * dY) + (dZ * dZ));
    return javaRound(HaversineDrivingTimeCalculator::TWICE_EARTH_RADIUS_IN_M
                     * std::asin(r));
}

}  // namespace

Seconds HaversineDrivingTimeCalculator::metersToDrivingSeconds(long meters)
{
    return javaRound(static_cast<double>(meters) / AVERAGE_SPEED_KMPH * 3.6);
}

Seconds
HaversineDrivingTimeCalculator::calculateDrivingTime(const Location &from,
                                                     const Location &to)
{
    if (from.latitude == to.latitude && from.longitude == to.longitude)
    {
        return 0;
    }
    return metersToDrivingSeconds(calculateDistanceMeters(
        locationToCartesian(from), locationToCartesian(to)));
}

void HaversineDrivingTimeCalculator::initDrivingTimeMatrix(PdpRoutePlan &plan)
{
    const std::size_t n = plan.locations.size();
    plan.drivingSeconds.assign(n, std::vector<Seconds>(n, 0));
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t j = 0; j < n; ++j)
        {
            plan.drivingSeconds[i][j]
                = (i == j) ? 0
                           : calculateDrivingTime(plan.locations[i],
                                                  plan.locations[j]);
        }
    }
}

}  // namespace pdp
