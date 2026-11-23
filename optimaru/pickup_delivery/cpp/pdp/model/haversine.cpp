#include "pdp/model/haversine.hpp"

#include <algorithm>
#include <cmath>

namespace pdp {

namespace {

struct CartesianCoordinate {
    double x, y, z;
};

// Same value as POSIX M_PI, which MSVC's <cmath> does not define by default.
constexpr double kPi = 3.14159265358979323846;

double toRadians(double degrees) { return degrees * kPi / 180.0; }

// Rounds half up: floor(x + 0.5), returned as long. All inputs here
// are non-negative, for which std::llround (round-half-away-from-zero) agrees.
long roundToLong(double value) { return static_cast<long>(std::llround(value)); }

CartesianCoordinate locationToCartesian(const Location& location) {
    double latRad = toRadians(location.latitude);
    double lonRad = toRadians(location.longitude);
    // Cartesian coordinates normalized for a sphere of diameter 1.0.
    return {0.5 * std::cos(latRad) * std::sin(lonRad), 0.5 * std::cos(latRad) * std::cos(lonRad),
            0.5 * std::sin(latRad)};
}

long calculateDistanceMeters(const CartesianCoordinate& from, const CartesianCoordinate& to) {
    double dX = from.x - to.x;
    double dY = from.y - to.y;
    double dZ = from.z - to.z;
    double r = std::sqrt((dX * dX) + (dY * dY) + (dZ * dZ));
    return roundToLong(HaversineDrivingTimeCalculator::TWICE_EARTH_RADIUS_IN_M * std::asin(r));
}

}  // namespace

Seconds HaversineDrivingTimeCalculator::metersToDrivingSeconds(long meters) {
    return roundToLong(static_cast<double>(meters) / AVERAGE_SPEED_KMPH * 3.6);
}

Meters HaversineDrivingTimeCalculator::calculateDistance(const Location& from, const Location& to) {
    if (from.latitude == to.latitude && from.longitude == to.longitude) {
        return 0;
    }
    return calculateDistanceMeters(locationToCartesian(from), locationToCartesian(to));
}

Seconds HaversineDrivingTimeCalculator::calculateDrivingTime(const Location& from,
                                                             const Location& to) {
    return metersToDrivingSeconds(calculateDistance(from, to));
}

void HaversineDrivingTimeCalculator::fillMissingMatrices(PdpRoutePlan& plan) {
    const std::size_t profiles =
            std::max<std::size_t>({plan.durationMatrices.size(), plan.distanceMatrices.size(), 1});
    plan.durationMatrices.resize(profiles);
    plan.distanceMatrices.resize(profiles);
    const std::size_t n = plan.locations.size();
    Matrix meters;  // computed once, on the first empty matrix
    auto straightLine = [&]() -> const Matrix& {
        if (meters.empty() && n > 0) {
            meters.assign(n, std::vector<std::int64_t>(n, 0));
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = 0; j < n; ++j) {
                    if (i != j)
                        meters[i][j] = calculateDistance(plan.locations[i], plan.locations[j]);
                }
            }
        }
        return meters;
    };
    for (std::size_t p = 0; p < profiles; ++p) {
        if (plan.distanceMatrices[p].empty()) plan.distanceMatrices[p] = straightLine();
        if (plan.durationMatrices[p].empty()) {
            Matrix seconds = straightLine();
            for (auto& row : seconds) {
                for (auto& value : row) value = metersToDrivingSeconds(value);
            }
            plan.durationMatrices[p] = std::move(seconds);
        }
    }
}

}  // namespace pdp
