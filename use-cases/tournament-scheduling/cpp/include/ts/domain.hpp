#ifndef TS_DOMAIN_HPP
#define TS_DOMAIN_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace ts
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The Java quickstart scores with HardMediumSoftBigDecimalScore; the engine
// provides HardSoftScore(int64 hard, double soft). Mapping rule (also in the
// README):
//
//   hard  = Java hard, unchanged (whole violation counts; 0 == feasible)
//   soft  = -(medium_unfairness * kMediumScale + soft_unfairness)
//
// i.e. the medium level (fairAssignmentCountPerTeam) is folded into the
// engine's soft as an offset of kMediumScale = 1e6 per medium unit, keeping
// the level priority: both unfairness values are BigDecimal-style decimals
// rounded to 6 significant figures, and for this domain's instance sizes the
// soft-level unfairness stays far below kMediumScale times the smallest
// representable medium difference, so medium always dominates soft.
using Score = optimaru::HardSoftScore;

// The medium -> soft folding offset (see the Score comment above).
constexpr double kMediumScale = 1e6;

// ---------------------------------------------------------------------------
// Team  (problem fact, @PlanningId id == its index in TournamentSchedule::
// teams — the value range of the planning variable)
// ---------------------------------------------------------------------------
struct Team
{
    std::string name;
};

// ---------------------------------------------------------------------------
// Day  (problem fact)
// ---------------------------------------------------------------------------
// The quickstart's Day only carries dateIndex; assignments and penalties
// reference days by index into TournamentSchedule::days.
struct Day
{
    int dateIndex = 0;
};

// ---------------------------------------------------------------------------
// UnavailabilityPenalty  (problem fact)
// ---------------------------------------------------------------------------
// "team cannot play on day": one hard point per fact whose (team, day) cell
// holds at least one assignment (Java's forEach(UnavailabilityPenalty)
// .ifExists(TeamAssignment) — per fact, not per assignment).
struct UnavailabilityPenalty
{
    int team = 0;  // index into teams
    int day = 0;   // index into days
};

// ---------------------------------------------------------------------------
// TeamAssignment  (@PlanningEntity)
// ---------------------------------------------------------------------------
// One match slot on a day; the solver assigns which team fills it. `team` is
// the only planning variable (-1 == unassigned). `id` mirrors the Java
// @PlanningId: it must be unique (constraint 1 counts unordered pairs, the
// lessThan(getId) join of the Java original).
struct TeamAssignment
{
    std::int64_t id = 0;
    int day = 0;         // index into days (problem fact)
    int indexInDay = 0;  // informational, like the Java indexInDate

    int team = -1;  // planning variable; -1 == unassigned

    // Pinned assignments keep their team fixed (@PlanningPin): scored like
    // any assignment but never selected for a move.
    bool pinned = false;
};

// ---------------------------------------------------------------------------
// TournamentSchedule  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct TournamentSchedule
{
    std::vector<Team> teams;
    std::vector<Day> days;
    std::vector<UnavailabilityPenalty> unavailabilityPenalties;
    std::vector<TeamAssignment> teamAssignments;

    Score score;

    int teamCount() const { return static_cast<int>(teams.size()); }
    int dayCount() const { return static_cast<int>(days.size()); }
};

}  // namespace ts

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are the team assignments, the
// planning variable is TeamAssignment::team, the value range is the teams
// list (@ValueRangeProvider on TournamentSchedule::teams).
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<ts::TournamentSchedule>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const ts::TournamentSchedule &s)
    {
        return static_cast<int>(s.teamAssignments.size());
    }
    static int valueCount(const ts::TournamentSchedule &s)
    {
        return s.teamCount();
    }
    static int getVariable(const ts::TournamentSchedule &s, int assignmentIndex)
    {
        return s.teamAssignments[assignmentIndex].team;
    }
    static void
    setVariable(ts::TournamentSchedule &s, int assignmentIndex, int team)
    {
        s.teamAssignments[assignmentIndex].team = team;
    }
    static bool isPinned(const ts::TournamentSchedule &s, int assignmentIndex)
    {
        return s.teamAssignments[assignmentIndex].pinned;
    }
};

#endif  // TS_DOMAIN_HPP
