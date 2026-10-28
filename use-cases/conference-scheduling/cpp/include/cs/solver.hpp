#ifndef CS_SOLVER_HPP
#define CS_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "cs/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace cs
{

// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++
// analog of a quickstart building a SolverFactory from a SolverConfig. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<ConferenceSchedule>. On top of the built-in change/swap
// moves and the engine's adaptive ruin-and-recreate (ALNS) it biases value
// selection toward the talk's type-compatible (timeslot, room) cells — the
// engine's global value range stands in for the quickstart's per-TalkType
// value ranges, with the synthetic talkTypeCompatibility hard constraint
// penalizing the stragglers (see cs/domain.hpp) — and it spends the budget as
// a sequence of restarted local-search runs (see restartSegments).
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
        // clamp(8 * talkCount, 800, 3200). The 8x-entities scaling is the
        // school-timetabling quality-bench result; the 800 floor was measured
        // on this domain's quality bench (demo +3-5% soft vs a 400 floor over
        // seeds 42-47, scaled-45 neutral-positive, no hard regressions).
        std::size_t lateAcceptanceSize = 0;
        std::uint64_t randomSeed = 0;

        // Iterated local search: how many consecutive local-search runs share
        // the one spentLimit. Each run restarts from the previous run's best
        // with a fresh late-acceptance history, a fresh ALNS kick interval and
        // its own random stream, so the search keeps diversifying for the
        // whole budget instead of settling in the first third of it. 1 = a
        // single run. Ignored when a step- or unimproved-based termination is
        // configured (those count per run).
        int restartSegments = 8;

        // Independent parallel starts: each worker receives randomSeed + i
        // and the full wall-clock budget (segmented as above); the best
        // solution wins.
        int parallelSolverCount = 1;

        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::EnvironmentMode environmentMode
            = optimaru::EnvironmentMode::NON_ASSERT;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS).
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

        // Destroy-and-repair as an ORDINARY acceptor-judged move (engine
        // RuinRecreateConfig::asMoveWeight): probability that a step is a
        // small ruin + greedy repair instead of a sampled change/swap; a
        // rejected one is rolled back like any other move. Cost per draw is
        // ~ruinCount * slotRoomCount score evaluations, so the school-
        // timetabling budget-share rule applies: weight * slotRoomCount
        // ~ 0.2. Negative (the default) = auto: 0.2 / slotRoomCount, the
        // budget-share value, which this domain's quality bench confirmed
        // (10s, 6 seeds: soft +0.9-1.4% on every scaled size, 6-0 wins on
        // scaled:45, no regressions — see benchmarks/school-timetabling/RESULTS.md). 0 disables.
        double ruinRecreateAsMoveWeight = -1.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // Best-improvement descent over one entity pair (engine
        // LocalSearchConfig::descentWeight); same budget-share arithmetic as
        // the as-move weight. Off by default.
        double descentWeight = 0.0;

        // Violation-guided entity selection (school-timetabling's
        // hot-lesson recommender, sampling-based): with this probability the
        // moved talk is drawn from talks whose current assignment visibly
        // carries hard units (see
        // ConferenceSchedulingIncrementalScoreCalculator::isHotTalk), by
        // rejection-sampling a few uniform draws. Once feasible no talk is
        // hot and selection falls back to uniform by itself. 0 disables.
        double hotTalkProbability = 0.0;

        // Probability that a change move's target value is drawn from the
        // talk's type-compatible (timeslot, room) cells instead of the whole
        // grid. 1.0 never proposes incompatible cells by change move (swaps
        // between talks of different types still can — the synthetic hard
        // constraint cleans those up). 0 disables the bias.
        double compatibleValueProbability = 0.9;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates talks[*].slotRoom and stores the best score
    // found in schedule.score. Pinned talks keep their assignment.
    void solve(ConferenceSchedule &schedule);

    // Solves with a caller-assembled plugin calculator (see
    // cs/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights come from the assembly, NOT from the Java
    // provider defaults. The final schedule.score is computed by THIS
    // calculator and the calculator is left primed on the final solution, so
    // constraintTotals() explains exactly that score.
    //
    // Two knobs do not apply on this path: parallelSolverCount and the
    // segmented restarts. Both need a calculator per start / per segment, and
    // a caller-owned assembly cannot be cloned; the plugin path runs one
    // segment, one start. Note also that this path re-costs the whole
    // schedule once per score (see cs/constraints/common.hpp) — it is the
    // composability path, not the throughput path.
    void
    solve(ConferenceSchedule &schedule,
          optimaru::DecomposedScoreCalculator<ConferenceSchedule> &calculator);

private:
    Config config_;
};

}  // namespace cs

#endif  // CS_SOLVER_HPP
