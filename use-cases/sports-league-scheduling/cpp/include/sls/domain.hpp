#ifndef SLS_DOMAIN_HPP
#define SLS_DOMAIN_HPP

#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace sls
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The Java quickstart already uses HardSoftScore, so the mapping onto the
// engine's HardSoftScore(int64 hard, double soft) is the identity: hard is
// the violation count sum, soft is the (integral) travel-distance / classic
// penalty sum stored in the engine's double field. No level remapping needed.
using Score = optimaru::HardSoftScore;

// ---------------------------------------------------------------------------
// Team  (problem fact)
// ---------------------------------------------------------------------------
// The quickstart's Team. Its Map<Team, Integer> distanceToTeam becomes the
// solution-level distance matrix LeagueSchedule::distanceToTeam (indexed by
// team position), so Team itself is plain data.
struct Team
{
    std::string id;
    std::string name;
};

// ---------------------------------------------------------------------------
// Round  (problem fact, the planning value)
// ---------------------------------------------------------------------------
// `index` is the quickstart's @PlanningId round number: every constraint
// compares/offsets round *indices* (same day, next day, first/last round),
// not positions in the rounds vector. Indices must be >= 0 and are normally
// contiguous 0..N-1, but sparse indices work too (a missing index simply has
// no round, like the quickstart's ifNotExists(Round) joins).
struct Round
{
    int index = 0;
    // Rounds on weekends/holidays; classic matches prefer these (soft).
    bool weekendOrHoliday = false;
};

// ---------------------------------------------------------------------------
// Match  (@PlanningEntity)
// ---------------------------------------------------------------------------
// `round` is the only planning variable: the position of the assigned Round
// in LeagueSchedule::rounds, or -1 while unassigned. Teams are referenced by
// their position in LeagueSchedule::teams.
struct Match
{
    std::string id;
    int homeTeam = -1;
    int awayTeam = -1;
    // A classic/important match (e.g. derby): prefers weekend/holiday rounds.
    bool classicMatch = false;

    int round = -1;  // planning variable; -1 == unassigned

    // Precomputed by ScoreCalculator::prepare (kept at the end so 4-field
    // brace initializers elsewhere still compile): dense ids of the ordered
    // (home, away) team pair and of the reversed pair (-1 when no match has
    // the reversed pair) — used by the "repeat match on the next day" hard
    // constraint.
    int pairId = -1;
    int reversePairId = -1;
};

// ---------------------------------------------------------------------------
// LeagueSchedule  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct LeagueSchedule
{
    std::vector<Team> teams;
    std::vector<Round> rounds;
    std::vector<Match> matches;

    // distanceToTeam[from][to] in km (the quickstart's Team.distanceToTeam
    // maps, flattened to the solution). Directional lookups are preserved
    // (dist(a, b) may differ from dist(b, a)); an empty matrix means all
    // distances are zero (prepare() fills it).
    std::vector<std::vector<int>> distanceToTeam;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against re-interning the
    // ordered-pair ids on every calculateScore call.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int maxRoundIndex = -1;
    int pairCount = 0;
    // roundIndexExists[i] != 0 iff some Round has index i (the quickstart's
    // ifNotExists(Round, index == ...) probes).
    std::vector<char> roundIndexExists;

    int teamCount() const { return static_cast<int>(teams.size()); }
    int roundCount() const { return static_cast<int>(rounds.size()); }
    int matchCount() const { return static_cast<int>(matches.size()); }
    int distance(int fromTeam, int toTeam) const
    {
        return distanceToTeam[fromTeam][toTeam];
    }
    // The constraint-facing round number of a planning value (see Round).
    int roundIndexOf(int roundPosition) const
    {
        return rounds[roundPosition].index;
    }
};

}  // namespace sls

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are matches, the planning variable
// is Match::round, the value range is the rounds vector.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<sls::LeagueSchedule>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const sls::LeagueSchedule &s)
    {
        return static_cast<int>(s.matches.size());
    }
    static int valueCount(const sls::LeagueSchedule &s)
    {
        return static_cast<int>(s.rounds.size());
    }
    static int getVariable(const sls::LeagueSchedule &s, int matchIndex)
    {
        return s.matches[matchIndex].round;
    }
    static void setVariable(sls::LeagueSchedule &s, int matchIndex, int round)
    {
        s.matches[matchIndex].round = round;
    }
};

#endif  // SLS_DOMAIN_HPP
