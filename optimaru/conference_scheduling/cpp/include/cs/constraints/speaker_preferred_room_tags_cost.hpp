#ifndef CS_CONSTRAINTS_SPEAKER_PREFERRED_ROOM_TAGS_COST_HPP
#define CS_CONSTRAINTS_SPEAKER_PREFERRED_ROOM_TAGS_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, weight 20: speakerPreferredRoomTagsCost — a room tag a speaker prefers
// and the assigned room lacks.
// Raw cost (one global total) = the sum of preferred tags the room lacks x the
// timeslot's duration, per speaker of each talk; 0 = no violation (every
// speaker's preferred room tags are present).
struct SpeakerPreferredRoomTagsCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 20.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SpeakerPreferredRoomTagsCost(double weight = kDefaultWeight)
        : BreakdownConstraint("speakerPreferredRoomTagsCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.speakerPreferredRoomTagsCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_PREFERRED_ROOM_TAGS_COST_HPP
