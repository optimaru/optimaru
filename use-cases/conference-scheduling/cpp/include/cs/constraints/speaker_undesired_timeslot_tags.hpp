#ifndef CS_CONSTRAINTS_SPEAKER_UNDESIRED_TIMESLOT_TAGS_HPP
#define CS_CONSTRAINTS_SPEAKER_UNDESIRED_TIMESLOT_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 20): speakerUndesiredTimeslotTags — a timeslot tag a
// speaker would rather avoid and the assigned timeslot carries.
struct SpeakerUndesiredTimeslotTags final : BreakdownConstraint
{
    explicit SpeakerUndesiredTimeslotTags(double weight = 20.0)
        : BreakdownConstraint(
              "speakerUndesiredTimeslotTags", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerUndesiredTimeslotTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_UNDESIRED_TIMESLOT_TAGS_HPP
