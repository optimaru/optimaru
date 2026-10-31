#ifndef CS_CONSTRAINTS_TALK_PREREQUISITE_TALKS_COST_HPP
#define CS_CONSTRAINTS_TALK_PREREQUISITE_TALKS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 10: talkPrerequisiteTalksCost — a talk scheduled before (or
// overlapping) a talk it lists as a prerequisite, weighed by their combined
// duration.
// Raw cost (one global total) = the sum of the combined duration of both talks
// for each prerequisite that does not end before its dependent talk starts; 0
// = no violation (every prerequisite ends before the talk that needs it).
struct TalkPrerequisiteTalksCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TalkPrerequisiteTalksCost(double weight = kDefaultWeight)
        : BreakdownConstraint(
              "talkPrerequisiteTalksCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.talkPrerequisiteTalksCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_PREREQUISITE_TALKS_COST_HPP
