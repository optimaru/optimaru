#ifndef CS_CONSTRAINTS_TALK_UNDESIRED_TIMESLOT_TAGS_HPP
#define CS_CONSTRAINTS_TALK_UNDESIRED_TIMESLOT_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 20): talkUndesiredTimeslotTags — a timeslot tag the
// talk would rather avoid and the assigned timeslot carries.
struct TalkUndesiredTimeslotTags final : BreakdownConstraint
{
    explicit TalkUndesiredTimeslotTags(double weight = 20.0)
        : BreakdownConstraint(
              "talkUndesiredTimeslotTags", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkUndesiredTimeslotTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_UNDESIRED_TIMESLOT_TAGS_HPP
