#ifndef CS_CONSTRAINTS_SPEAKER_PREFERRED_TIMESLOT_TAGS_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_PREFERRED_TIMESLOT_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 20: speakerPreferredTimeslotTagsCost — a timeslot tag a speaker
// prefers and the assigned timeslot lacks.
// Raw cost (one global total) = the sum of preferred tags the timeslot lacks x
// its duration, per speaker of each talk; 0 = no violation (every speaker's
// preferred timeslot tags are present).
struct SpeakerPreferredTimeslotTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SpeakerPreferredTimeslotTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "speakerPreferredTimeslotTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerPreferredTimeslotTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_PREFERRED_TIMESLOT_TAGS_COST_HPP
