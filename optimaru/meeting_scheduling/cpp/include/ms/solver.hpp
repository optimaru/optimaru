#ifndef MS_SOLVER_HPP
#define MS_SOLVER_HPP

#include <utility>

#include "ms/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"

namespace ms {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<MeetingSchedule>, with the engine's adaptive
// ruin-and-recreate (ALNS) enabled on top of the built-in change/swap moves.
class Solver {
public:
    // Tuning history (benchmarks/meeting-scheduling/cpp/ms_quality_bench.cpp, 10s single-thread,
    // interleaved A/B on demo + scaled:48 + scaled:96, seeds 42..47; scaled:N
    // was an instance of the generator the bench had up to 0.5.0, which now
    // reads the comparison's instances from benchmarks/meeting-scheduling/dump_instances.py; the
    // objective is medium * 1e6 + soft, since the folded medium level
    // dominates on every non-trivial instance). Adopted: the late-acceptance
    // auto-scale below (+43% / +21% / +0.7%), the day-fit value recommender,
    // and destroy-repair as an ordinary move at 2e-4 (2026-08-18, seeds
    // 101..112: scaled:48 -19%, scaled:96 -8.7%, demo neutral — see
    // ruin_recreate_as_move_weight). Measured and REJECTED, do not retry without
    // new evidence:
    //   * near-current-grain value bias (the project-job delay-bias pattern):
    //     -3.5% / -9.1% — attendance conflicts are relieved by long jumps in
    //     the grain index, not by local nudges, so grain locality is harmful
    //     here even though the encoding is grain-major.
    //   * shared-attendee nearby neighborhoods biasing the swap partner:
    //     -32% / -9.4% — swapping two meetings that share attendees moves
    //     their conflict rather than removing it.
    //   * violation-guided entity_hint over hot (person, grain) /
    //     (room, grain) cells: -20% / -12%, and the O(1) hot-set maintenance
    //     alone cost ~28% of the move throughput.
    //   * move-mix sweeps (0.9/0.1, 0.5/0.5) and ALNS trigger sweeps
    //     (1500, 20000): all worse than the defaults (the mix below, the
    //     shared trigger of 5000).
    //
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0       clamp(128 * assignmentCount, 3200, 6400). Tuned
    //                          on the quality bench
    //                          (benchmarks/meeting-scheduling/cpp/ms_quality_bench.cpp,
    //                          10s single-thread, seeds 42..47): the medium
    //                          level (attendance conflicts) dominates the
    //                          objective and only a long acceptance history
    //                          lets the search tunnel between the conflict-free
    //                          packings, so the school-timetabling
    //                          clamp(8n, 400, 3200) was far too short here
    //                          (scaled:48 -26.7% objective, scaled:96 -13.6%
    //                          against this setting)
    //   parallel_solver_count  the engine's multi-start; ignored by the
    //                          constraint-assembly overload
    //   ruin_recreate          without the kicks the LA acceptor can lock into
    //                          a local optimum within milliseconds on small
    //                          instances and never escape
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Destroy-and-repair as an ORDINARY acceptor-judged move (the engine's
        // ruin_recreate.as_move_weight), in addition to the stagnation kick:
        // the probability per step that a small ruin+repair candidate is drawn
        // instead of a change/swap. One repair costs ~ruinCount x
        // grainRoomCount evaluations, so this must stay orders of magnitude
        // below 1 (school-timetabling's budget-share rule). Tuned on the
        // quality bench (10s, interleaved A/B, seeds 101..112): 2e-4 wins
        // scaled:48 -19% (8/12 seeds) and scaled:96 -8.7% (5/6), demo neutral
        // (-0.3%); 1e-4 only -2% and 3e-4/5e-4 noisier-to-worse at scaled:96,
        // 4e-4 worse at scaled:48 — the peak is flat 2e-4 across sizes, not
        // the 0.2/cells scaling rule. 0 disables.
        double ruin_recreate_as_move_weight = 2e-4;
        int ruin_recreate_as_move_max_ruin = 12;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Guided value selection: the probability that a change move draws its
        // starting grain from the meeting's "day-fit" grains — the starts whose
        // whole meeting fits inside one existing day (see solver.cpp). 0
        // disables the hook. 0.8 on the quality bench (scaled:48 +7.4%,
        // scaled:96 +8.4%, demo +0.7%); at 1.0 the search loses the
        // day-crossing states it needs as stepping stones and the gain is gone.
        double day_fit_value_probability = 0.8;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates meetingAssignments[*].grainRoom and stores the
    // best score found in schedule.score. Pinned assignments keep their
    // grainRoom.
    void solve(MeetingSchedule& schedule);

    // Solves with a caller-assembled plugin calculator (see
    // ms/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same search applies, day-fit value selection
    // included. Weights come from the assembly, NOT from the level
    // defaults. The final schedule.score is computed by THIS calculator and
    // the calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score. Single-start: the
    // caller-owned calculator cannot be cloned, so parallel_solver_count is
    // ignored.
    void solve(MeetingSchedule& schedule,
               optimaru::DecomposedObjective<MeetingSchedule>& calculator);

private:
    Config config_;
};

}  // namespace ms

#endif  // MS_SOLVER_HPP
