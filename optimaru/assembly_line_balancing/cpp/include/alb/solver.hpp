#ifndef ALB_SOLVER_HPP
#define ALB_SOLVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "alb/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"

namespace alb {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/: it
// runs the domain construction (a first fit, see
// construction.hpp), freezes the station value range, wires up the
// incremental score calculator and the domain's own move (CloseStationMove)
// and value recommender, and delegates the search to
// optimaru::Solver<AssemblyLineBalancingProblem> (late acceptance + adaptive
// ruin-and-recreate on top of the built-in change / swap moves).
class Solver {
public:
    struct Config {
        // Wall-clock budget. 0 stops before the first step: the solve then
        // returns the construction (or a complete seed) unchanged.
        std::chrono::milliseconds time_limit{std::chrono::seconds(30)};

        // Stop early once bestScore >= target_score. Default is above every
        // achievable score, so it never fires.
        Score target_score{std::numeric_limits<std::int64_t>::max(),
                           std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion (so
        // step_limit = 0 means "no step limit", not "no search").
        std::chrono::milliseconds stall_time_limit{0};
        std::int64_t step_limit = 0;
        std::int64_t stall_step_limit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(8 * task count, 400, 3200).
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts: worker i solves its own copy of the
        // constructed problem with seed + i and the full budget; the
        // best score wins. Ignored by the constraint-assembly overload.
        int parallel_solver_count = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        optimaru::ScoreChecks score_checks = optimaru::ScoreChecks::Off;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS).
        bool ruin_recreate = true;
        std::int64_t ruin_recreate_unimproved_step_trigger = 5000;
        double ruin_recreate_min_ruin_fraction = 0.10;
        double ruin_recreate_max_ruin_fraction = 0.30;
        int ruin_recreate_max_ruin_count = 60;
        double ruin_recreate_operator_decay = 0.8;
        std::vector<double> ruin_recreate_rewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruin_recreate_polish_steps = 5000;
        // Destroy-and-repair as an ordinary acceptor-judged move (engine
        // ruin_recreate.as_move_weight); one costs ~ruinCount * station count
        // evaluations. 0 disables it.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move. Swaps move
        // load across a station boundary without opening a station.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Guided change-move targets (engine local_search.value_hint):
        // with this probability a change move draws the task's new station
        // uniformly from its current window — [latest predecessor station,
        // earliest successor station] intersected with its own
        // [min_station, max_station] (and max_stations) — instead of
        // uniformly over all stations. 0 = off.
        double value_recommender_probability = 0.5;

        // Assignment best-improvement descent strategy weight (engine
        // local_search.descent_weight); ~2 * station count evaluations per
        // application. 0 (default) = off.
        double descent_weight = 0.0;

        // Upper gate on how often a step may draw CloseStationMove instead
        // of the ordinary mix; the adaptive selector still chooses inside the
        // gate. 1 = unrestricted (engine default).
        double custom_move_probability = 1.0;

        // CloseStationMove: empty one used station in a single move (see
        // solver.cpp). usedStationsCost is a step function no single-task
        // move can cross.
        bool close_station_moves = true;

        std::function<void(std::int64_t, const Score&)> on_new_best;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stop_flag;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: prepares the problem, constructs (clearing every
    // station first unless problem.warmStart), searches, writes the best
    // stations and problem.score (a full recompute), and resets
    // problem.warmStart to false. Throws std::invalid_argument on invalid
    // facts or a cyclic precedence graph.
    void solve(AssemblyLineBalancingProblem& problem);

    // Solves with a caller-assembled plugin calculator (see
    // alb/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same construction and search apply. The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be cloned,
    // so parallel_solver_count is ignored.
    void solve(AssemblyLineBalancingProblem& problem,
               optimaru::DecomposedObjective<AssemblyLineBalancingProblem>& calculator);

private:
    Config config_;
};

}  // namespace alb

#endif  // ALB_SOLVER_HPP
