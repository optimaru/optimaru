#ifndef CS_CONSTRAINTS_SPEAKER_CONFLICT_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_CONFLICT_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard, weight 10: speakerConflictCost — two overlapping talks sharing a
// speaker, weighed by the overlapping minutes per shared speaker. Nobody can
// be in two rooms at once.
// Raw cost (one global total) = the sum of shared speakers x overlapping
// minutes for each pair of overlapping talks; 0 = no violation (nobody has to
// be in two rooms at once).
struct SpeakerConflictCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit SpeakerConflictCost(double weight = kDefaultWeight)
        : BreakdownConstraint("speakerConflictCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerConflictCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_CONFLICT_COST_HPP
