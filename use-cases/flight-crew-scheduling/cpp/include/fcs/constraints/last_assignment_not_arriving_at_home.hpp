#ifndef FCS_CONSTRAINTS_LAST_ASSIGNMENT_NOT_ARRIVING_AT_HOME_HPP
#define FCS_CONSTRAINTS_LAST_ASSIGNMENT_NOT_ARRIVING_AT_HOME_HPP

#include "fcs/constraints/common.hpp"

namespace fcs
{

// Soft (default, weight 1'000): lastAssignmentNotArrivingAtHome — the
// employee's latest assignment does not land at their home airport, so they
// end the roster stranded. Ties on the latest departure all count.
struct LastAssignmentNotArrivingAtHome final : ItineraryConstraint
{
    explicit LastAssignmentNotArrivingAtHome(double weight = 1000.0)
        : ItineraryConstraint("lastAssignmentNotArrivingAtHome",
                              weight,
                              /*hard=*/false)
    {
    }
    std::int64_t select(const ItineraryStats &stats) const override
    {
        return stats.lastNotHome;
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_LAST_ASSIGNMENT_NOT_ARRIVING_AT_HOME_HPP
