#ifndef CS_CONSTRAINTS_TALK_TYPE_COMPATIBILITY_HPP
#define CS_CONSTRAINTS_TALK_TYPE_COMPATIBILITY_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 100000): talkTypeCompatibility — a talk in a timeslot
// or room that does not admit its talk type. Synthetic: the Java quickstart
// makes such assignments impossible through per-talkType value ranges, which
// the engine's single global value range cannot express, so the port forbids
// them by penalty instead. It never fires on a solution the Java model could
// have produced.
struct TalkTypeCompatibility final : BreakdownConstraint
{
    explicit TalkTypeCompatibility(double weight = 100000.0)
        : BreakdownConstraint("talkTypeCompatibility", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkTypeCompatibility;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_TYPE_COMPATIBILITY_HPP
