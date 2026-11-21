#ifndef CS_CONSTRAINTS_SPEAKER_MAKESPAN_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_MAKESPAN_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, weight 20: speakerMakespanCost — a speaker whose talks span more than
// two days, weighed by the extra days times a working day — it is what keeps a
// speaker's conference short.
// Raw cost (one global total) = the sum of (last day - first day - 1) x 480
// minutes for each speaker whose talks span more than two consecutive days; 0
// = no violation (every speaker's talks fit within two consecutive days).
struct SpeakerMakespanCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SpeakerMakespanCost(double weight = kDefaultWeight)
        : BreakdownConstraint("speakerMakespanCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.speakerMakespanCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_MAKESPAN_COST_HPP
