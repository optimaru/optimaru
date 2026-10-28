#ifndef CS_CONSTRAINTS_SPEAKER_PREFERRED_TIMESLOT_TAGS_HPP
#define CS_CONSTRAINTS_SPEAKER_PREFERRED_TIMESLOT_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 20): speakerPreferredTimeslotTags — a timeslot tag a
// speaker prefers and the assigned timeslot lacks.
struct SpeakerPreferredTimeslotTags final : BreakdownConstraint
{
    explicit SpeakerPreferredTimeslotTags(double weight = 20.0)
        : BreakdownConstraint(
              "speakerPreferredTimeslotTags", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerPreferredTimeslotTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_PREFERRED_TIMESLOT_TAGS_HPP
