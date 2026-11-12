#ifndef CS_SOLVER_HPP
#define CS_SOLVER_HPP

#include <utility>

#include "cs/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace cs {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<ConferenceSchedule>. On top of the built-in change/swap
// moves and the engine's adaptive ruin-and-recreate (ALNS) it biases value
// selection toward the talk's type-compatible (timeslot, room) cells — the
// engine's value range is the whole grid for every talk, with the
// talkTypeIncompatibilityCost hard constraint penalizing the stragglers
// (see cs/domain.hpp) — and it spends the
// budget as a sequence of restarted local-search runs (see restart_segments).
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0       clamp(8 * talkCount, 800, 3200). The 8x-entities
    //                          scaling is the school-timetabling quality-bench
    //                          result; the 800 floor was measured on this
    //                          domain's quality bench (demo +3-5% soft vs a 400
    //                          floor over seeds 42-47, scaled-45
    //                          neutral-positive, no hard regressions)
    //   parallel_solver_count  every start gets the full wall-clock budget,
    //                          segmented as restart_segments says
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Iterated local search: how many consecutive local-search runs share
        // the one time_limit. Each run restarts from the previous run's best
        // with a fresh late-acceptance history, a fresh ALNS kick interval and
        // its own random stream, so the search keeps diversifying for the
        // whole budget instead of settling in the first third of it. 1 = a
        // single run. Ignored when a step- or unimproved-based termination is
        // configured (those count per run).
        int restart_segments = 8;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Destroy-and-repair as an ORDINARY acceptor-judged move (engine
        // RuinRecreateSettings::as_move_weight): probability that a step is a
        // small ruin + greedy repair instead of a sampled change/swap; a
        // rejected one is rolled back like any other move. Cost per draw is
        // ~ruinCount * slotRoomCount score evaluations, so the school-
        // timetabling budget-share rule applies: weight * slotRoomCount
        // ~ 0.2. Negative (the default) = auto: 0.2 / slotRoomCount, the
        // budget-share value, which this domain's quality bench confirmed
        // (10s, 6 seeds: soft +0.9-1.4% on every scaled size, 6-0 wins on
        // scaled:45, no regressions — see benchmarks/school-timetabling/RESULTS.md;
        // scaled:N was an instance of the generator the bench had up to 0.5.0). 0 disables.
        double ruin_recreate_as_move_weight = -1.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Best-improvement descent over one entity pair (engine
        // SearchSettings::descent_weight); same budget-share arithmetic as
        // the as-move weight. Off by default.
        double descent_weight = 0.0;

        // Violation-guided entity selection (school-timetabling's
        // hot-lesson recommender, sampling-based): with this probability the
        // moved talk is drawn from talks whose current assignment visibly
        // carries hard units (see
        // ConferenceSchedulingIncrementalScoreCalculator::isHotTalk), by
        // rejection-sampling a few uniform draws. Once feasible no talk is
        // hot and selection falls back to uniform by itself. 0 disables.
        double hot_talk_probability = 0.0;

        // Probability that a change move's target value is drawn from the
        // talk's type-compatible (timeslot, room) cells instead of the whole
        // grid. 1.0 never proposes incompatible cells by change move (swaps
        // between talks of different types still can — the hard
        // constraint cleans those up). 0 disables the bias.
        double compatible_value_probability = 0.9;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates talks[*].slotRoom and stores the best score
    // found in schedule.score. Pinned talks keep their assignment.
    void solve(ConferenceSchedule& schedule);

    // Solves with a caller-assembled plugin calculator (see
    // cs/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights come from the assembly, NOT from the
    // component defaults. The final schedule.score is computed by THIS
    // calculator and the calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score.
    //
    // Two knobs do not apply on this path: parallel_solver_count and the
    // segmented restarts. Both need a calculator per start / per segment, and
    // a caller-owned assembly cannot be cloned; the plugin path runs one
    // segment, one start. Note also that this path re-costs the whole
    // schedule once per score (see cs/constraints/common.hpp) — it is the
    // composability path, not the throughput path.
    void solve(ConferenceSchedule& schedule,
               optimaru::DecomposedObjective<ConferenceSchedule>& calculator);

private:
    Config config_;
};

}  // namespace cs

#endif  // CS_SOLVER_HPP
