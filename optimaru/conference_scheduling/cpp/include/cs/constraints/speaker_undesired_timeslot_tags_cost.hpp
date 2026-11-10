#ifndef CS_CONSTRAINTS_SPEAKER_UNDESIRED_TIMESLOT_TAGS_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_UNDESIRED_TIMESLOT_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, weight 20: speakerUndesiredTimeslotTagsCost — a timeslot tag a speaker
// would rather avoid and the assigned timeslot carries.
// Raw cost (one global total) = the sum of undesired tags the timeslot carries
// x its duration, per speaker of each talk; 0 = no violation (no speaker sits
// in a timeslot with a tag they would rather avoid).
struct SpeakerUndesiredTimeslotTagsCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SpeakerUndesiredTimeslotTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint("speakerUndesiredTimeslotTagsCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.speakerUndesiredTimeslotTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_UNDESIRED_TIMESLOT_TAGS_COST_HPP
