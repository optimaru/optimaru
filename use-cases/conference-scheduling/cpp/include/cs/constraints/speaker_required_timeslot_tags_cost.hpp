#ifndef CS_CONSTRAINTS_SPEAKER_REQUIRED_TIMESLOT_TAGS_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_REQUIRED_TIMESLOT_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: speakerRequiredTimeslotTagsCost — a timeslot tag a speaker
// requires and the assigned timeslot lacks, weighed by missing tags times the
// duration.
// Raw cost (one global total) = the sum of required tags the timeslot lacks x
// its duration, per speaker of each talk; 0 = no violation (every speaker's
// required timeslot tags are present).
struct SpeakerRequiredTimeslotTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SpeakerRequiredTimeslotTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "speakerRequiredTimeslotTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerRequiredTimeslotTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_REQUIRED_TIMESLOT_TAGS_COST_HPP
