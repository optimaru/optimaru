#ifndef CS_CONSTRAINTS_TALK_PROHIBITED_TIMESLOT_TAGS_HPP
#define CS_CONSTRAINTS_TALK_PROHIBITED_TIMESLOT_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): talkProhibitedTimeslotTags — a timeslot tag the
// talk forbids and the assigned timeslot carries.
struct TalkProhibitedTimeslotTags final : BreakdownConstraint
{
    explicit TalkProhibitedTimeslotTags(double weight = 1.0)
        : BreakdownConstraint(
              "talkProhibitedTimeslotTags", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkProhibitedTimeslotTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PROHIBITED_TIMESLOT_TAGS_HPP
