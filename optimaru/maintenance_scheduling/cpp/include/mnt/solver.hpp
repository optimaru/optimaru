#ifndef MNT_SOLVER_HPP
#define MNT_SOLVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "mnt/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"

namespace mnt
{

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<MaintenanceSchedule>, with the engine's built-in
// change/swap moves and adaptive ruin-and-recreate (ALNS) enabled (the
// school-timetabling pattern).
class Solver
{
public:
    struct Config
    {
        // Wall-clock budget (30s by default).
        std::chrono::milliseconds time_limit{std::chrono::seconds(30)};

        // Stop early once bestScore >= target_score. Default is above
        // every achievable score, so it never fires (termination is by
        // time_limit).
        Score target_score{std::numeric_limits<std::int64_t>::max(),
                             std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds stall_time_limit{0};
        std::int64_t step_limit = 0;
        std::int64_t stall_step_limit = 0;

        // Late-acceptance history length. 0 (the default) = auto:
        // clamp(8 * jobCount, 400, 3200) — instances here are small (dozens
        // of jobs), so this normally resolves to 400.
        std::size_t history_length = 0;
        std::uint64_t seed = 0;

        // Independent parallel starts (the engine's multi-start): each
        // worker receives seed + i and the full wall-clock budget; the
        // best solution wins.
        int parallelSolverCount = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; orders of magnitude
        // slower).
        optimaru::ScoreChecks score_checks
            = optimaru::ScoreChecks::Off;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Without this the LA acceptor can lock into a local
        // optimum within milliseconds on small instances and never escape.
        // Fractions/cap retuned for this domain (measured): the guided value
        // selection narrows the neighborhood, so the search converges sooner
        // and needs bigger, but absolutely bounded, kicks to keep escaping.
        bool ruin_recreate = true;
        std::int64_t ruinRecreateUnimprovedStepTrigger = 5000;
        double ruinRecreateMinRuinFraction = 0.20;
        double ruinRecreateMaxRuinFraction = 0.60;
        int ruinRecreateMaxRuinCount = 24;
        double ruinRecreateOperatorDecay = 0.8;
        std::vector<double> ruinRecreateRewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruinRecreatePolishSteps = 5000;

        // BASIC move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Destroy-and-repair as an ORDINARY acceptor-judged move, on top of
        // the stagnation kick above (engine
        // RuinRecreateSettings::as_move_weight): probability that a step is a
        // small ruin + greedy best-placement repair instead of a sampled
        // change/swap; a rejected one is rolled back like any other move.
        // 0 disables (measured: no win on the demo instances at 10s), a
        // positive value is the per-step draw probability.
        double ruinRecreateAsMoveWeight = 0.0;
        int ruinRecreateAsMoveMaxRuin = 12;

        // Best-improvement descent over one entity pair (engine
        // SearchSettings::descent_weight): enumerate every crewDate for
        // both jobs plus their swap, apply the best improving one, repeat
        // until the pair is locally optimal. 0 disables (measured: loses
        // budget on the demo instances at 10s).
        double descent_weight = 0.0;
        int descent_max_sweeps = 4;

        // --- guided selection (see solver.cpp) ---
        // Probability that a change move draws the date half of the composite
        // value from the job's own [minStart, idealEnd] window instead of the
        // whole (date x crew) grid. 1.0 (the default, measured best) always
        // guides; 0 disables the guidance.
        double windowValueProbability = 1.0;
        // Probability that a move seeds on a "hot" job — one currently inside
        // a tagConflictCost (two jobs sharing a tag on one day). 0 disables.
        double hotJobProbability = 0.5;
        // How many random crews a guided change move tries before settling
        // for a random one, looking for a crew that is free for the job's
        // whole span. 0 disables (uniform crew).
        int freeCrewCandidates = 4;

        std::function<void(std::int64_t, const Score &)> on_new_best;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stop_flag;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates jobs[*].crewDate and stores the best score
    // found in schedule.score. Pinned jobs keep their assignment.
    void solve(MaintenanceSchedule &schedule);

    // Solves with a caller-assembled plugin calculator (see
    // mnt/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights and levels come from the assembly (each
    // component's kDefaultWeight / kDefaultLevel unless the caller changed
    // them). The final schedule.score is computed by THIS calculator and
    // the calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score.
    //
    // Three knobs do not apply on this path: parallelSolverCount (the
    // caller-owned calculator cannot be cloned across workers) and the two
    // guided-selection hooks (windowValueProbability / hotJobProbability),
    // which read the monolithic calculator's live occupancy and hot-cell
    // state. The search is the engine's construction heuristic +
    // late-acceptance local search + ALNS kicks.
    void
    solve(MaintenanceSchedule &schedule,
          optimaru::DecomposedObjective<MaintenanceSchedule> &calculator);

private:
    Config config_;
};

}  // namespace mnt

#endif  // MNT_SOLVER_HPP
