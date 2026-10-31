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
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace bp
{

// Thin domain-side wrapper around the generic engine in include/optimaru/:
// the domain construction (best-fit decreasing, bp/construction.hpp), then
// optimaru::Solver<BinPackingProblem> with late acceptance over the built-in
// change/swap moves, the domain's EmptyBinMove and the engine's adaptive
// ruin-and-recreate (ALNS).
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget. 0 = no search: the construction (or the seed,
        // completed where an item is still -1) comes back as it is.
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
        // clamp(8 * itemCount, 400, 3200).
        std::size_t lateAcceptanceSize = 0;
        std::uint64_t randomSeed = 0;

        // Independent parallel starts: worker i searches its own
        // construction with randomSeed + i and the full wall-clock budget;
        // the best solution wins (the lowest worker on ties). Worker 0
        // constructs in weight-descending order, worker i > 0 shuffles the
        // items of equal weight (a seed is completed the same way in every
        // worker).
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::EnvironmentMode environmentMode
            = optimaru::EnvironmentMode::NON_ASSERT;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS); the recreate places each ruined item in its best
        // bin.
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // Destroy-and-repair as an ORDINARY acceptor-judged move (the
        // engine's ruinRecreate.asMoveWeight); one costs ~ruinCount * n score
        // evaluations. 0 disables it.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double changeMoveWeight = 0.75;
        double swapMoveWeight = 0.25;

        // Guided change-move targets (engine localSearch.valueRecommender):
        // with this probability the change move draws the item's new bin as
        // the bin of a uniformly drawn item — a bin in use, weighted by how
        // many items it holds — instead of uniformly over all n bins, most of
        // which are empty in a good packing. 0 = off.
        double valueRecommenderProbability = 0.5;

        // BASIC best-improvement descent strategy (engine
        // localSearch.descentWeight). 0 (default) = off.
        double descentWeight = 0.0;

        // Upper gate on how often a step may draw a caller-registered macro
        // move (EmptyBinMove) instead of the ordinary mix; the adaptive
        // selector still chooses inside the gate. 1 = unrestricted (engine
        // default).
        double customMoveProbability = 1.0;

        // EmptyBinMove: empty one light bin in a single move (see
        // solver.cpp). usedBinsCost is a step function no single-item move
        // can cross, so without it the search rarely closes a bin.
        bool emptyBinMoves = true;
        // The move empties one of this many lightest non-empty bins (drawn
        // uniformly; 1 = always the lightest, the 0.1.5 rule).
        int emptyBinCandidates = 3;

        std::function<void(std::int64_t, const Score &)> onNewBest;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stopFlag;
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
    // primed on the final solution, so constraintTotals() explains exactly
    // that score. Single-start: the caller-owned calculator cannot be
    // cloned, so parallelSolverCount is ignored.
    void solve(BinPackingProblem &problem,
               optimaru::DecomposedScoreCalculator<BinPackingProblem> &calculator);

private:
    Config config_;
};

}  // namespace bp

#endif  // BP_SOLVER_HPP
