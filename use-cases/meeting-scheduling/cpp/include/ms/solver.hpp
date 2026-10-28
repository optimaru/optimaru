#ifndef MS_SOLVER_HPP
#define MS_SOLVER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "ms/domain.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"

namespace ms
{

// Thin domain-side wrapper around the generic engine in include/optimaru/ — the C++
// analog of a quickstart building a SolverFactory from a SolverConfig. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<MeetingSchedule>, with the engine's adaptive
// ruin-and-recreate (ALNS) enabled on top of the built-in change/swap moves.
class Solver
{
public:
    // Tuning history (bench/ms_quality_bench.cpp, 10s single-thread,
    // interleaved A/B on demo + scaled:48 + scaled:96, seeds 42..47; the
    // objective is medium * 1e6 + soft, since the folded medium level
    // dominates on every non-trivial instance). Adopted: the late-acceptance
    // auto-scale below (+43% / +21% / +0.7%), the day-fit value recommender,
    // and destroy-repair as an ordinary move at 2e-4 (2026-08-18, seeds
    // 101..112: scaled:48 -19%, scaled:96 -8.7%, demo neutral — see
    // ruinRecreateAsMoveWeight). Measured and REJECTED, do not retry without
    // new evidence:
    //   * near-current-grain value bias (the project-job delay-bias pattern):
    //     -3.5% / -9.1% — attendance conflicts are relieved by long jumps in
    //     the grain index, not by local nudges, so grain locality is harmful
    //     here even though the encoding is grain-major.
    //   * shared-attendee nearby neighborhoods biasing the swap partner:
    //     -32% / -9.4% — swapping two meetings that share attendees moves
    //     their conflict rather than removing it.
    //   * violation-guided entityRecommender over hot (person, grain) /
    //     (room, grain) cells: -20% / -12%, and the O(1) hot-set maintenance
    //     alone cost ~28% of the move throughput.
    //   * move-mix sweeps (0.9/0.1, 0.5/0.5) and ALNS trigger sweeps
    //     (1500, 20000): all worse than the defaults below.
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
        // clamp(128 * assignmentCount, 3200, 6400). Tuned on the quality bench
        // (bench/ms_quality_bench.cpp, 10s single-thread, seeds 42..47): the
        // medium level (attendance conflicts) dominates the objective and only
        // a long acceptance history lets the search tunnel between the
        // conflict-free packings, so the school-timetabling clamp(8n, 400,
        // 3200) was far too short here (scaled:48 -26.7% objective, scaled:96
        // -13.6% against this setting).
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

        // Destroy-and-repair as an ORDINARY acceptor-judged move (the engine's
        // lns.asMoveWeight), in addition to the stagnation kick above: the
        // probability per step that a small ruin+repair candidate is drawn
        // instead of a change/swap. One repair costs ~ruinCount x
        // grainRoomCount evaluations, so this must stay orders of magnitude
        // below 1 (school-timetabling's budget-share rule). Tuned on the
        // quality bench (10s, interleaved A/B, seeds 101..112): 2e-4 wins
        // scaled:48 -19% (8/12 seeds) and scaled:96 -8.7% (5/6), demo neutral
        // (-0.3%); 1e-4 only -2% and 3e-4/5e-4 noisier-to-worse at scaled:96,
        // 4e-4 worse at scaled:48 — the peak is flat 2e-4 across sizes, not
        // the 0.2/cells scaling rule. 0 disables.
        double ruinRecreateAsMoveWeight = 2e-4;
        int ruinRecreateAsMoveMaxRuin = 12;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double changeMoveWeight = 0.75;
        double swapMoveWeight = 0.25;

        // Guided value selection: the probability that a change move draws its
        // starting grain from the meeting's "day-fit" grains — the starts whose
        // whole meeting fits inside one existing day (see solver.cpp). 0
        // disables the hook. 0.8 on the quality bench (scaled:48 +7.4%,
        // scaled:96 +8.4%, demo +0.7%); at 1.0 the search loses the
        // day-crossing states it needs as stepping stones and the gain is gone.
        double dayFitValueProbability = 0.8;

        std::function<void(std::int64_t, const Score &)> onNewBest;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates meetingAssignments[*].grainRoom and stores the
    // best score found in schedule.score. Pinned assignments keep their
    // grainRoom.
    void solve(MeetingSchedule &schedule);

    // Solves with a caller-assembled plugin calculator (see
    // ms/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, day-fit value selection
    // included. Weights come from the assembly, NOT from the quickstart level
    // defaults. The final schedule.score is computed by THIS calculator and
    // the calculator is left primed on the final solution, so
    // constraintTotals() explains exactly that score. Single-start: the
    // caller-owned calculator cannot be cloned, so parallelSolverCount is
    // ignored.
    void
    solve(MeetingSchedule &schedule,
          optimaru::DecomposedScoreCalculator<MeetingSchedule> &calculator);

private:
    Config config_;
};

}  // namespace ms

#endif  // MS_SOLVER_HPP
