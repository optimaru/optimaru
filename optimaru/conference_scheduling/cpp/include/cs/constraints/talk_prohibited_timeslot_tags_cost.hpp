#ifndef CS_CONSTRAINTS_TALK_PROHIBITED_TIMESLOT_TAGS_COST_HPP
#define CS_CONSTRAINTS_TALK_PROHIBITED_TIMESLOT_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: talkProhibitedTimeslotTagsCost — a timeslot tag the talk
// forbids and the assigned timeslot carries.
// Raw cost (one global total) = the sum of prohibited tags the timeslot
// carries x its duration, per talk; 0 = no violation (no talk sits in a
// timeslot with a tag it prohibits).
struct TalkProhibitedTimeslotTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TalkProhibitedTimeslotTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "talkProhibitedTimeslotTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkProhibitedTimeslotTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PROHIBITED_TIMESLOT_TAGS_COST_HPP
