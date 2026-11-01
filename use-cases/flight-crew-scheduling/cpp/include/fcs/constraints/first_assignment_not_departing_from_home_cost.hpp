#ifndef FCS_CONSTRAINTS_FIRST_ASSIGNMENT_NOT_DEPARTING_FROM_HOME_COST_HPP
#define FCS_CONSTRAINTS_FIRST_ASSIGNMENT_NOT_DEPARTING_FROM_HOME_COST_HPP

#include "fcs/constraints/common.hpp"

namespace fcs
{

// Soft, weight 1'000: firstAssignmentNotDepartingFromHomeCost — the
// employee's earliest assignment does not leave from their home airport, so
// they must get there on their own time. Raw cost per employee = the number
// of their earliest-departure assignments (ties on the earliest departure all
// count) that leave from another airport, so 0 = the roster starts at home
// (an employee without assignments costs 0).
struct FirstAssignmentNotDepartingFromHomeCost final : ItineraryConstraint
{
    static constexpr double kDefaultWeight = 1000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit FirstAssignmentNotDepartingFromHomeCost(
        double weight = kDefaultWeight)
        : ItineraryConstraint(
            "firstAssignmentNotDepartingFromHomeCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const ItineraryStats &stats) const override
    {
        return stats.firstNotHome;
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_FIRST_ASSIGNMENT_NOT_DEPARTING_FROM_HOME_COST_HPP
