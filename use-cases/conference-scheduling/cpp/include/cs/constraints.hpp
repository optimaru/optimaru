#ifndef CS_CONSTRAINTS_HPP
#define CS_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/ (<subject>_cost.hpp, the struct
// name in snake_case); common.hpp holds what all of them
// need (the calculator aliases, the shared breakdown projection and the base
// every component derives from) and carries the pattern's documentation —
// including WHY this domain decomposes by projection rather than by
// re-deriving 36 pairwise cost functions.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// defaultConstraints() below when it belongs to the default assembly).

#include "cs/constraints/audience_level_diversity_cost.hpp"
#include "cs/constraints/audience_type_diversity_cost.hpp"
#include "cs/constraints/audience_type_theme_track_conflict_cost.hpp"
#include "cs/constraints/common.hpp"
#include "cs/constraints/consecutive_talks_pause_cost.hpp"
#include "cs/constraints/content_audience_level_flow_violation_cost.hpp"
#include "cs/constraints/content_conflict_cost.hpp"
#include "cs/constraints/crowd_control_risk_cost.hpp"
#include "cs/constraints/different_day_talks_cost.hpp"
#include "cs/constraints/language_diversity_cost.hpp"
#include "cs/constraints/popular_talks_room_size_cost.hpp"
#include "cs/constraints/room_conflict_cost.hpp"
#include "cs/constraints/room_unavailability_cost.hpp"
#include "cs/constraints/sector_conflict_cost.hpp"
#include "cs/constraints/speaker_conflict_cost.hpp"
#include "cs/constraints/speaker_makespan_cost.hpp"
#include "cs/constraints/speaker_preferred_room_tags_cost.hpp"
#include "cs/constraints/speaker_preferred_timeslot_tags_cost.hpp"
#include "cs/constraints/speaker_prohibited_room_tags_cost.hpp"
#include "cs/constraints/speaker_prohibited_timeslot_tags_cost.hpp"
#include "cs/constraints/speaker_required_room_tags_cost.hpp"
#include "cs/constraints/speaker_required_timeslot_tags_cost.hpp"
#include "cs/constraints/speaker_unavailability_cost.hpp"
#include "cs/constraints/speaker_undesired_room_tags_cost.hpp"
#include "cs/constraints/speaker_undesired_timeslot_tags_cost.hpp"
#include "cs/constraints/talk_mutually_exclusive_talks_tags_cost.hpp"
#include "cs/constraints/talk_preferred_room_tags_cost.hpp"
#include "cs/constraints/talk_preferred_timeslot_tags_cost.hpp"
#include "cs/constraints/talk_prerequisite_talks_cost.hpp"
#include "cs/constraints/talk_prohibited_room_tags_cost.hpp"
#include "cs/constraints/talk_prohibited_timeslot_tags_cost.hpp"
#include "cs/constraints/talk_required_room_tags_cost.hpp"
#include "cs/constraints/talk_required_timeslot_tags_cost.hpp"
#include "cs/constraints/talk_type_incompatibility_cost.hpp"
#include "cs/constraints/talk_undesired_room_tags_cost.hpp"
#include "cs/constraints/talk_undesired_timeslot_tags_cost.hpp"
#include "cs/constraints/theme_track_conflict_cost.hpp"
#include "cs/constraints/theme_track_room_change_cost.hpp"

namespace cs
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// All 36 provider constraints plus the port's synthetic
// talkTypeIncompatibilityCost, each at its component default (kDefaultWeight /
// kDefaultLevel = the Java weights and levels). This assembly scores
// bit-for-bit like ScoreCalculator::calculateScore and the monolithic
// incremental calculator, rewards included: both read the same constants.
// The order is the catalog order (list_constraints() / constraint_info()).
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("audienceLevelDiversityCost", 2)
//         .soft("roomConflictCost")
//         .drop("audienceTypeDiversityCost")
//         .into(calc);
//
// The three *DiversityCost rewards refuse .hard(...) and any level change
// (reward, not level-flippable); every other constraint flips both ways.
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<TalkTypeIncompatibilityCost>();
    assembly.emplace<RoomUnavailabilityCost>();
    assembly.emplace<RoomConflictCost>();
    assembly.emplace<SpeakerUnavailabilityCost>();
    assembly.emplace<SpeakerConflictCost>();
    assembly.emplace<TalkPrerequisiteTalksCost>();
    assembly.emplace<TalkMutuallyExclusiveTalksTagsCost>();
    assembly.emplace<ConsecutiveTalksPauseCost>();
    assembly.emplace<CrowdControlRiskCost>();
    assembly.emplace<SpeakerRequiredTimeslotTagsCost>();
    assembly.emplace<SpeakerProhibitedTimeslotTagsCost>();
    assembly.emplace<TalkRequiredTimeslotTagsCost>();
    assembly.emplace<TalkProhibitedTimeslotTagsCost>();
    assembly.emplace<SpeakerRequiredRoomTagsCost>();
    assembly.emplace<SpeakerProhibitedRoomTagsCost>();
    assembly.emplace<TalkRequiredRoomTagsCost>();
    assembly.emplace<TalkProhibitedRoomTagsCost>();
    assembly.emplace<ThemeTrackConflictCost>();
    assembly.emplace<ThemeTrackRoomChangeCost>();
    assembly.emplace<SectorConflictCost>();
    assembly.emplace<AudienceTypeThemeTrackConflictCost>();
    assembly.emplace<ContentAudienceLevelFlowViolationCost>();
    assembly.emplace<ContentConflictCost>();
    assembly.emplace<DifferentDayTalksCost>();
    assembly.emplace<PopularTalksRoomSizeCost>();
    assembly.emplace<SpeakerPreferredTimeslotTagsCost>();
    assembly.emplace<SpeakerUndesiredTimeslotTagsCost>();
    assembly.emplace<TalkPreferredTimeslotTagsCost>();
    assembly.emplace<TalkUndesiredTimeslotTagsCost>();
    assembly.emplace<SpeakerPreferredRoomTagsCost>();
    assembly.emplace<SpeakerUndesiredRoomTagsCost>();
    assembly.emplace<TalkPreferredRoomTagsCost>();
    assembly.emplace<TalkUndesiredRoomTagsCost>();
    assembly.emplace<SpeakerMakespanCost>();
    assembly.emplace<AudienceTypeDiversityCost>();
    assembly.emplace<AudienceLevelDiversityCost>();
    assembly.emplace<LanguageDiversityCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace cs

#endif  // CS_CONSTRAINTS_HPP
