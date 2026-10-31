#ifndef CS_CONSTRAINTS_TALK_REQUIRED_TIMESLOT_TAGS_COST_HPP
#define CS_CONSTRAINTS_TALK_REQUIRED_TIMESLOT_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 1: talkRequiredTimeslotTagsCost — a timeslot tag the talk
// requires and the assigned timeslot lacks.
// Raw cost (one global total) = the sum of required tags the timeslot lacks x
// its duration, per talk; 0 = no violation (every talk's required timeslot
// tags are present).
struct TalkRequiredTimeslotTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TalkRequiredTimeslotTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "talkRequiredTimeslotTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkRequiredTimeslotTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_REQUIRED_TIMESLOT_TAGS_COST_HPP
