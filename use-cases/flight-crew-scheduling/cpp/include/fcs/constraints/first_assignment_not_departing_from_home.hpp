#ifndef FCS_CONSTRAINTS_FIRST_ASSIGNMENT_NOT_DEPARTING_FROM_HOME_HPP
#define FCS_CONSTRAINTS_FIRST_ASSIGNMENT_NOT_DEPARTING_FROM_HOME_HPP

#include "fcs/constraints/common.hpp"

namespace fcs
{

// Soft (default, weight 1'000): firstAssignmentNotDepartingFromHome — the
// employee's earliest assignment does not leave from their home airport, so
// they must get there on their own time. Ties on the earliest departure all
// count.
struct FirstAssignmentNotDepartingFromHome final : ItineraryConstraint
{
    explicit FirstAssignmentNotDepartingFromHome(double weight = 1000.0)
        : ItineraryConstraint("firstAssignmentNotDepartingFromHome",
                              weight,
                              /*hard=*/false)
    {
    }
    std::int64_t select(const ItineraryStats &stats) const override
    {
        return stats.firstNotHome;
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_FIRST_ASSIGNMENT_NOT_DEPARTING_FROM_HOME_HPP
