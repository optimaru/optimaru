#ifndef CS_CONSTRAINTS_SPEAKER_REQUIRED_TIMESLOT_TAGS_HPP
#define CS_CONSTRAINTS_SPEAKER_REQUIRED_TIMESLOT_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): speakerRequiredTimeslotTags — a timeslot tag a
// speaker requires and the assigned timeslot lacks, weighed by missing tags
// times the duration.
struct SpeakerRequiredTimeslotTags final : BreakdownConstraint
{
    explicit SpeakerRequiredTimeslotTags(double weight = 1.0)
        : BreakdownConstraint(
              "speakerRequiredTimeslotTags", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerRequiredTimeslotTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_REQUIRED_TIMESLOT_TAGS_HPP
