#ifndef CS_CONSTRAINTS_SPEAKER_MAKESPAN_HPP
#define CS_CONSTRAINTS_SPEAKER_MAKESPAN_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 20): speakerMakespan — a speaker whose talks span more
// than two days, weighed by the extra days times a working day — it is what
// keeps a speaker's conference short.
struct SpeakerMakespan final : BreakdownConstraint
{
    explicit SpeakerMakespan(double weight = 20.0)
        : BreakdownConstraint("speakerMakespan", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.speakerMakespan;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SPEAKER_MAKESPAN_HPP
