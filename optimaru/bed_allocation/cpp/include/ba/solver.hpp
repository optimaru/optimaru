#ifndef BA_SOLVER_HPP
#define BA_SOLVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "ba/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"

namespace ba {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<BedPlan>, with the engine's adaptive ruin-and-recreate
// (ALNS) kicks enabled by default.
class Solver {
public:
    struct Config {
        // Wall-clock budget (30s by default).
        std::chrono::milliseconds time_limit{std::chrono::seconds(30)};

        // Stop early once bestScore >= target_score. Default is above every
        // achievable score, so it never fires (termination is by time_limit).
        Score target_score{std::numeric_limits<std::int64_t>::max(),
                           std::numeric_limits<std::int64_t>::max(),
                           std::numeric_limits<std::int64_t>::max()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds stall_time_limit{0};
        std::int64_t step_limit = 0;
        std::int64_t stall_step_limit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(8 * stayCount, 8000, 12000). Bed allocation reaches hard 0 /
        // medium 0 within milliseconds, so the whole 10s+ budget is spent on
        // the soft level; a long acceptance history pays off there. Measured
        // (10s, single thread, 74/150/304-stay instances, 3 seeds) against
        // the previous clamp(8 * stayCount, 400, 3200): soft +5.7% / +5.1% /
        // +2.6%. 16000 was better still on the mid instance but 4.6% worse on
        // the largest, so the floor stops at 8000.
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts (the engine's multi-start): each worker
        // receives seed + i and the full wall-clock budget; the best
        // solution wins.
        int parallelSolverCount = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::ScoreChecks score_checks = optimaru::ScoreChecks::Off;

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

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // --- nearby selection (see solver.cpp) ---
        // Distance between two stays is how far apart their date windows are,
        // so a nearby swap partner occupies roughly the same nights (the swap
        // stays hard-clean) and the LNS related ruin removes one temporal
        // cluster. 0 disables nearby selection entirely.
        //
        // Measured (10s, single thread, 6 seeds) against nearby disabled:
        // soft +0.0% / +1.6% / +4.3% on the 74 / 150 / 304-stay instances.
        // Probability 0.8 and neighbourhood size 40 were both worse.
        double nearbyProbability = 0.4;
        int nearbySize = 20;

        std::function<void(std::int64_t, const Score&)> on_new_best;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stop_flag;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates stays[*].bed and stores the best score found
    // in plan.score. Every stay ends up with a value — a real bed index or
    // the "no bed" sentinel (plan.noBedValue()), never -1.
    void solve(BedPlan& plan);

    // Solves with a caller-assembled plugin calculator (see
    // ba/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies — construction heuristic,
    // late-acceptance local search, ALNS kicks and the date-window nearby
    // selection. Weights and levels come from the assembly (the components'
    // kDefaultWeight / kDefaultLevel unless the caller changed them). The
    // final plan.score is computed by THIS calculator (its three level
    // accumulators, calculator.level_scores()), and on_new_best reports every new
    // best the same way, so the last reported score equals plan.score. The
    // calculator is left primed on the final solution, so constraint_totals()
    // explains exactly that score. Single-start: the caller-owned calculator
    // cannot be cloned, so parallelSolverCount is ignored.
    void solve(BedPlan& plan, optimaru::DecomposedObjective<BedPlan>& calculator);

private:
    Config config_;
};

}  // namespace ba

#endif  // BA_SOLVER_HPP
