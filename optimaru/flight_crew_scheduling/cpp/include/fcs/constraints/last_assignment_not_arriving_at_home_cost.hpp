#ifndef FCS_CONSTRAINTS_LAST_ASSIGNMENT_NOT_ARRIVING_AT_HOME_COST_HPP
#define FCS_CONSTRAINTS_LAST_ASSIGNMENT_NOT_ARRIVING_AT_HOME_COST_HPP

#include "fcs/constraints/common.hpp"

namespace fcs
{

// Soft, weight 1'000: lastAssignmentNotArrivingAtHomeCost — the employee's
// latest assignment does not land at their home airport, so they end the
// roster stranded. Raw cost per employee = the number of their
// latest-departure assignments (ties on the latest departure all count) that
// arrive somewhere else, so 0 = the roster ends at home (an employee without
// assignments costs 0).
struct LastAssignmentNotArrivingAtHomeCost final : ItineraryConstraint
{
    static constexpr double kDefaultWeight = 1000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LastAssignmentNotArrivingAtHomeCost(
        double weight = kDefaultWeight)
        : ItineraryConstraint(
            "lastAssignmentNotArrivingAtHomeCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const ItineraryStats &stats) const override
    {
        return stats.lastNotHome;
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_LAST_ASSIGNMENT_NOT_ARRIVING_AT_HOME_COST_HPP
