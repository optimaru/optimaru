#ifndef CS_CONSTRAINTS_SPEAKER_UNAVAILABLE_TIMESLOT_HPP
#define CS_CONSTRAINTS_SPEAKER_UNAVAILABLE_TIMESLOT_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Hard (default, weight 100): speakerUnavailableTimeslot — a talk in a timeslot
// one of its speakers is unavailable for.
struct SpeakerUnavailableTimeslot final : BreakdownConstraint
{
    explicit SpeakerUnavailableTimeslot(double weight = 100.0)
        : BreakdownConstraint(
              "speakerUnavailableTimeslot", weight, /*hard=*/true)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerUnavailableTimeslot;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_UNAVAILABLE_TIMESLOT_HPP
