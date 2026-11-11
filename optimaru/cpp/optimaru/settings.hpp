// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SETTINGS_HPP
#define OPTIMARU_SETTINGS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "optimaru/score/score.hpp"
// Only the plugin point, never the schemes umbrella: select/schemes.hpp
// includes this header back, so including it here would be a cycle. Which
// side of it wins depends on include order, so it built for some translation
// units and left SelectionScheme undefined for others.
#include "optimaru/select/scheme.hpp"

namespace optimaru {

// Plain structs, set field by field; every field has a working default.
//
//   optimaru::Settings settings;
//   settings.limits.time_limit = std::chrono::seconds(10);
//   settings.search.acceptance.rule = optimaru::AcceptanceRule::LateAcceptance;

// When the solve stops: as soon as ANY configured limit is hit.
struct Limits {
    // Total wall time (always active).
    std::chrono::milliseconds time_limit{std::chrono::seconds(30)};
    // Wall time since the last new best (0 = disabled).
    std::chrono::milliseconds stall_time_limit{0};
    // Local-search steps (0 = disabled).
    std::int64_t step_limit = 0;
    // Steps since the last new best (0 = disabled).
    std::int64_t stall_step_limit = 0;
    // Stop once the best score reaches this (default never fires).
    Score target_score = Score::unreachable();
};

// The construction step that assigns everything left unassigned before local
// search starts. With `difficulty` set, entities/elements are placed in
// DECREASING difficulty order (first fit decreasing); otherwise in index order
// (first fit).
struct ConstructionSettings {
    std::function<double(int entity_index)> difficulty;
};

// The built-in move mix: each step draws one move kind by relative weight.
// Weights need not sum to 1; a weight of 0 disables the move. Moves that
// don't apply to the current solution shape (e.g. tail swap with a single
// list) are skipped automatically.
struct MoveMix {
    // Assignment shape
    double change_weight = 0.75;  // reassign one entity
    double swap_weight = 0.25;    // swap two entities' values

    // Sequence shape
    double relocate_weight = 0.5;   // move one element
    double list_swap_weight = 0.3;  // swap two elements
    double two_opt_weight = 0.2;    // reverse a segment within one list
    // Or-opt: relocate a short segment (2-3 consecutive elements) intact.
    double or_opt_weight = 0.0;
    // 2-opt* (tail swap): exchange the tails of two lists at random cut
    // points — the classic inter-route VRP move. Needs >= 2 lists.
    double tail_swap_weight = 0.0;
    // CROSS-exchange: swap a run of elements in one list with a run in
    // another (the runs may differ in length, and one may be empty, which
    // makes it a segment relocate). Each draw takes 0..cross_exchange_max_run
    // elements from each side (at least one in total), so the default covers
    // every length pair up to 3x3. It is the move that rebalances load between
    // routes.
    //
    // Why the other inter-list moves do not cover it: the tail swap can only
    // cut once per list, so it cannot move work out of the MIDDLE of a route,
    // and relocate/or-opt move elements one way only, so shifting work from a
    // full route to a full one has to pass through an overloaded intermediate
    // state that a hard capacity constraint rejects. Exchanging both ways at
    // once keeps every intermediate feasible.
    double cross_exchange_weight = 0.0;
    // Longest run CROSS-exchange may take from either side.
    int cross_exchange_max_run = 3;
};

// Nearby selection. When `distance` is set (the domain distance between two
// entities (assignment shape) or elements (sequence shape)), the engine
// precomputes each one's `size` nearest neighbours and biases swap partners /
// relocate targets toward them with probability `probability`. The same lists
// drive the related ruin and the nearby-restricted repair.
struct NeighborSettings {
    std::function<double(int, int)> distance;
    int size = 20;
    double probability = 0.8;
};

// The acceptance rule local search judges each candidate move by.
enum class AcceptanceRule {
    HillClimbing,
    LateAcceptance,
    EntityTabu,
    SimulatedAnnealing,
    GreatDeluge,
    StepCountingHillClimbing,
    RecordToRecordTravel,
};

// The acceptance rule and its parameters.
struct AcceptanceSettings {
    AcceptanceRule rule = AcceptanceRule::LateAcceptance;

    std::size_t history_length = 400;  // LateAcceptance
    // LateAcceptance only: search in stages of growing history. A longer
    // history explores more widely and converges later, so the right length
    // depends on the time there is, which one fixed number cannot follow: a
    // short one converges in a fraction of a second on a small instance and
    // leaves the rest of the budget to the kicks alone. Above 1, this is the
    // factor from one stage to the next: once the search has converged and no
    // new best has come for half as long as the next stage would take (at
    // once, where ruin & recreate is off), the acceptor starts over from the
    // start's score with the history this much longer, provided that stage
    // still fits in the budget (a stage is taken to last the factor times the
    // one before). The search goes on from the current solution and the best
    // one is kept as always, so a stage that finds nothing costs its time and
    // the kicks that would have fired in it. The history stops growing at
    // 2^20 entries (16 MB per start); from there a stage restarts with that
    // length. 1 or less = off.
    //
    // Set it for a solve that starts from a construction, and for no other
    // (a use case asks nothing_placed(), construct/construction.hpp). A stage
    // starts over from the start's score, which loosens the search only where
    // it began well below where it stands. On a warm start from a solved
    // solution the start's score is where the search stands: a stage loosens
    // nothing there, and it is taken at a stall in the place of the kick that
    // is due, so a search whose improvements are the kicks' would return its
    // start. The engine guards the plainest case itself: a search that stalls
    // before it has found anything better than its start has no stages from
    // there on, and goes on as it does without them.
    //
    // A stage does not know the stall limits: it walks below the best for a
    // while, so a stall_time_limit or stall_step_limit shorter than a stage
    // ends the solve inside it, with the best solution so far.
    //
    // Stages also change when a ruin & recreate kick fires: a kick's acceptor
    // has the history of the stage the search is in, stagnation counts from
    // the last improving move (a stage starts far below the best), and a kick
    // waits at least two histories, so unimproved_step_trigger has less
    // effect the longer the history gets.
    double history_growth = 0.0;
    int tabu_tenure = 7;  // EntityTabu
    // SimulatedAnnealing: the starting temperature, cooling to 0 over the
    // solve. (0, 0) with a start_temperature_fraction above 0 is automatic:
    // hard 0 (a hard-worse move is never taken) and soft = the fraction of
    // the magnitude of the start's soft level, at least 1 (the soft level
    // alone: a medium level the domain folds into the engine's soft does not
    // count; see unfolded_soft in score/levels.hpp for what that reading
    // is exact for). (0, 0) with a fraction of 0 accepts no worsening move
    // at all.
    Score start_temperature{0, 0.0};
    double start_temperature_fraction = 0.0;
    // GreatDeluge: how much the water level rises per step.
    Score water_level_step{0, 0.0};
    std::int64_t recount_interval = 400;  // StepCountingHillClimbing

    // RecordToRecordTravel: the acceptable soft-score gap below the best
    // score, shrinking linearly from start to end over the solve. Both are
    // fractions of the initial score's soft magnitude (the autofit form), so
    // they need no per-instance tuning. The schedule spans the step limit,
    // or 10^6 steps when there is none.
    double rrt_start_gap = 0.02;
    double rrt_end_gap = 0.0;
};

// The adaptive ruin-and-recreate (ALNS) settings, with adaptive operator
// selection. After
// unimproved_step_trigger steps without a new best, one kick runs: an
// adaptively selected ruin operator (random / related / worst, SISR and route
// ruins when enabled below, registered customs) removes a fraction of the
// solution, the repair (regret-k by default, see regret_k) rebuilds it,
// polish_steps hill-climbing moves repair the seams, and the result becomes
// the new incumbent unconditionally with a fresh acceptor
// (perturbation-style; the best-solution snapshot is the safety net).
struct RuinRecreateSettings {
    bool enabled = false;
    std::int64_t unimproved_step_trigger = 5000;
    double min_ruin_fraction = 0.10;
    double max_ruin_fraction = 0.30;
    // Absolute cap on entities ruined per kick (Ropke & Pisinger's q <= ~60):
    // on large instances an uncapped fractional ruin (10-30% of thousands)
    // makes every kick expensive and unrecoverable within polish_steps, so the
    // unconditional-incumbent kick destroys more progress than it enables.
    int max_ruin_count = 60;
    // Operator adaptation. RouletteWheel is w = decay*w + (1-decay)*reward,
    // rewards indexed by MoveOutcome (NewBest / Better / Accepted / Rejected);
    // see select/ for the other three schemes.
    SelectionScheme selection_type = SelectionScheme::RouletteWheel;
    double operator_decay = 0.8;
    std::vector<double> rewards{33.0, 9.0, 3.0, 0.0};
    // SegmentedRouletteWheel: iterations per weight-update segment.
    std::int64_t selection_segment_length = 100;
    // AlphaUcb: exploration weight; <= 0.1 is the usual choice.
    double selection_alpha = 0.05;

    // Which ruin operators join the adaptive pool, on top of the always-on
    // random / related / worst trio. Both are sequence-shape strategies and
    // fall back to the random ruin on assignment solutions (see their headers).
    bool sisr_ruin = true;   // substring removal (Christiaens & Vanden Berghe)
    bool route_ruin = true;  // empty whole lists
    int sisr_max_string_removals = 3;
    int sisr_max_string_size = 12;

    // Recreate settings. (The list the repair never places into, a virtual
    // unassigned pool, is SearchSettings::unassigned_list.)
    // regret_k >= 2 switches the repair from plain cheapest-insertion to
    // regret-k: instead of taking elements in random order, repeatedly insert
    // the element with the largest gap between its best and its k-th best
    // placement — the one that will suffer most if it waits. Costs one extra
    // pass over the pending elements per insertion, and on time-windowed
    // instances routinely buys more than it costs. 0/1 = plain greedy.
    int regret_k = 3;
    // SISR's "blinking": skip evaluating a candidate placement with this
    // probability, so the repair is not deterministically greedy and repeated
    // kicks on the same hole explore different rebuilds. 0 disables it.
    double blink_rate = 0.01;

    // Hill-climbing small moves polishing each kick before it becomes the
    // incumbent. NOT counted against step_limit.
    std::int64_t polish_steps = 5000;

    // Destroy-and-repair as an ORDINARY move, in addition to the stagnation
    // kick above: a small ruin is applied, repaired, scored, and UNDONE if the
    // acceptor rejects it. Implemented as a move operator plugin
    // (ruin_recreate/ruin_recreate_move.hpp) drawn with this probability per step.
    // The candidate is judged by the normal acceptance criterion. The kick
    // above is the opposite — rare and unconditional.
    // 0 disables it, which is the default.
    double as_move_weight = 0.0;
    // Ruin size for the as-a-move form. Much smaller than a kick's: it has to
    // be cheap enough to run every few steps and small enough that the
    // acceptor sees a local change rather than a restart.
    int as_move_max_ruin = 12;
};

// Local search: the acceptance rule, the move mix, the guided/nearby
// selection hooks, ruin-and-recreate and the built-in strategies.
struct SearchSettings {
    AcceptanceSettings acceptance;
    MoveMix moves;

    // Optional guided move selection, assignment shape only. Either hook may return
    // -1 to fall back to uniform-random selection for that draw.
    std::function<int(std::mt19937_64&)> entity_hint;
    std::function<int(std::mt19937_64&, int entity_index)> value_hint;

    NeighborSettings nearby;
    RuinRecreateSettings ruin_recreate;

    // Sequence shape: the list holding unplaced elements, a virtual unassigned
    // pool, or -1 for none. One field, read by every built-in that must know
    // the pool:
    //  - the ruin-and-recreate repair, and the iterated local search's,
    //    never place into it. Without that, a ruin on a tightly loaded
    //    instance ends with the elements that fit nowhere parked in the pool,
    //    a lexicographic loss the kick then makes the incumbent and one the
    //    ordinary moves never undo (nothing but a chain re-assigns). With it
    //    the leftover lands on the least overloaded list instead, a hard
    //    breach the next few relocations and swaps mend. Measured on CVRPLIB
    //    X-n289-k60 (99.7% loaded): with the pool open the search stalls at
    //    its first kick;
    //  - the ejection chain seeds half its chains from it while it is
    //    non-empty, and never moves anything into it;
    //  - the descents (Descent, GranularLocalSearch) neither reorder it nor
    //    move elements in or out of it: its order carries no score, and
    //    assigning or unassigning is the repair's and the chain's job, where
    //    hard feasibility is judged as a whole.
    int unassigned_list = -1;

    // Upper gate for caller-registered MoveOperators. The adaptive selector
    // still chooses between the built-in mix and custom operators inside the
    // gate; outside it, the ordinary move mix runs directly. This prevents an
    // expensive macro move with one recent reward from monopolizing the clock.
    // 1 preserves the original unrestricted behavior.
    double custom_move_probability = 1.0;

    // ---------------------------------------------------------------------
    // Built-in search strategies
    // ---------------------------------------------------------------------
    // Neighbourhoods too expensive to fire every step, and too useful to leave
    // out. Each is a MoveOperator in moves/ or ruin_recreate/, one file
    // per strategy, implementing the same interface a caller gets from
    // Solver::add_move_operator(); these weights are the probability that a step
    // draws that plugin instead of the ordinary move mix.
    //
    // WHY A WEIGHT AND NOT ADAPTIVE SELECTION. The obvious next step is to drop
    // these numbers into the adaptive pool that already weights the ruin
    // operators and let it learn them. That was tried and measured, and it
    // loses badly (RC2_10_5, 1000 customers, 120s, vs best known: +8.1% at
    // these weights, +12.9% adapting on reward-per-evaluation, +16.6% also
    // dividing the draw probability by measured cost). The reason is
    // structural, not a tuning miss: RouletteWheel adapts an EMA of the reward
    // with operator_decay, whose default 0.8 remembers about five applications.
    // That is the right memory for the ruin pool — a handful of similar-cost
    // operators, each applied at a kick and each succeeding often enough to
    // register. The move pool is the opposite: the ordinary move mix improves
    // roughly once in 10^5 draws late in a large solve, so its EMA sits at zero
    // essentially always, and ANY operator with a non-zero recent reward
    // outbids it — however much of the clock it burns. Weighting the move pool
    // adaptively needs a scheme that can represent a rare-success operator's
    // long-run rate, which is a change to the selection schemes rather than to
    // the strategies here.
    //
    // descent: best-improvement over one nearby pair, repeated until that pair
    // is locally optimal. The neighbourhood is per shape — sequence: two
    // geometrically nearby routes, every relocation between them, both
    // directions; assignment: a nearby entity pair, every value of each entity plus
    // the swap of the two. See moves/descent.hpp.
    //
    // What the right weight is depends entirely on how big that neighbourhood
    // is, and for assignment that is the size of the VALUE RANGE — one sweep costs
    // ~2 * value_count score evaluations against an ordinary step's one. With 40
    // employees (employee-scheduling) a percent-scale weight spends a sensible
    // slice of the budget and pays for itself; with a 40x25 slot x room grid
    // (school-timetabling) the same weight spends over 90% of the budget on
    // descent and starves the move mix. Weight by the value range, and measure.
    double descent_weight = 0.0;
    // Give up on a neighbourhood after this many improving passes, so one
    // application cannot run away on a large pair.
    int descent_max_sweeps = 4;

    // Sequence-only intensification. When enabled, descent evaluates the
    // granular neighbourhood around one nearby route pair instead of only
    // relocate moves: Exchange(1,0), Exchange(1,1), 2-opt, 2-opt* and small
    // CROSS-exchange moves. This is intentionally opt-in because a full
    // route-pair neighbourhood is much more expensive than the assignment
    // entity/value descent used by rostering and timetabling.
    bool descent_list_advanced = false;
    int descent_list_max_segment = 2;
    int descent_list_candidate_routes = 4;

    // route_elimination: empty the smallest list into the others in one atomic
    // move, so the acceptor sees the net effect of losing its depot legs
    // rather than the uphill intermediates that relocating one element at a
    // time creates. See moves/route_elimination.hpp.
    double route_elimination_weight = 0.0;
    // Do not attempt lists longer than this — beyond a certain size the
    // reinsertion is both expensive and hopeless.
    int route_elimination_max_size = 12;

    // ejection_chain: break a hard constraint on purpose (insert an element of
    // the source list, or take the best-soft relocation that is infeasible),
    // then apply strictly hard-improving relocations and swaps until the hard
    // score is back, and let the acceptor judge the whole chain. This reaches
    // the slack shifts a tightly packed instance needs, every one of which is
    // uphill alone. See moves/ejection_chain.hpp.
    double ejection_chain_weight = 0.0;
    // Give up (and undo) after this many repair operations.
    int ejection_chain_max_steps = 24;
    // Lists searched per element during the repair: those of its nearest
    // neighbours, in nearby order.
    int ejection_chain_candidate_lists = 4;
    // Score evaluations one chain may spend, seed and repair together; a
    // chain that runs out is undone like one that stalls. This, not the step
    // cap, is what bounds the cost of a chain on long lists (candidate
    // collection between evaluations is not counted, but it is bounded by
    // the nearby size and ejection_chain_candidate_lists).
    std::int64_t ejection_chain_max_evaluations = 50000;

    // iterated_local_search (sequence only): one ILS iteration as a move — a small
    // ruin from the adaptive ruin pool, the greedy repair, then a granular
    // first-improvement descent (relocate, swap, 2-opt, 2-opt*, Or-opt, CROSS
    // around every element's nearby neighbours) until the touched lists are
    // locally optimal. The acceptor judges the local optimum. The first
    // iteration only descends, so the construction result is a local optimum
    // before anything is perturbed. Needs nearby selection and a calculator
    // that serves previews; it declines every draw otherwise. See
    // moves/iterated_local_search.hpp.
    double iterated_local_search_weight = 0.0;
    // Elements ruined per iteration, uniform in [min, max].
    int iterated_local_search_min_ruin = 5;
    int iterated_local_search_max_ruin = 15;
    // Longest Or-opt segment the descent relocates (1 disables Or-opt).
    int iterated_local_search_max_segment = 3;
    // Evaluations one descent may spend before it stops where it is. At O(1)
    // previews five million evaluations is on the order of a quarter second;
    // where a preview costs O(route) (a decomposed assembly, a plan with
    // waiting) the descent instead consults the solver's stop condition every
    // thousand evaluations and stops at the time limit, so this cap is the
    // only bound just for a solve without a wall-clock limit. Lower caps
    // truncate the descents
    // that matter: with one million, CVRPLIB X-n895-k37 (37 routes of ~24
    // stops) goes from 4.3 / 5.7% to 6.1 / 6.0% gap at 60 seconds.
    std::int64_t iterated_local_search_max_evaluations = 5000000;
};

// Everything one solve is configured with.
struct Settings {
    std::uint64_t seed = 0;

    // ScoreChecks::Full re-verifies the incremental score against a full
    // recompute after every move (debugging aid; orders of magnitude slower).
    ScoreChecks score_checks = ScoreChecks::Off;

    // Multi-start: solve_multistart() runs this many independent searches
    // (seeds seed, seed+1, ...) on threads of their own and keeps the best.
    int parallel_starts = 1;

    // Called with (step, score) on every new best, from the solving thread.
    std::function<void(std::int64_t, const Score&)> on_new_best;

    Limits limits;
    ConstructionSettings construction;
    SearchSettings search;
};

}  // namespace optimaru

#endif  // OPTIMARU_SETTINGS_HPP
