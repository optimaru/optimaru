#ifndef CS_CONSTRAINTS_SPEAKER_PROHIBITED_TIMESLOT_TAGS_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_PROHIBITED_TIMESLOT_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: speakerProhibitedTimeslotTagsCost — a timeslot tag a speaker
// forbids and the assigned timeslot carries.
// Raw cost (one global total) = the sum of prohibited tags the timeslot
// carries x its duration, per speaker of each talk; 0 = no violation (no
// speaker sits in a timeslot with a tag they prohibit).
struct SpeakerProhibitedTimeslotTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SpeakerProhibitedTimeslotTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "speakerProhibitedTimeslotTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerProhibitedTimeslotTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_PROHIBITED_TIMESLOT_TAGS_COST_HPP
