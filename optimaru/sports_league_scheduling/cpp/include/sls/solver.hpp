#ifndef SLS_SOLVER_HPP
#define SLS_SOLVER_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "sls/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"

namespace sls {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/.
// It wires up the
// incremental score calculator and delegates solving to
// optimaru::Solver<LeagueSchedule>, with the engine's built-in change/swap
// moves and adaptive ruin-and-recreate (ALNS) enabled.
class Solver {
public:
    struct Config {
        // Wall-clock budget (30s by default).
        std::chrono::milliseconds time_limit{std::chrono::seconds(30)};

        // Stop early once bestScore >= target_score. Default is above every
        // achievable score, so it never fires (termination is by time_limit).
        Score target_score{std::numeric_limits<std::int64_t>::max(),
                           std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds stall_time_limit{0};
        std::int64_t step_limit = 0;
        std::int64_t stall_step_limit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(32 * matchCount, 400, 3200) — every bench instance from the
        // 182-match demo up wants the 3200 ceiling, and the small-instance
        // multiplier only matters below ~100 matches.
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts (the engine's multi-start): each worker
        // receives seed + i and the full wall-clock budget; the best
        // solution wins.
        int parallel_solver_count = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::ScoreChecks score_checks = optimaru::ScoreChecks::Off;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Without this the LA acceptor can lock into a local
        // optimum within milliseconds on small instances and never escape.
        bool ruin_recreate = true;
        std::int64_t ruin_recreate_unimproved_step_trigger = 5000;
        double ruin_recreate_min_ruin_fraction = 0.10;
        double ruin_recreate_max_ruin_fraction = 0.30;
        int ruin_recreate_max_ruin_count = 60;
        double ruin_recreate_operator_decay = 0.8;
        std::vector<double> ruin_recreate_rewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruin_recreate_polish_steps = 5000;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Nearby selection: how many neighbors each match keeps. Two matches
        // are neighbors when they share a team — sharing a team is the only
        // way two matches can directly trade travel — ordered by the distance
        // between their venues. Biases swap partners and drives the LNS
        // related ruin toward one team's cluster of fixtures. 0 (the default)
        // = auto: 4 * teamCount, which covers a match's whole shared-team set
        // (4 * teamCount - 6 of them in a double round robin); a negative
        // value disables nearby selection.
        int nearby_size = 0;
        double nearby_probability = 0.95;

        // Travel-guided entity selection: every move picks this many random
        // matches and moves the one currently carrying the most travel (plus
        // classic-match) penalty — tournament selection on the incremental
        // calculator's per-match soft cost. 1 (or 0) selects uniformly.
        // The guidance stands down while the solution is infeasible (see
        // SportsLeagueIncrementalScoreCalculator::costliestMatch).
        int costliest_match_sample = 4;

        std::function<void(std::int64_t, const Score&)> on_new_best;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stop_flag;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates matches[*].round and stores the best score
    // found in schedule.score.
    void solve(LeagueSchedule& schedule);

    // Solves with a caller-assembled plugin calculator (see
    // sls/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights come from the assembly, NOT from the
    // defaults. The final schedule.score is computed by THIS calculator and
    // the calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score.
    //
    // Two knobs do not apply on this path: parallel_solver_count (the
    // caller-owned calculator cannot be cloned across workers) and
    // costliest_match_sample (the travel-guided entity recommender reads the
    // monolithic calculator's per-match travel costs). The search is the
    // engine's construction heuristic + late-acceptance local search + ALNS
    // kicks.
    void solve(LeagueSchedule& schedule, optimaru::DecomposedObjective<LeagueSchedule>& calculator);

private:
    Config config_;
};

}  // namespace sls

#endif  // SLS_SOLVER_HPP
