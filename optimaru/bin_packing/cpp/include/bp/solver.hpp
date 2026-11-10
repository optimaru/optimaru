#ifndef BP_SOLVER_HPP
#define BP_SOLVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "bp/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"

namespace bp
{

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/:
// the domain construction (best-fit decreasing, bp/construction.hpp), then
// optimaru::Solver<BinPackingProblem> with late acceptance over the built-in
// change/swap moves, the domain's EmptyBinMove and RepackMove
// (bp/moves.hpp) and the engine's adaptive ruin-and-recreate (ALNS).
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget. 0 = no search: the construction (or the seed,
        // completed where an item is still -1) comes back as it is.
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
        // clamp(8 * itemCount, 400, 3200).
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts: worker i searches its own
        // construction with seed + i and the full wall-clock budget;
        // the best solution wins (the lowest worker on ties). Worker 0
        // constructs in weight-descending order, worker i > 0 shuffles the
        // items of equal weight (a seed is completed the same way in every
        // worker).
        int parallelSolverCount = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::ScoreChecks score_checks
            = optimaru::ScoreChecks::Off;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS); the recreate places each ruined item in its best
        // bin.
        bool ruin_recreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // Destroy-and-repair as an ORDINARY acceptor-judged move (the
        // engine's ruin_recreate.as_move_weight); one costs ~ruinCount * n score
        // evaluations. 0 disables it.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Guided change-move targets (engine local_search.value_hint):
        // with this probability the change move draws the item's new bin as
        // the bin of a uniformly drawn item — a bin in use, weighted by how
        // many items it holds — instead of uniformly over all n bins, most of
        // which are empty in a good packing. 0 = off.
        double valueRecommenderProbability = 0.5;

        // Assignment best-improvement descent strategy (engine
        // local_search.descent_weight). 0 (default) = off.
        double descent_weight = 0.0;

        // Upper gate on how often a step may draw one of the domain's macro
        // moves (EmptyBinMove, RepackMove) instead of the ordinary mix; the
        // adaptive selector still chooses inside the gate. 1 = unrestricted
        // (engine default).
        double custom_move_probability = 1.0;

        // EmptyBinMove: empty one light bin in a single move (see
        // bp/moves.hpp). usedBinsCost is a step function no single-item move
        // can cross, so without it the search rarely closes a bin.
        bool emptyBinMoves = true;
        // Both macro moves empty one of this many lightest non-empty bins
        // (drawn uniformly; 1 = always the lightest).
        int emptyBinCandidates = 3;

        // RepackMove (bp/moves.hpp): empty a light bin together with 1 to
        // repackPartnerBins other light bins and put their items back best
        // fit decreasing, only where they add no capacity, colour or
        // conflict violation. EmptyBinMove checks the capacity only, so on
        // instances with colours or conflicts this is the move that closes
        // bins. repackPartnerBins 0 = the source bin alone.
        bool repackMoves = true;
        int repackPartnerBins = 2;

        std::function<void(std::int64_t, const Score &)> on_new_best;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stop_flag;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: sets items[*].bin and problem.score, clears
    // warmStart. A plain solve (warmStart false) ignores the input bins; a
    // warm start keeps them and completes the items still at -1. When the
    // search moved anything, the bins are relabelled 0..k-1 by first
    // appearance (score-neutral), so a complete seed with a zero budget
    // comes back exactly as it was, gaps included.
    // Throws std::invalid_argument for binCapacity <= 0, a repeated item
    // id, a weight <= 0, a bad conflict or (warm start) a bin outside
    // -1..n-1.
    void solve(BinPackingProblem &problem);

    // Solves with a caller-assembled plugin calculator (see
    // bp/constraints.hpp): the same construction and search, scored by the
    // assembly (custom weights, levels and exponents included). The final
    // problem.score is computed by THIS calculator and the calculator is left
    // primed on the final solution, so constraint_totals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be
    // cloned, so parallelSolverCount is ignored.
    void solve(BinPackingProblem &problem,
               optimaru::DecomposedObjective<BinPackingProblem> &calculator);

private:
    Config config_;
};

}  // namespace bp

#endif  // BP_SOLVER_HPP
