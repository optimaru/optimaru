#ifndef CS_CONSTRAINTS_TALK_REQUIRED_TIMESLOT_TAGS_HPP
#define CS_CONSTRAINTS_TALK_REQUIRED_TIMESLOT_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): talkRequiredTimeslotTags — a timeslot tag the talk
// requires and the assigned timeslot lacks.
struct TalkRequiredTimeslotTags final : BreakdownConstraint
{
    explicit TalkRequiredTimeslotTags(double weight = 1.0)
        : BreakdownConstraint("talkRequiredTimeslotTags", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkRequiredTimeslotTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_REQUIRED_TIMESLOT_TAGS_HPP
