#ifndef CS_CONSTRAINTS_SPEAKER_PROHIBITED_TIMESLOT_TAGS_HPP
#define CS_CONSTRAINTS_SPEAKER_PROHIBITED_TIMESLOT_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): speakerProhibitedTimeslotTags — a timeslot tag a
// speaker forbids and the assigned timeslot carries.
struct SpeakerProhibitedTimeslotTags final : BreakdownConstraint
{
    explicit SpeakerProhibitedTimeslotTags(double weight = 1.0)
        : BreakdownConstraint(
              "speakerProhibitedTimeslotTags", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerProhibitedTimeslotTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_PROHIBITED_TIMESLOT_TAGS_HPP
