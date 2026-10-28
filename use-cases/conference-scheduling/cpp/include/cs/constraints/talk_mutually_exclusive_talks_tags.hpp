#ifndef CS_CONSTRAINTS_TALK_MUTUALLY_EXCLUSIVE_TALKS_TAGS_HPP
#define CS_CONSTRAINTS_TALK_MUTUALLY_EXCLUSIVE_TALKS_TAGS_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 1): talkMutuallyExclusiveTalksTags — two overlapping
// talks carrying the same mutually-exclusive tag, weighed by shared tags times
// the overlap.
struct TalkMutuallyExclusiveTalksTags final : BreakdownConstraint
{
    explicit TalkMutuallyExclusiveTalksTags(double weight = 1.0)
        : BreakdownConstraint(
              "talkMutuallyExclusiveTalksTags", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkMutuallyExclusiveTalksTags;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_MUTUALLY_EXCLUSIVE_TALKS_TAGS_HPP
