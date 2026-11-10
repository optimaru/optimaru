#ifndef VRP_CONSTRAINTS_ROUTE_GEOMETRY_HPP
#define VRP_CONSTRAINTS_ROUTE_GEOMETRY_HPP

#include <cmath>

#include "vrp/constraints/common.hpp"

namespace vrp
{

// ---------------------------------------------------------------------------
// Route geometry helpers (no constraint component lives here)
// ---------------------------------------------------------------------------
// The smallest enclosing circle of a route's visit points (an incremental
// minimum-enclosing-circle construction on an equirectangular projection
// around the mean latitude), used by enclosingCircleRadiusCost.
// maxRouteRadiusCost measures haversine distances from home instead
// (detail::haversineMeters in common.hpp).
namespace detail
{

struct EnclosingPoint
{
    double x;
    double y;
};

struct EnclosingCircle
{
    EnclosingPoint center{};
    double radius = -1.0;
};

inline double squaredDistance(const EnclosingPoint &a, const EnclosingPoint &b)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return dx * dx + dy * dy;
}

inline bool contains(const EnclosingCircle &circle, const EnclosingPoint &point)
{
    return circle.radius >= 0.0
           && squaredDistance(circle.center, point)
                  <= circle.radius * circle.radius * (1.0 + 1e-12);
}

inline EnclosingCircle diameterCircle(const EnclosingPoint &a,
                                      const EnclosingPoint &b)
{
    const EnclosingPoint center{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0};
    return {center, std::sqrt(squaredDistance(center, a))};
}

inline EnclosingCircle circumcircle(const EnclosingPoint &a,
                                    const EnclosingPoint &b,
                                    const EnclosingPoint &c)
{
    const double d
        = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::abs(d) < 1e-12)
        return {};
    const double aa = a.x * a.x + a.y * a.y;
    const double bb = b.x * b.x + b.y * b.y;
    const double cc = c.x * c.x + c.y * c.y;
    const EnclosingPoint center{
        (aa * (b.y - c.y) + bb * (c.y - a.y) + cc * (a.y - b.y)) / d,
        (aa * (c.x - b.x) + bb * (a.x - c.x) + cc * (b.x - a.x)) / d};
    return {center, std::sqrt(squaredDistance(center, a))};
}

inline double smallestEnclosingRadius(const std::vector<Location> &locations)
{
    if (locations.empty())
        return 0.0;
    constexpr double earthRadius = 6371000.0;
    constexpr double pi = 3.14159265358979323846;
    double meanLatitude = 0.0;
    for (const Location &location : locations)
        meanLatitude += location.latitude;
    meanLatitude = meanLatitude / locations.size() * pi / 180.0;
    std::vector<EnclosingPoint> points;
    points.reserve(locations.size());
    for (const Location &location : locations)
    {
        points.push_back({earthRadius * location.longitude * pi / 180.0
                              * std::cos(meanLatitude),
                          earthRadius * location.latitude * pi / 180.0});
    }

    // Deterministic incremental minimum-enclosing-circle algorithm. A route
    // rarely has more than a few hundred stops; O(n^3) worst case avoids
    // random test results while remaining cheap for this opt-in objective.
    EnclosingCircle circle{points.front(), 0.0};
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        if (contains(circle, points[i]))
            continue;
        circle = {points[i], 0.0};
        for (std::size_t j = 0; j < i; ++j)
        {
            if (contains(circle, points[j]))
                continue;
            circle = diameterCircle(points[i], points[j]);
            for (std::size_t k = 0; k < j; ++k)
            {
                if (contains(circle, points[k]))
                    continue;
                const EnclosingCircle candidate
                    = circumcircle(points[i], points[j], points[k]);
                if (candidate.radius >= 0.0)
                    circle = candidate;
            }
        }
    }
    return std::max(0.0, circle.radius);
}

}  // namespace detail

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_ROUTE_GEOMETRY_HPP
