#ifndef TS_SOLVER_HPP
#define TS_SOLVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "ts/domain.hpp"

namespace ts
{

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<TournamentSchedule>, with the engine's built-in
// change/swap moves and adaptive ruin-and-recreate (ALNS) enabled.
class Solver
{
public:
    struct Config
    {
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
        // clamp(8 * assignmentCount, 400, 3200), the scaling adopted on the
        // school-timetabling quality bench.
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts (the engine's multi-start): each worker
        // receives seed + i and the full wall-clock budget; the best
        // solution wins.
        int parallelSolverCount = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::ScoreChecks score_checks
            = optimaru::ScoreChecks::Off;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Without this the LA acceptor can lock into a local
        // optimum within milliseconds on small instances and never escape.
        bool ruin_recreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Engine strategy plugins (see core SearchSettings for the model).
        // descent_weight: probability a step runs a best-improvement descent on
        // a nearby entity pair instead of the move mix (a sweep costs
        // ~2 * teamCount evaluations here — cheap at this value range).
        double descent_weight = 0.0;
        // Destroy-and-repair as an ordinary acceptor-judged move (distinct
        // from the rare unconditional stagnation kick). 0 disables.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;
        // Gate for caller-registered custom operators (TeamCountMove): the
        // probability a step may draw them at all. 1 = ungated.
        double custom_move_probability = 1.0;

        // Guided change values (monolithic path only): steer change moves
        // toward teams that keep the move hard-feasible — not already playing
        // that day and not unavailable on it — read off the incremental
        // calculator's live counters. Uniform draws waste most proposals on
        // hard-violating teams once the schedule is feasible.
        bool value_hint = false;

        // Loaded-team-count macro moves (adopted on the quality bench — see
        // solver.cpp). assignmentCountImbalanceCost is an unfairness over the
        // LOADED teams only, so spreading the same assignments evenly over a
        // different number of teams can be a whole medium level better — a
        // jump no single-variable move reaches, because emptying (or
        // refilling) a team passes through wildly unfair intermediate states.
        // This operator reshapes the schedule onto a promising loaded-team
        // count in one move and lets the acceptor judge the result.
        bool teamCountMoves = true;

        std::function<void(std::int64_t, const Score &)> on_new_best;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stop_flag;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates teamAssignments[*].team and stores the best
    // score found in schedule.score. Pinned assignments keep their team.
    void solve(TournamentSchedule &schedule);

    // Solves with a caller-assembled plugin calculator (see
    // ts/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights come from the assembly, NOT from the
    // level defaults. The final schedule.score is computed by THIS calculator
    // and the calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score.
    //
    // Two knobs do not apply on this path: parallelSolverCount (the
    // caller-owned calculator cannot be cloned across workers) and
    // teamCountMoves (the guided operator binds to the monolithic
    // calculator's live aggregates). The search is the engine's construction
    // heuristic + late-acceptance local search + ALNS kicks.
    void
    solve(TournamentSchedule &schedule,
          optimaru::DecomposedObjective<TournamentSchedule> &calculator);

private:
    Config config_;
};

}  // namespace ts

#endif  // TS_SOLVER_HPP
