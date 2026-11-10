#ifndef CS_CONSTRAINTS_TALK_TYPE_INCOMPATIBILITY_COST_HPP
#define CS_CONSTRAINTS_TALK_TYPE_INCOMPATIBILITY_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Hard, weight 100000: talkTypeIncompatibilityCost — a talk in a timeslot or
// room that does not admit its talk type. The engine's
// single global value range cannot restrict a talk to its type-compatible
// cells, so such assignments are forbidden by penalty instead.
// Raw cost (one global total) = the sum of max(duration, 1) minutes for each
// talk whose timeslot does not admit its talk type, and again when its room
// does not; 0 = no violation (every talk sits in a timeslot and a room that
// admit its type).
struct TalkTypeIncompatibilityCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 100000.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit TalkTypeIncompatibilityCost(double weight = kDefaultWeight)
        : BreakdownConstraint("talkTypeIncompatibilityCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.talkTypeIncompatibilityCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_TYPE_INCOMPATIBILITY_COST_HPP
