#ifndef CS_CONSTRAINTS_TALK_UNDESIRED_TIMESLOT_TAGS_COST_HPP
#define CS_CONSTRAINTS_TALK_UNDESIRED_TIMESLOT_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, weight 20: talkUndesiredTimeslotTagsCost — a timeslot tag the talk
// would rather avoid and the assigned timeslot carries.
// Raw cost (one global total) = the sum of undesired tags the timeslot carries
// x its duration, per talk; 0 = no violation (no talk sits in a timeslot with
// a tag it would rather avoid).
struct TalkUndesiredTimeslotTagsCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit TalkUndesiredTimeslotTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint("talkUndesiredTimeslotTagsCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.talkUndesiredTimeslotTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_TALK_UNDESIRED_TIMESLOT_TAGS_COST_HPP
