#ifndef CS_CONSTRAINTS_TALK_PREREQUISITE_TALKS_HPP
#define CS_CONSTRAINTS_TALK_PREREQUISITE_TALKS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 10): talkPrerequisiteTalks — a talk scheduled before
// (or overlapping) a talk it lists as a prerequisite, weighed by their combined
// duration.
struct TalkPrerequisiteTalks final : BreakdownConstraint
{
    explicit TalkPrerequisiteTalks(double weight = 10.0)
        : BreakdownConstraint("talkPrerequisiteTalks", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkPrerequisiteTalks;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PREREQUISITE_TALKS_HPP
