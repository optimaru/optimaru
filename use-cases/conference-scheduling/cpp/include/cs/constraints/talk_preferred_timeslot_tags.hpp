#ifndef CS_CONSTRAINTS_TALK_PREFERRED_TIMESLOT_TAGS_HPP
#define CS_CONSTRAINTS_TALK_PREFERRED_TIMESLOT_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 20): talkPreferredTimeslotTags — a timeslot tag the
// talk prefers and the assigned timeslot lacks.
struct TalkPreferredTimeslotTags final : BreakdownConstraint
{
    explicit TalkPreferredTimeslotTags(double weight = 20.0)
        : BreakdownConstraint(
              "talkPreferredTimeslotTags", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkPreferredTimeslotTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PREFERRED_TIMESLOT_TAGS_HPP
