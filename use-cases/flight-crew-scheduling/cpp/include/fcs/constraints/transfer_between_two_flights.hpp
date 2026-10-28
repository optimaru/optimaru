#ifndef FCS_CONSTRAINTS_TRANSFER_BETWEEN_TWO_FLIGHTS_HPP
#define FCS_CONSTRAINTS_TRANSFER_BETWEEN_TWO_FLIGHTS_HPP

#include "fcs/constraints/common.hpp"

namespace fcs
{

// Hard (default, weight 1): transferBetweenTwoFlights — an employee's flight
// arrives at one airport and their next departure leaves from another, so
// somebody has to be flown or driven across. "Next" follows the Java stream
// exactly (see ScoreCalculator's header): an assignment sharing its departure
// time with another never starts a pair, while ties on the later side all
// count — which is why this goes through itineraryStats instead of a
// hand-rolled adjacency walk.
struct TransferBetweenTwoFlights final : ItineraryConstraint
{
    explicit TransferBetweenTwoFlights(double weight = 1.0)
        : ItineraryConstraint("transferBetweenTwoFlights",
                              weight,
                              /*hard=*/true)
    {
    }
    std::int64_t select(const ItineraryStats &stats) const override
    {
        return stats.transferMismatches;
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_TRANSFER_BETWEEN_TWO_FLIGHTS_HPP
