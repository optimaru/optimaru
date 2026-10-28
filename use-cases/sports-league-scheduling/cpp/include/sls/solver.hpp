#ifndef SLS_SOLVER_HPP
#define SLS_SOLVER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "sls/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace sls
{

// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++
// analog of the quickstart building a SolverFactory from a SolverConfig
// (application.properties: termination.spent-limit=30s). It wires up the
// incremental score calculator and delegates solving to
// optimaru::Solver<LeagueSchedule>, with the engine's built-in change/swap
// moves and adaptive ruin-and-recreate (ALNS) enabled.
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget, mirroring termination.spent-limit (30s in the
        // quickstart's application.properties).
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit. Default is above every
        // achievable score, so it never fires (termination is by spentLimit).
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(32 * matchCount, 400, 3200) — every bench instance from the
        // 182-match demo up wants the 3200 ceiling, and the small-instance
        // multiplier only matters below ~100 matches.
        std::size_t lateAcceptanceSize = 0;
        std::uint64_t randomSeed = 0;

        // Independent parallel starts (the engine's multi-start): each worker
        // receives randomSeed + i and the full wall-clock budget; the best
        // solution wins.
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::EnvironmentMode environmentMode
            = optimaru::EnvironmentMode::NON_ASSERT;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Without this the LA acceptor can lock into a local
        // optimum within milliseconds on small instances and never escape.
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double changeMoveWeight = 0.75;
        double swapMoveWeight = 0.25;

        // Nearby selection: how many neighbors each match keeps. Two matches
        // are neighbors when they share a team — sharing a team is the only
        // way two matches can directly trade travel — ordered by the distance
        // between their venues. Biases swap partners and drives the LNS
        // related ruin toward one team's cluster of fixtures. 0 (the default)
        // = auto: 4 * teamCount, which covers a match's whole shared-team set
        // (4 * teamCount - 6 of them in a double round robin); a negative
        // value disables nearby selection.
        int nearbySize = 0;
        double nearbyProbability = 0.95;

        // Travel-guided entity selection: every move picks this many random
        // matches and moves the one currently carrying the most travel (plus
        // classic-match) penalty — tournament selection on the incremental
        // calculator's per-match soft cost. 1 (or 0) selects uniformly.
        // The guidance stands down while the solution is infeasible (see
        // SportsLeagueIncrementalScoreCalculator::costliestMatch).
        int costliestMatchSample = 4;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates matches[*].round and stores the best score
    // found in schedule.score.
    void solve(LeagueSchedule &schedule);

    // Solves with a caller-assembled plugin calculator (see
    // sls/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights come from the assembly, NOT from the quickstart
    // defaults. The final schedule.score is computed by THIS calculator and
    // the calculator is left primed on the final solution, so
    // constraintTotals() explains exactly that score.
    //
    // Two knobs do not apply on this path: parallelSolverCount (the
    // caller-owned calculator cannot be cloned across workers) and
    // costliestMatchSample (the travel-guided entity recommender reads the
    // monolithic calculator's per-match travel costs). The search is the
    // engine's construction heuristic + late-acceptance local search + ALNS
    // kicks.
    void solve(LeagueSchedule &schedule,
               optimaru::DecomposedScoreCalculator<LeagueSchedule> &calculator);

private:
    Config config_;
};

}  // namespace sls

#endif  // SLS_SOLVER_HPP
