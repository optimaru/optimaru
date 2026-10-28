#ifndef CS_CONSTRAINTS_HPP
#define CS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases, the shared breakdown projection and the base
// every component derives from) and carries the pattern's documentation —
// including WHY this domain decomposes by projection rather than by
// re-deriving 36 pairwise cost functions.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "cs/constraints/audience_level_diversity.hpp"
#include "cs/constraints/audience_type_diversity.hpp"
#include "cs/constraints/audience_type_theme_track_conflict.hpp"
#include "cs/constraints/common.hpp"
#include "cs/constraints/consecutive_talks_pause.hpp"
#include "cs/constraints/content_audience_level_flow_violation.hpp"
#include "cs/constraints/content_conflict.hpp"
#include "cs/constraints/crowd_control.hpp"
#include "cs/constraints/language_diversity.hpp"
#include "cs/constraints/popular_talks.hpp"
#include "cs/constraints/room_conflict.hpp"
#include "cs/constraints/room_unavailable_timeslot.hpp"
#include "cs/constraints/same_day_talks.hpp"
#include "cs/constraints/sector_conflict.hpp"
#include "cs/constraints/speaker_conflict.hpp"
#include "cs/constraints/speaker_makespan.hpp"
#include "cs/constraints/speaker_preferred_room_tags.hpp"
#include "cs/constraints/speaker_preferred_timeslot_tags.hpp"
#include "cs/constraints/speaker_prohibited_room_tags.hpp"
#include "cs/constraints/speaker_prohibited_timeslot_tags.hpp"
#include "cs/constraints/speaker_required_room_tags.hpp"
#include "cs/constraints/speaker_required_timeslot_tags.hpp"
#include "cs/constraints/speaker_unavailable_timeslot.hpp"
#include "cs/constraints/speaker_undesired_room_tags.hpp"
#include "cs/constraints/speaker_undesired_timeslot_tags.hpp"
#include "cs/constraints/talk_mutually_exclusive_talks_tags.hpp"
#include "cs/constraints/talk_preferred_room_tags.hpp"
#include "cs/constraints/talk_preferred_timeslot_tags.hpp"
#include "cs/constraints/talk_prerequisite_talks.hpp"
#include "cs/constraints/talk_prohibited_room_tags.hpp"
#include "cs/constraints/talk_prohibited_timeslot_tags.hpp"
#include "cs/constraints/talk_required_room_tags.hpp"
#include "cs/constraints/talk_required_timeslot_tags.hpp"
#include "cs/constraints/talk_type_compatibility.hpp"
#include "cs/constraints/talk_undesired_room_tags.hpp"
#include "cs/constraints/talk_undesired_timeslot_tags.hpp"
#include "cs/constraints/theme_track_conflict.hpp"
#include "cs/constraints/theme_track_room_stability.hpp"

namespace cs
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// All 36 provider constraints plus the port's synthetic talkTypeCompatibility,
// at the Java weights. This assembly scores bit-for-bit like
// ScoreCalculator::calculateScore and the monolithic incremental calculator,
// rewards included.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("audienceLevelDiversity", 2)
//         .drop("audienceTypeDiversity")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<TalkTypeCompatibility>();
    assembly.emplace<RoomUnavailableTimeslot>();
    assembly.emplace<RoomConflict>();
    assembly.emplace<SpeakerUnavailableTimeslot>();
    assembly.emplace<SpeakerConflict>();
    assembly.emplace<TalkPrerequisiteTalks>();
    assembly.emplace<TalkMutuallyExclusiveTalksTags>();
    assembly.emplace<ConsecutiveTalksPause>();
    assembly.emplace<CrowdControl>();
    assembly.emplace<SpeakerRequiredTimeslotTags>();
    assembly.emplace<SpeakerProhibitedTimeslotTags>();
    assembly.emplace<TalkRequiredTimeslotTags>();
    assembly.emplace<TalkProhibitedTimeslotTags>();
    assembly.emplace<SpeakerRequiredRoomTags>();
    assembly.emplace<SpeakerProhibitedRoomTags>();
    assembly.emplace<TalkRequiredRoomTags>();
    assembly.emplace<TalkProhibitedRoomTags>();
    assembly.emplace<ThemeTrackConflict>();
    assembly.emplace<ThemeTrackRoomStability>();
    assembly.emplace<SectorConflict>();
    assembly.emplace<AudienceTypeThemeTrackConflict>();
    assembly.emplace<ContentAudienceLevelFlowViolation>();
    assembly.emplace<ContentConflict>();
    assembly.emplace<SameDayTalks>();
    assembly.emplace<PopularTalks>();
    assembly.emplace<SpeakerPreferredTimeslotTags>();
    assembly.emplace<SpeakerUndesiredTimeslotTags>();
    assembly.emplace<TalkPreferredTimeslotTags>();
    assembly.emplace<TalkUndesiredTimeslotTags>();
    assembly.emplace<SpeakerPreferredRoomTags>();
    assembly.emplace<SpeakerUndesiredRoomTags>();
    assembly.emplace<TalkPreferredRoomTags>();
    assembly.emplace<TalkUndesiredRoomTags>();
    assembly.emplace<SpeakerMakespan>();
    assembly.emplace<AudienceTypeDiversity>();
    assembly.emplace<AudienceLevelDiversity>();
    assembly.emplace<LanguageDiversity>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace cs

#endif  // CS_CONSTRAINTS_HPP
