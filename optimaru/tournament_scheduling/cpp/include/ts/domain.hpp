#ifndef TS_DOMAIN_HPP
#define TS_DOMAIN_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/levels.hpp"

namespace ts
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The Java quickstart scores with HardMediumSoftBigDecimalScore; the engine
// provides Score(int64 hard, double soft). Mapping rule (also in the
// README):
//
//   hard  = Java hard, unchanged (whole violation counts; 0 == feasible)
//   soft  = -(medium_unfairness * kMediumScale + soft_unfairness)
//
// i.e. the medium level (assignmentCountImbalanceCost) is folded into the
// engine's soft as an offset of kMediumScale = 1e6 per medium unit, keeping
// the level priority: both unfairness values are BigDecimal-style decimals
// rounded to 6 significant figures, and for this domain's instance sizes the
// soft-level unfairness stays far below kMediumScale times the smallest
// representable medium difference, so medium always dominates soft.
//
// The fold is declared ONCE, here: ScoreCalculator::combinedSoft packs the
// monolithic totals with kMediumFold, and the MediumFoldOf<TournamentSchedule>
// specialization at the end of this header hands the same constant to the
// decomposed calculator, which keeps a separate medium accumulator. The
// medium constraint's weight is therefore in medium units (default 1), never
// 1e6.
using Score = optimaru::Score;

// The medium -> soft folding offset (see the Score comment above).
constexpr double kMediumScale = 1e6;
constexpr optimaru::MediumFold kMediumFold
    = optimaru::MediumFold::into_soft(kMediumScale);

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
// "team cannot play on day": one hard point of teamUnavailabilityCost per
// fact whose (team, day) cell holds at least one assignment (Java's
// forEach(UnavailabilityPenalty).ifExists(TeamAssignment) — per fact, not per
// assignment).
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
// @PlanningId: it must be unique (teamConflictCost counts unordered pairs,
// the lessThan(getId) join of the Java original).
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
// SolutionTraits specialization (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are the team assignments, the
// planning variable is TeamAssignment::team, the value range is the teams
// list (@ValueRangeProvider on TournamentSchedule::teams).
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionTraits<ts::TournamentSchedule>
{
    static constexpr optimaru::Shape shape
        = optimaru::Shape::Assignment;
    static int entity_count(const ts::TournamentSchedule &s)
    {
        return static_cast<int>(s.teamAssignments.size());
    }
    static int value_count(const ts::TournamentSchedule &s)
    {
        return s.teamCount();
    }
    static int value_of(const ts::TournamentSchedule &s, int assignmentIndex)
    {
        return s.teamAssignments[assignmentIndex].team;
    }
    static void
    assign(ts::TournamentSchedule &s, int assignmentIndex, int team)
    {
        s.teamAssignments[assignmentIndex].team = team;
    }
    static bool is_pinned(const ts::TournamentSchedule &s, int assignmentIndex)
    {
        return s.teamAssignments[assignmentIndex].pinned;
    }
};

// ---------------------------------------------------------------------------
// Medium fold registration
// ---------------------------------------------------------------------------
// The decomposed calculator folds its medium accumulator with the domain's
// kMediumFold (see the Score adapter above): engine soft = soft + medium * 1e6.
template <> struct optimaru::MediumFoldOf<ts::TournamentSchedule>
{
    static constexpr optimaru::MediumFold value = ts::kMediumFold;
};

#endif  // TS_DOMAIN_HPP
