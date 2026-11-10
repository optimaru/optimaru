#ifndef CS_CONSTRAINTS_TALK_MUTUALLY_EXCLUSIVE_TALKS_TAGS_COST_HPP
#define CS_CONSTRAINTS_TALK_MUTUALLY_EXCLUSIVE_TALKS_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Hard, weight 1: talkMutuallyExclusiveTalksTagsCost — two overlapping talks
// carrying the same mutually-exclusive tag, weighed by shared tags times the
// overlap.
// Raw cost (one global total) = the sum of shared mutually-exclusive tags x
// overlapping minutes for each pair of overlapping talks; 0 = no violation (no
// overlapping talks share such a tag).
struct TalkMutuallyExclusiveTalksTagsCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TalkMutuallyExclusiveTalksTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint("talkMutuallyExclusiveTalksTagsCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.talkMutuallyExclusiveTalksTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_MUTUALLY_EXCLUSIVE_TALKS_TAGS_COST_HPP
