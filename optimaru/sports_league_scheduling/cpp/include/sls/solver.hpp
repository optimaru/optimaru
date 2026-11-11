#ifndef SLS_SOLVER_HPP
#define SLS_SOLVER_HPP

#include <utility>

#include "sls/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace sls {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/.
// It wires up the
// incremental score calculator and delegates solving to
// optimaru::Solver<LeagueSchedule>, with the engine's built-in change/swap
// moves and adaptive ruin-and-recreate (ALNS) enabled.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0  clamp(32 * matchCount, 400, 3200) — every bench
    //                     instance from the 182-match demo up wants the 3200
    //                     ceiling, and the small-instance multiplier only
    //                     matters below ~100 matches. It is the first stage's
    //                     length: the search restarts with a doubled history
    //                     when it stalls (the engine's history_growth); a solve
    //                     that starts from a given schedule keeps this one
    //                     length
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
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
