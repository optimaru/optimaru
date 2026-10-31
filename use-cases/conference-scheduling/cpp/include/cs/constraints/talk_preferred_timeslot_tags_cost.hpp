#ifndef CS_CONSTRAINTS_TALK_PREFERRED_TIMESLOT_TAGS_COST_HPP
#define CS_CONSTRAINTS_TALK_PREFERRED_TIMESLOT_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 20: talkPreferredTimeslotTagsCost — a timeslot tag the talk
// prefers and the assigned timeslot lacks.
// Raw cost (one global total) = the sum of preferred tags the timeslot lacks x
// its duration, per talk; 0 = no violation (every talk's preferred timeslot
// tags are present).
struct TalkPreferredTimeslotTagsCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TalkPreferredTimeslotTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "talkPreferredTimeslotTagsCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkPreferredTimeslotTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PREFERRED_TIMESLOT_TAGS_COST_HPP
