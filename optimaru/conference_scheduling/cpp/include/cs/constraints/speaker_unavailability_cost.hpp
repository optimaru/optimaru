#ifndef CS_CONSTRAINTS_SPEAKER_UNAVAILABILITY_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_UNAVAILABILITY_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 100: speakerUnavailabilityCost — a talk in a timeslot one of
// its speakers is unavailable for.
// Raw cost (one global total) = the sum of the timeslot's duration for each
// unavailable speaker of each talk (speaker-minutes); 0 = no violation (every
// speaker is available for their talks).
struct SpeakerUnavailabilityCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 100.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SpeakerUnavailabilityCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "speakerUnavailabilityCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerUnavailabilityCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_UNAVAILABILITY_COST_HPP
