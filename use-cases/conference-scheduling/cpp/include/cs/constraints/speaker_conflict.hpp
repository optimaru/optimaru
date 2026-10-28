#ifndef CS_CONSTRAINTS_SPEAKER_CONFLICT_HPP
#define CS_CONSTRAINTS_SPEAKER_CONFLICT_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 10): speakerConflict — two overlapping talks sharing a
// speaker, weighed by the overlapping minutes per shared speaker. Nobody can be
// in two rooms at once.
struct SpeakerConflict final : BreakdownConstraint
{
    explicit SpeakerConflict(double weight = 10.0)
        : BreakdownConstraint("speakerConflict", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerConflict;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_CONFLICT_HPP
