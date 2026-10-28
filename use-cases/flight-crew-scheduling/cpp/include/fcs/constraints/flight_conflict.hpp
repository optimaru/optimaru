#ifndef FCS_CONSTRAINTS_FLIGHT_CONFLICT_HPP
#define FCS_CONSTRAINTS_FLIGHT_CONFLICT_HPP

#include "fcs/constraints/common.hpp"

namespace fcs
{

// Hard (default, weight 10): flightConflict — one point per pair of the same
// employee's assignments whose flights overlap in time. A plain sum over the
// pairs inside one itinerary, so the exact delta of one assignment joining or
// leaving is its overlap count against the other members.
struct FlightConflict final : ItineraryConstraint
{
    explicit FlightConflict(double weight = 10.0)
        : ItineraryConstraint("flightConflict", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const ItineraryStats &stats) const override
    {
        return stats.conflictPairs;
    }
    bool supportsDelta() const override { return true; }
    double deltaCost(const FlightCrewSchedule &schedule,
                     int /*unit*/,
                     const std::vector<int> &members,
                     int entity) const override
    {
        const Minutes start = schedule.departureOf(entity);
        const Minutes end = schedule.arrivalOf(entity);
        std::int64_t overlaps = 0;
        for (int other : members)
        {
            if (other == entity)
                continue;
            if (start < schedule.arrivalOf(other)
                && schedule.departureOf(other) < end)
                ++overlaps;
        }
        return static_cast<double>(overlaps);
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_FLIGHT_CONFLICT_HPP
