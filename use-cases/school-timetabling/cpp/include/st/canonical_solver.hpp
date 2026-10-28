#ifndef ST_CANONICAL_SOLVER_HPP
#define ST_CANONICAL_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "st/canonical_domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace st::canon
{

// Thin domain-side wrapper around the generic engine in include/optimaru/ for the
// canonical domain, following the st::Solver pattern. On top of the built-in
// change/swap moves and the engine's adaptive ruin-and-recreate (ALNS, on by
// default) it wires the domain guidance adopted on the track-2 bench:
//   - a valueRecommender that only proposes slots from the moved lesson's
//     allowedSlot table, so H5-violating slots are never even suggested;
//   - an entityRecommender preferring lessons involved in a hard violation
//     (the incremental calculator's hot sets — es guided-moves pattern);
//   - same-class nearby neighborhoods biasing swap partners to classmates
//     (H3-neutral swaps) and driving the LNS related ruin;
//   - a SmartClassSwapMove operator (best-of-k classmate swap in the soft
//     phase; builtin-equivalent single move in the hard phase);
//   - a construction-heuristic difficulty order (block members, team-teaching
//     and tightly-restricted lessons first — First Fit Decreasing).
// parallelSolverCount > 1 runs the independent worker threads
// (each with its own calculator + operator instances, seed + i).
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget (the CP-SAT package's SOLVER_TIME_LIMIT_SECONDS
        // analog).
        std::chrono::milliseconds spentLimit{std::chrono::seconds(30)};

        // Stop early once bestScore >= bestScoreLimit. Default is above every
        // achievable score, so it never fires (termination is by spentLimit).
        Score bestScoreLimit{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds unimprovedSpentLimit{0};
        std::int64_t stepCountLimit = 0;
        std::int64_t unimprovedStepCountLimit = 0;

        // Late-acceptance history length. 0 (the default) = auto: clamp(4 *
        // lessonCount, 400, 3200) — on the track-2 bench the small instance
        // prefers ~800 while the mid/large ones profit from 1600+, and 4x
        // the lesson count tracks that optimum across sizes.
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
        // engine's ALNS) — essential on dense instances where plain LA local
        // search locks into a local optimum.
        bool ruinRecreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.10;
        double ruinRecreateMaxRuinFraction = 0.30;
        int ruinRecreateMaxRuinCount = 60;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // Destroy-and-repair as an ORDINARY acceptor-judged move, on top of
        // the stagnation kick above (engine
        // RuinRecreateConfig::asMoveWeight). Probability that a step is a
        // small ruin + greedy repair instead of a sampled change/swap; a
        // rejected one is rolled back like any other move.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // Best-improvement descent over one nearby lesson pair (engine
        // LocalSearchConfig::descentWeight): enumerate every allowed period
        // for both lessons plus their swap, apply the best improving one,
        // repeat until the pair is locally optimal. Always improving, so it
        // bypasses the acceptor. One descent costs ~2 * periodCount score
        // evaluations, so the weight is a fraction of a percent.
        double descentWeight = 0.0;
        int descentMaxSweeps = 4;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double changeMoveWeight = 0.75;
        double swapMoveWeight = 0.25;

        // Violation-guided entity selection (the es guided-moves pattern):
        // with this probability the moved lesson is drawn from the lessons
        // currently involved in a hard violation (H3~H10 hot sets in the
        // incremental calculator). Falls back to uniform selection once
        // feasible. 0 disables the hook entirely.
        double hotLessonProbability = 0.7;

        // Smart same-class swap operator (the es SmartChangeMove pattern):
        // evaluate up to smartClassSwapCandidates random classmate swaps and
        // apply the best. Joins the adaptive move-operator roulette next to
        // the built-in change/swap mix.
        bool smartClassSwapMoves = true;
        int smartClassSwapCandidates = 8;

        // Best-of-k width of the re-assign move (partial solving only; see
        // ReassignMove). Wider than the swap operator's k on purpose: putting
        // a lesson back into a packed timetable needs a slot that collides
        // with nothing, and one random allowed slot almost never is — the
        // whole point of the operator is to look at enough of them.
        //
        // Swept on the track-3 bench (partial_M, 10s, seed 1), placed periods
        // / total objective: 8 -> 325 / 891748, 16 -> 328 / 862891,
        // 32 -> 342 / 722035, 64 -> 341 / 731519. Past ~32 each attempt costs
        // more score evaluations than the extra reach is worth.
        int reassignCandidates = 32;

        // Same-class nearby selection. Canonical instances are typically
        // fully packed per class (weekly hours == slot count), where a change
        // move almost always creates a class conflict (H3) and the productive
        // move is a swap between two lessons of the SAME class (class
        // occupancy is conflict-neutral by construction). The nearby distance
        // meter makes classmates each other's nearest neighbors, which biases
        // swap partners toward classmates and turns the engine's related ruin
        // into a same-class cluster ruin.
        bool sameClassNearby = true;
        // Probability that a swap partner is drawn from the nearby
        // (same-class) list instead of uniformly. Swept on the track-2 bench
        // instances (S/M/L x 3 seeds, 10s): 0.5 dominates 0.8/0.95 — enough
        // pull to keep dense instances feasible, enough diversity to keep the
        // small instances from over-focusing.
        double sameClassNearbyProbability = 0.5;

        // S1~S6 weights, copied onto the Timetable at the start of solve()
        // (mirrors the CP-SAT SolverConfig carrying ObjectiveWeights).
        ObjectiveWeights weights;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: copies config_.weights into table.weights, mutates
    // lessons[*].slot and stores the best score found in table.score. Pinned
    // lessons keep their assignment.
    void solve(Timetable &table);

    // Solves with a caller-assembled plugin calculator (see
    // st/canonical_constraints.hpp): pick components and weights, then pass
    // the assembly here (the es Solver pattern). The same guided search
    // applies — value guidance from the allowedSlot tables, violation
    // guidance (entity recommender + the smart same-class swap operator)
    // from the calculator's penalizedUnits()/membersPerUnit(). Soft weights
    // come from the assembly, NOT from config_.weights / table.weights. The
    // final table.score is computed by THIS calculator and the calculator is
    // left primed on the final solution, so constraintTotals() explains
    // exactly that score. Single-start: the caller-owned calculator cannot
    // be cloned, so parallelSolverCount is ignored.
    void solve(Timetable &table,
               optimaru::DecomposedScoreCalculator<Timetable> &calculator);

private:
    Config config_;
};

}  // namespace st::canon

#endif  // ST_CANONICAL_SOLVER_HPP
