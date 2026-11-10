#ifndef FCS_CONSTRAINTS_FLIGHT_CONFLICT_COST_HPP
#define FCS_CONSTRAINTS_FLIGHT_CONFLICT_COST_HPP

#include "fcs/constraints/common.hpp"

namespace fcs {

// Hard, weight 10: flightConflictCost — one point per pair of the same
// employee's assignments whose flights overlap in time (aStart < bEnd &&
// bStart < aEnd; touching flights do not overlap). Raw cost per employee =
// the number of such pairs in their itinerary, so 0 = nobody is booked on two
// flights at once.
//
// A plain sum over the pairs inside one itinerary, so the exact delta of one
// assignment joining or leaving is its overlap count against the other
// members.
struct FlightConflictCost final : ItineraryConstraint {
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit FlightConflictCost(double weight = kDefaultWeight)
        : ItineraryConstraint("flightConflictCost", weight, kDefaultLevel) {}
    std::int64_t select(const ItineraryStats& stats) const override { return stats.conflictPairs; }
    bool supports_delta() const override { return true; }
    double delta_cost(const FlightCrewSchedule& schedule, int /*unit*/,
                      const std::vector<int>& members, int entity) const override {
        const Minutes start = schedule.departureOf(entity);
        const Minutes end = schedule.arrivalOf(entity);
        std::int64_t overlaps = 0;
        for (int other : members) {
            if (other == entity) continue;
            if (start < schedule.arrivalOf(other) && schedule.departureOf(other) < end) ++overlaps;
        }
        return static_cast<double>(overlaps);
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_FLIGHT_CONFLICT_COST_HPP
