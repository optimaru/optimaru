#ifndef VRP_CONSTRAINTS_MAX_END_TIME_COST_HPP
#define VRP_CONSTRAINTS_MAX_END_TIME_COST_HPP

#include "vrp/constraints/route_sums.hpp"

namespace vrp {

// Hard, weight 1: maxEndTimeCost -- the minutes a visit's service runs past
// the end of its time window (Visit::maxEndTime), rounded up. Measured from
// the ARRIVAL time rather than the start of service, faithfully to the Java
// quickstart. Raw cost per vehicle = the sum over its visits of
// ceil(max(0, arrival + service - maxEndTime) / 60 s); 0 = every service
// finishes in its window (the pool list costs 0). The vehicle's own end time
// is vehicleMaxEndTimeCost.
//
// Lateness is not a sum along the route, so the sums answer a preview only
// as "none": on a plan where no visit waits for its window, every service on
// the candidate route finishes before its earliest deadline when the route's
// own finish time does. Otherwise the preview declines and the calculator
// walks the written-out route (cost()), still exact.
struct MaxEndTimeCost final : detail::SummedUnitConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxEndTimeCost(double weight = kDefaultWeight)
        : SummedUnitConstraint("maxEndTimeCost", weight, kDefaultLevel) {}

    bool piecewise_cost(const VehicleRoutePlan& plan, int unit, const optimaru::ListPiece* pieces,
                       int count, double& out) const override {
        if (detail::isPool(plan, unit)) {
            out = 0.0;
            return true;
        }
        if (!sums_->noWaiting()) return false;
        const detail::RouteSumsContext::PieceWalk walk = sums_->walk(unit, pieces, count, /*withMinEnd=*/true);
        if (!walk.any) {
            out = 0.0;
            return true;
        }
        const Seconds finish = plan.vehicles[unit].departureTime + walk.drive + walk.service;
        if (finish > walk.minEnd) return false;
        out = 0.0;
        return true;
    }

    double cost(const VehicleRoutePlan& plan, int unit,
                const std::vector<int>& members) const override {
        if (detail::isPool(plan, unit)) return 0.0;  // no arrival times there
        std::int64_t late = 0;
        detail::walkRoute(plan, unit, members, [&](int visitIndex, Seconds arrival) {
            const Visit& visit = plan.visits[visitIndex];
            const Seconds finished = arrival + visit.serviceDuration;
            if (finished > visit.maxEndTime) {
                // Visit.roundDurationToNextOrEqualMinutes.
                const Seconds over = finished - visit.maxEndTime;
                late += (over + 59) / 60;
            }
        });
        return static_cast<double>(late);
    }
};

}  // namespace vrp

#endif  // VRP_CONSTRAINTS_MAX_END_TIME_COST_HPP
