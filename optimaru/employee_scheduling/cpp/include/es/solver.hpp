#ifndef ES_SOLVER_HPP
#define ES_SOLVER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "es/domain.hpp"
#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"  // ScoreChecks

namespace es {

// Thin domain-side wrapper around the generic engine. It wires up the domain
// pieces (the incremental score calculator and the guided move-selection
// heuristics) and delegates solving to
// optimaru::Solver<EmployeeSchedule>.
class Solver {
public:
    struct Config {
        std::chrono::milliseconds time_limit{std::chrono::seconds(30)};

        // Stop early once bestScore >= target_score. Default is above every
        // achievable score, so it never fires (termination is by time_limit).
        Score target_score{std::numeric_limits<std::int64_t>::max(),
                           std::numeric_limits<double>::infinity()};
        // Optional early exits; 0 disables each criterion.
        std::chrono::milliseconds stall_time_limit{0};
        std::int64_t step_limit = 0;
        std::int64_t stall_step_limit = 0;

        std::size_t history_length = 400;
        std::uint64_t seed = 0;

        // Fraction of the total time budget reserved for the post-local-search
        // greedy hard-repair. Local search runs for time_limit*(1-fraction) and
        // the repair gets the rest, so total wall time still respects
        // time_limit. Clamped to [0, 0.9).
        //
        // Default 0 (off): local search already returns its best-so-far — the
        // least-infeasible solution it found. The extra greedy pass was measured
        // not to improve on that best (a single relocation cannot dent
        // violations spread over a worker's whole run) and its reserve only
        // costs local-search time. Kept as a knob for instances where local
        // search ends a few points short of feasible and a final greedy push
        // closes the gap.
        double greedy_repair_budget_fraction = 0.0;

        // Independent parallel starts. Each worker receives seed + i
        // and the full wall-clock budget; the best solution wins. This is
        // generally more scalable than synchronizing candidate evaluation
        // around one mutable incremental score editor.
        // Only solve(schedule) honors this; solve(schedule, calculator) ignores
        // it and runs one search. Python goes through the calculator overload
        // and gets its parallel starts from separate processes instead (see
        // optimaru.employee_scheduling.models.solve_request).
        int parallel_solver_count = 1;

        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (a debugging aid for the C++ tests;
        // orders of magnitude slower, not exposed to Python).
        optimaru::ScoreChecks score_checks = optimaru::ScoreChecks::Off;

        // Guided move selection (A: pick a skill-capable, available target
        // employee; B: bias the shift toward employees with hard violations).
        // Set false for plain uniform-random move selection.
        bool guided_moves = true;
        bool employee_greedy_construction = true;

        // Adaptive ruin-and-recreate kicks when the search stagnates (the
        // engine's ALNS). Without this the late-acceptance search can lock into a local
        // optimum within milliseconds on small instances and never escape.
        bool ruin_recreate = true;
        std::int64_t ruin_recreate_unimproved_step_trigger = 5000;
        double ruin_recreate_min_ruin_fraction = 0.10;
        double ruin_recreate_max_ruin_fraction = 0.30;
        int ruin_recreate_max_ruin_count = 60;
        double ruin_recreate_operator_decay = 0.8;
        std::vector<double> ruin_recreate_rewards{33.0, 9.0, 3.0, 0.0};
        std::int64_t ruin_recreate_polish_steps = 5000;

        // Destroy-and-repair as an ORDINARY acceptor-judged move, on top of
        // the stagnation kick above (engine
        // RuinRecreateSettings::as_move_weight). Probability that a step is a
        // small ruin + greedy repair instead of a sampled change/swap; a
        // rejected one is rolled back like any other move. It draws from the
        // same adaptive ruin-operator pool as the kick, so with
        // employee_ruin_moves on the domain's own employeeHotRuin is one of the
        // operators it can pick — the difference from the kick is the
        // cadence (every step by weight, not on stagnation) and that the
        // acceptor gets a veto.
        double ruin_recreate_as_move_weight = 0.0;
        int ruin_recreate_as_move_max_ruin = 12;

        // Best-improvement descent over one nearby shift pair (engine
        // SearchSettings::descent_weight): enumerate every employee for
        // both shifts plus their swap, apply the best improving one, repeat
        // until the pair is locally optimal. Always improving, so it bypasses
        // the acceptor. Picks its first shift through the same violation
        // guidance as the change moves when guided_moves is on.
        //
        // One descent costs ~2 * employeeCount score evaluations, which is
        // what makes it affordable HERE: the value range is the employee
        // list, usually orders of magnitude smaller than a routing domain's
        // positions or a timetable's slot x room grid. A sweep picked 0.02:
        // a clear gain on large rosters, a wash on small ones. Past ~0.05 the
        // descent starts starving the ordinary move mix and the gain shrinks.
        double descent_weight = 0.02;
        int descent_max_sweeps = 4;

        // Temporal nearby selection (engine NeighborSettings): distance
        // between two shifts is |start - start| in minutes, so swap partners,
        // descent pairs and the LNS related-ruin cluster stay inside the time
        // window where the pairwise hard constraints (overlap, one-per-day,
        // min-rest) actually interact. Neighbor-list size is auto-derived from
        // the densest 24h window of the instance. Same pattern as
        // school-timetabling's adopted same-group nearby.
        bool nearby_shifts = true;
        // Default 0.65: measured best compromise across roster sizes. 0.8 wins
        // bigger on rosters of ~1000 shifts but costs small rosters their soft
        // budget once hard converges (same-day swaps cannot move the date-based
        // soft terms). For large-only workloads prefer 0.8.
        double nearby_shifts_probability = 0.65;

        // Assignment move mix. Weights are relative; 0 disables a move.
        double change_weight = 0.75;
        double swap_weight = 0.25;

        // Employee-scheduling-specific assignment operators registered through the
        // engine's custom move hooks. They complement the generic change/swap
        // moves with stronger domain moves while keeping the same score
        // calculator and acceptor protocol.
        bool smart_change_moves = false;
        int smart_change_candidate_count = 6;
        bool employee_day_repair_moves = true;
        bool employee_run_repair_moves = true;
        int employee_run_repair_max_days = 3;
        bool employee_window_moves = true;
        int employee_window_max_days = 3;
        // Cap how often the adaptive custom-operator pool gets a turn. Window
        // swaps are much costlier than sampled change/swap moves and can
        // otherwise monopolize the selector after one reward. 0.30-0.40 is a
        // plateau in interleaved A/B runs across roster sizes. Re-measure if
        // move economics shift.
        double custom_move_probability = 0.30;
        bool employee_ruin_moves = true;

        // Allow the search to leave a shift UNASSIGNED (employeeIndex = -1).
        // The default fills every shift (the value range is the employee list,
        // so no move ever produces -1); enable this to let local search empty a
        // slot when that improves the score — needed when shifts are optional
        // coverage slots (staffing floor via shift_coverage_cost) rather than
        // fixed demand. Default off: with the unassigned_shifts_cost hard rule
        // (the usual setup) an empty slot is penalized and refilled anyway.
        bool employee_unassign_moves = false;

        // Sequence move mix. Exposed for parity with the generic engine; employee
        // scheduling is assignment-shaped, so these do not affect this domain.
        double relocate_weight = 0.5;
        double list_swap_weight = 0.3;
        double two_opt_weight = 0.2;
        double or_opt_weight = 0.0;
        double tail_swap_weight = 0.0;

        std::function<void(std::int64_t, const Score&)> on_new_best;
        // Set by solve() on each parallel worker's copy, not by a caller (and
        // not bound to Python): the first worker that fails raises it, and the
        // other workers' engines stop at their next step instead of running out
        // their budget before the error reaches the caller.
        std::shared_ptr<std::atomic<bool>> stop_flag;
    };

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves with the built-in monolithic incremental calculator (the fixed
    // default constraints, the hand-tuned fast path).
    void solve(EmployeeSchedule& schedule);

    // Solves with a caller-assembled plugin calculator (see es/constraints.hpp):
    // pick constraints and weights, then pass the assembly here. The same
    // guided move selection applies — violation guidance comes from the
    // calculator's penalized_units(). The final schedule.score is computed by
    // THIS calculator (custom weights included), and the calculator is left
    // primed on the final solution so constraint_totals() explains the score.
    // Structurally compatible delta constraints are grouped automatically;
    // unsupported constraints remain on the individual generic delta path.
    // Always one search: Config::parallel_solver_count is ignored here.
    void solve(EmployeeSchedule& schedule,
               optimaru::DecomposedObjective<EmployeeSchedule>& calculator);

private:
    Config config_;
};

}  // namespace es

#endif  // ES_SOLVER_HPP
