#ifndef FCS_CONSTRAINTS_TRANSFER_BETWEEN_TWO_FLIGHTS_COST_HPP
#define FCS_CONSTRAINTS_TRANSFER_BETWEEN_TWO_FLIGHTS_COST_HPP

#include "fcs/constraints/common.hpp"

namespace fcs
{

// Hard, weight 1: transferBetweenTwoFlightsCost — an employee's flight
// arrives at one airport and their next departure leaves from another, so
// somebody has to be flown or driven across. Raw cost per employee = the
// number of such departure-consecutive pairs, so 0 = every itinerary is a
// connected chain. The weight 1 is deliberately 1/100 of
// requiredSkillCost.
//
// "Next" is by departure time (see ScoreCalculator's header): an
// assignment sharing its departure time with another never starts a pair,
// while ties on the later side all count — which is why this goes through
// itineraryStats instead of a hand-rolled adjacency walk.
struct TransferBetweenTwoFlightsCost final : ItineraryConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TransferBetweenTwoFlightsCost(double weight = kDefaultWeight)
        : ItineraryConstraint(
            "transferBetweenTwoFlightsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const ItineraryStats &stats) const override
    {
        return stats.transferMismatches;
    }
};

}  // namespace fcs

#endif  // FCS_CONSTRAINTS_TRANSFER_BETWEEN_TWO_FLIGHTS_COST_HPP
