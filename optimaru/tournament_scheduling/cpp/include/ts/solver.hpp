#ifndef TS_SOLVER_HPP
#define TS_SOLVER_HPP

#include <utility>

#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/solver_config.hpp"
#include "ts/domain.hpp"

namespace ts {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/. It
// wires up the incremental score calculator and delegates solving to
// optimaru::Solver<TournamentSchedule>, with the engine's built-in
// change/swap moves, adaptive ruin-and-recreate (ALNS) and the domain's
// TeamCountMove enabled.
class Solver {
public:
    // The limits, the seed, the parallel starts, the callback and the ruin &
    // recreate knobs are the shared ones of optimaru/solver_config.hpp. Here:
    //   history_length 0  clamp(8 * assignmentCount, 400, 3200), the scaling
    //                     adopted on the school-timetabling quality bench
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Engine strategy plugins (see core SearchSettings for the model).
        // descent_weight: probability a step runs a best-improvement descent on
        // a nearby entity pair instead of the move mix (a sweep costs
        // ~2 * teamCount evaluations here — cheap at this value range).
        double descent_weight = 0.0;
        // Destroy-and-repair as an ordinary acceptor-judged move (distinct
        // from the rare unconditional stagnation kick). 0 disables.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;
        // Gate for caller-registered custom operators (TeamCountMove): the
        // probability a step may draw them at all. 1 = ungated.
        double custom_move_probability = 1.0;

        // Guided change values: steer change moves toward teams that keep
        // the move hard-feasible — not already playing that day and not
        // unavailable on it. The default path reads them off the incremental
        // calculator's live counters, the picked-constraints path counts the
        // day's teams off the working solution; both pick the same team.
        // Uniform draws waste most proposals on hard-violating teams once the
        // schedule is feasible.
        bool value_hint = false;

        // Loaded-team-count macro moves (adopted on the quality bench — see
        // solver.cpp). assignmentCountImbalanceCost is an unfairness over the
        // LOADED teams only, so spreading the same assignments evenly over a
        // different number of teams can be a whole medium level better — a
        // jump no single-variable move reaches, because emptying (or
        // refilling) a team passes through wildly unfair intermediate states.
        // This operator reshapes the schedule onto a promising loaded-team
        // count in one move and lets the acceptor judge the result. It runs on
        // both solve() overloads.
        bool team_count_moves = true;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: mutates teamAssignments[*].team and stores the best
    // score found in schedule.score. Pinned assignments keep their team.
    void solve(TournamentSchedule& schedule);

    // Solves with a caller-assembled plugin calculator (see
    // ts/constraints.hpp): pick components and weights, then pass the
    // assembly here. Weights come from the assembly, NOT from the
    // level defaults. The final schedule.score is computed by THIS calculator
    // and the calculator is left primed on the final solution, so
    // constraint_totals() explains exactly that score.
    //
    // One knob does not apply on this path: parallel_solver_count (the
    // caller-owned calculator cannot be cloned across workers). The search is
    // the default path's: the engine's construction heuristic +
    // late-acceptance local search + ALNS kicks, TeamCountMove under
    // team_count_moves and the guided change values under value_hint, both
    // keeping their own counts instead of reading the monolithic
    // calculator's. The same seed and steps on the default constraints end
    // on the same schedule as solve(schedule).
    void solve(TournamentSchedule& schedule,
               optimaru::DecomposedObjective<TournamentSchedule>& calculator);

private:
    Config config_;
};

}  // namespace ts

#endif  // TS_SOLVER_HPP
