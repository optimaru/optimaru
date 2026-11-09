// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_LOCAL_SEARCH_HPP
#define OPTIMARU_SEARCH_LOCAL_SEARCH_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/select/schemes.hpp"
#include "optimaru/destroy/random.hpp"
#include "optimaru/destroy/related.hpp"
#include "optimaru/destroy/route.hpp"
#include "optimaru/destroy/sisr.hpp"
#include "optimaru/destroy/worst.hpp"
#include "optimaru/accept/acceptors.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/repair/greedy_repair.hpp"
#include "optimaru/local/list_move.hpp"
#include "optimaru/local/movable_entity.hpp"
#include "optimaru/model/snapshot.hpp"
#include "optimaru/local/descent.hpp"
#include "optimaru/local/ejection_chain.hpp"
#include "optimaru/local/preview_evaluation.hpp"
#include "optimaru/search/iterated_local_search.hpp"
#include "optimaru/search/route_elimination.hpp"
#include "optimaru/search/ruin_recreate_move.hpp"
#include "optimaru/stop/stop_condition.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// LocalSearch
// ---------------------------------------------------------------------------
// One sampled candidate move per step, judged by the configured Acceptor,
// until the StopCondition fires; interleaved with adaptive ruin-and-recreate
// kicks when the search stagnates (if enabled).
//   assignment shape: change (give one entity another value) and swap (two
//                     entities exchange values);
//   sequence shape:   relocate (move one element), element swap, 2-opt
//                     (reverse a segment within one list), Or-opt (move a
//                     2-3 element segment), 2-opt* (tail swap) and
//                     CROSS-exchange; mixed by MoveMix weights (the last three
//                     default to 0).
// Moves follow a do/undo protocol: apply through the
// Editor, read score(), and revert when rejected.
//
// Anything more elaborate than the random move mix above is a plugin, not a
// branch in this file: the built-in strategies in local/ and search/ implement the
// same MoveOperator interface a caller gets from Solver::add_move_operator(),
// and compete for selection in one adaptive pool with them.
template <typename Solution>
class LocalSearch {
    using Traits = SolutionTraits<Solution>;
    using Snapshot = SolutionSnapshot<Solution>;

public:
    LocalSearch(
            const Settings& config, const Neighborhoods& nearby,
            const std::vector<std::unique_ptr<MoveOperator<Solution>>>& move_operators,
            const std::vector<std::unique_ptr<RuinOperator<Solution>>>& ruin_operators,
            const std::function<std::unique_ptr<Acceptor>(const Score&)>& acceptor_factory)
        : config_(config),
          nearby_(nearby),
          move_operators_(move_operators),
          ruin_operators_(ruin_operators),
          acceptor_factory_(acceptor_factory),
          repair_(index_, nearby, config.search.ruin_recreate,
                  config.search.unassigned_list) {}

    Score run(Editor<Solution>& editor, StopCondition& stop) {
        Solution& solution = editor.solution();
        if constexpr (Traits::shape == Shape::Assignment) {
            if (Traits::entity_count(solution) == 0 || Traits::value_count(solution) == 0) {
                return editor.score();
            }
            if (all_entities_pinned(solution)) {
                const Score score = editor.score();
                if (config_.on_new_best) config_.on_new_best(0, score);
                return score;
            }
        } else {
            if (Traits::list_count(solution) == 0 || count_list_elements(solution) == 0) {
                return editor.score();
            }
            index_.build(solution);
        }

        const RuinRecreateSettings& lns = config_.search.ruin_recreate;
        const SearchSettings& ls = config_.search;
        Score current_score = editor.score();
        Score best_score = current_score;
        auto best = Snapshot::take(solution);
        if (config_.on_new_best) config_.on_new_best(0, best_score);

        std::unique_ptr<Acceptor> acceptor = make_acceptor(current_score);
        // Custom acceptors may consume the time gradient. Of the built-ins,
        // only simulated annealing does; avoid a duration conversion and
        // floating-point division on every move for the default LA path.
        const bool needs_time_gradient =
                static_cast<bool>(acceptor_factory_) ||
                config_.search.acceptance.rule ==
                        AcceptanceRule::SimulatedAnnealing;
        // The built-in ruins are plugins like any other: one file each in
        // destroy/, registered ahead of the caller's operators so the
        // adaptive selector sees a single uniform list.
        RandomRuin<Solution> random_ruin;
        RelatedRuin<Solution> related_ruin;
        WorstRuin<Solution> worst_ruin;
        [[maybe_unused]] SisrRuin<Solution> sisr_ruin(lns.sisr_max_string_removals,
                                                    lns.sisr_max_string_size);
        [[maybe_unused]] RouteRuin<Solution> route_ruin;
        std::vector<RuinOperator<Solution>*> ruins{&random_ruin, &related_ruin, &worst_ruin};
        // sequence shape only — and not merely as an optimization. Both delegate to
        // the random ruin on assignment solutions (a "substring" and a "route" mean
        // nothing without an ordered list), so registering them there would put
        // three copies of the random ruin in a five-operator pool and silently
        // triple its selection weight for every assignment use case.
        if constexpr (Traits::shape == Shape::Sequence) {
            if (lns.sisr_ruin) ruins.push_back(&sisr_ruin);
            if (lns.route_ruin) ruins.push_back(&route_ruin);
        }
        for (const auto& op : ruin_operators_) ruins.push_back(op.get());
        auto ruin_selector = make_operator_selection_scheme(
                lns.selection_type, static_cast<int>(ruins.size()), lns.rewards, lns.operator_decay,
                lns.selection_segment_length, lns.selection_alpha);

        // Adaptive selection over {built-in move mix, custom operators...};
        // only consulted when custom move operators are registered.
        auto move_selector = make_operator_selection_scheme(
                lns.selection_type, 1 + static_cast<int>(move_operators_.size()), lns.rewards,
                lns.operator_decay, lns.selection_segment_length, lns.selection_alpha);
        OperatorContext<Solution> operator_context{editor, nearby_, index_.lists(),
                                                  index_.positions()};

        // The built-in strategies: plugins like any other MoveOperator, drawn
        // by weight before the ordinary move mix gets its turn. Their weights
        // are small by necessity — each costs orders of magnitude more than one
        // sampled move — and the header of SearchSettings records why that
        // is a configured number here rather than something the adaptive
        // selector above is asked to discover.
        [[maybe_unused]] RouteElimination<Solution> route_elimination(index_, repair_,
                                                                     ls.route_elimination_max_size);
        Descent<Solution> descent(index_, nearby_, ls.descent_max_sweeps, ls.entity_hint,
                                  ls.descent_list_advanced, ls.descent_list_max_segment,
                                  ls.descent_list_candidate_routes, ls.unassigned_list);
        RuinRecreateMove<Solution> ruin_recreate_move(index_, repair_, ruins, *ruin_selector,
                                                    lns.as_move_max_ruin);
        [[maybe_unused]] EjectionChain<Solution> ejection_chain(
                index_, nearby_, ls.unassigned_list, ls.ejection_chain_max_steps,
                ls.ejection_chain_candidate_lists, ls.ejection_chain_max_evaluations);
        [[maybe_unused]] GranularLocalSearch<Solution> granular_local_search(
                index_, nearby_, ls.iterated_local_search_max_segment);
        // Its own adaptive ruin weights: an ILS outcome is measured after the
        // descent and would otherwise retrain the destroy-repair move's.
        auto ils_ruin_selector = make_operator_selection_scheme(
                lns.selection_type, static_cast<int>(ruins.size()), lns.rewards, lns.operator_decay,
                lns.selection_segment_length, lns.selection_alpha);
        [[maybe_unused]] IteratedLocalSearchMove<Solution> iterated_local_search(
                index_, repair_, granular_local_search, ruins, *ils_ruin_selector,
                ls.iterated_local_search_min_ruin, ls.iterated_local_search_max_ruin,
                ls.unassigned_list, ls.iterated_local_search_max_evaluations);
        std::vector<WeightedStrategy> strategies;
        // Route elimination is sequence-only, for the same reason the SISR and
        // route ruins are: it declines on assignment solutions, so registering it
        // there would only burn steps on an operator that can never move.
        // Descent has a neighbourhood on both kinds and is registered on both.
        if constexpr (Traits::shape == Shape::Sequence) {
            if (ls.route_elimination_weight > 0.0) {
                strategies.push_back({ls.route_elimination_weight, &route_elimination});
            }
            if (ls.ejection_chain_weight > 0.0) {
                strategies.push_back({ls.ejection_chain_weight, &ejection_chain});
            }
            if (ls.iterated_local_search_weight > 0.0) {
                strategies.push_back({ls.iterated_local_search_weight, &iterated_local_search});
            }
        }
        if (ls.descent_weight > 0.0) {
            strategies.push_back({ls.descent_weight, &descent});
        }
        if (lns.as_move_weight > 0.0) {
            strategies.push_back({lns.as_move_weight, &ruin_recreate_move});
        }
        std::mt19937_64 rng(config_.seed);
        // Reused by caller-provided operators. Keeping it out of AppliedMove
        // avoids constructing and destroying a std::vector on every built-in
        // move, which is the hot path for all use cases.
        std::vector<int> custom_entities;
        std::int64_t step = 0;
        std::int64_t steps_since_best_or_lns = 0;
        // The strategies that evaluate thousands of candidates inside one
        // move (the iterated local search's descent, the ejection chain's
        // repair) consult the stop condition between batches of evaluations, so
        // one move cannot run far past a time limit the step loop only reads
        // between moves. Only the clock-reading criteria matter here; the
        // step-based ones see the current step.
        const std::function<bool()> stop_requested = [&] {
            return stop.should_stop(step, best_score, /*force_time_check=*/true);
        };
        operator_context.stop_requested = &stop_requested;
        // Kick cadence. "Stagnated" must be judged relative to how often this
        // instance normally improves: on a large instance plain LA improves
        // every ~10^5-10^6 steps, so a fixed step trigger would kick mid-
        // progress and turn the search into a perpetual restart. Two guards:
        //  - dynamic base: 8x the EMA of recent steps-between-new-bests,
        //    never below the configured trigger;
        //  - exponential backoff: every kick that fails to reach a new best
        //    before the next trigger doubles the interval (capped so kicks
        //    never stop entirely); any new best resets it.
        std::int64_t lns_trigger = lns.unimproved_step_trigger;
        const std::int64_t lns_trigger_cap = lns.unimproved_step_trigger << 12;
        std::int64_t last_best_step = 0;
        std::int64_t ema_best_gap = 0;
        bool force_time_check = false;

        while (!stop.should_stop(step, best_score, force_time_check)) {
            force_time_check = false;
            if (lns.enabled && steps_since_best_or_lns >= std::max(lns_trigger, 8 * ema_best_gap)) {
                current_score = lns_iteration(editor, current_score, best_score, *ruin_selector,
                                            ruins, operator_context, rng);
                force_time_check = true;
                // A perturbation kick becomes the new incumbent uncondition-
                // ally and the acceptor restarts fresh, so
                // the following small moves re-polish the kicked solution
                // instead of instantly rejecting it (best is snapshotted, so
                // a bad kick can never worsen the returned solution).
                acceptor = make_acceptor(current_score);
                steps_since_best_or_lns = 0;
                lns_trigger = std::min(lns_trigger * 2, lns_trigger_cap);
            } else {
                // A built-in strategy plugin if one is drawn, otherwise the
                // ordinary move mix / a caller's operator under the adaptive
                // selector. `selector_index` < 0 means nothing to report back.
                MoveOperator<Solution>* op = draw_strategy(strategies, rng);
                int selector_index = -1;
                bool custom = false;
                if (op == nullptr) {
                    int move_op = 0;
                    bool selector_consulted = false;
                    if (!move_operators_.empty()) {
                        const double gate = ls.custom_move_probability;
                        if (gate >= 1.0 || (gate > 0.0 && unit_(rng) < gate)) {
                            move_op = move_selector->select(rng);
                            selector_consulted = true;
                        }
                    }
                    if (move_op != 0) {
                        op = move_operators_[static_cast<std::size_t>(move_op) - 1].get();
                        custom = true;
                    }
                    if (selector_consulted) selector_index = move_op;
                }
                AppliedMove move;
                bool applied = true;
                // A previewed move is scored from the objective's cached
                // route data without touching the solution, and applied only
                // if the acceptor takes it. Most moves are rejected, so this
                // is where the apply/score/undo cost of a step disappears.
                // `previewed`: the move is not applied and `sampled_score` is
                // its preview; `scored`: `sampled_score` is its score either
                // way (see score_list_move).
                bool previewed = false;
                bool scored = false;
                Score sampled_score;
                editor.begin_trial();
                if (op == nullptr) {
                    if constexpr (Traits::shape == Shape::Assignment) {
                        move = apply_basic_move(editor, rng);
                    } else {
                        move = sample_list_move(editor, rng);
                        previewed = score_list_move(editor, move, sampled_score);
                        scored = true;
                    }
                } else {
                    if (custom) editor.track_touched_lists();
                    custom_entities.clear();
                    applied = op->apply(operator_context, rng, custom_entities);
                }
                if (applied) {
                    Score candidate_score = scored ? sampled_score
                                                          : editor.score();
                    const int* move_entities = op == nullptr ? move.entities
                                                            : custom_entities.data();
                    const int move_entity_count =
                            op == nullptr ? move.entity_count
                                          : static_cast<int>(custom_entities.size());
                    bool accepted = acceptor->accepts(candidate_score, current_score, best_score,
                                                         move_entities, move_entity_count,
                                                         needs_time_gradient
                                                                 ? stop.progress()
                                                                 : 0.0,
                                                         rng);
                    const MoveOutcome outcome =
                            candidate_score > best_score      ? MoveOutcome::NewBest
                            : candidate_score > current_score ? MoveOutcome::Better
                            : accepted                      ? MoveOutcome::Accepted
                                                            : MoveOutcome::Rejected;
                    if (accepted) {
                        current_score = candidate_score;
                        acceptor->on_accepted(move_entities, move_entity_count);
                        if (previewed) {
                            if constexpr (Traits::shape == Shape::Sequence) {
                                apply_list_move(editor, move);
                            }
                        }
                    } else if (!previewed) {  // a previewed move was never applied
                        editor.begin_rollback();
                        if (op == nullptr) {
                            undo_move(editor, move);
                        } else {
                            op->undo_last(operator_context);
                        }
                    }
                    editor.end_trial(accepted);
                    if (selector_index >= 0) move_selector->update(selector_index, outcome);
                    if (op != nullptr) op->outcome(outcome);
                } else {
                    editor.end_trial(false);
                }
                if (op != nullptr && custom) {
                    // Resynchronize the element index with whatever lists the
                    // custom operator touched (apply and/or undo).
                    if constexpr (Traits::shape == Shape::Sequence) {
                        for (int l : editor.touched_lists()) index_.reindex(solution, l);
                    }
                    editor.untrack_touched_lists();
                }
                // Built-in strategies and caller-provided operators may scan
                // a neighbourhood or run a repair. Do not defer the clock
                // sample after those potentially expensive steps.
                if (op != nullptr) force_time_check = true;
            }
            acceptor->on_step_end(current_score);

            if (current_score > best_score) {
                best_score = current_score;
                best = Snapshot::take(solution);
                stop.on_improvement(step);
                steps_since_best_or_lns = 0;
                lns_trigger = lns.unimproved_step_trigger;
                const std::int64_t gap = step - last_best_step;
                ema_best_gap = ema_best_gap == 0 ? gap : (3 * ema_best_gap + gap) / 4;
                last_best_step = step;
                if (config_.on_new_best) config_.on_new_best(step, best_score);
            } else {
                ++steps_since_best_or_lns;
            }
            ++step;
        }

        Snapshot::restore(editor, best);
        return best_score;
    }

private:
    // A built-in strategy plugin and the probability a step draws it.
    struct WeightedStrategy {
        double weight;
        MoveOperator<Solution>* op;
    };

    // Offer each strategy its own draw, in vector order, and take the first
    // that hits; null means this step falls through to the ordinary move mix.
    //
    // The draws are sequential, so a weight is the probability of firing GIVEN
    // that everything before it missed, not a share of all steps: at the
    // vehicle-routing defaults route elimination runs on 0.10 of steps, the
    // ejection chain on 0.9 x 0.05, the iterated local search on
    // 0.9 x 0.95 x 0.05, descent on 0.9 x 0.95 x 0.95 x 0.03 and
    // destroy-repair on 0.9 x 0.95 x 0.95 x 0.97 x 0.05. The weights are small
    // enough that the difference barely shows, and they were measured through
    // this same code so the numbers are self-consistent — but the ORDER below
    // is a priority, and raising a weight dilutes everything after it.
    static MoveOperator<Solution>* draw_strategy(const std::vector<WeightedStrategy>& strategies,
                                                std::mt19937_64& rng) {
        if (strategies.empty()) return nullptr;
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        for (const WeightedStrategy& s : strategies) {
            if (unit(rng) < s.weight) return s.op;
        }
        return nullptr;
    }

    const Settings& config_;
    const Neighborhoods& nearby_;
    const std::vector<std::unique_ptr<MoveOperator<Solution>>>& move_operators_;
    const std::vector<std::unique_ptr<RuinOperator<Solution>>>& ruin_operators_;
    const std::function<std::unique_ptr<Acceptor>(const Score&)>& acceptor_factory_;
    // sequence shape: element id -> (list, position), kept in sync with every move
    // so nearby targets and LNS ruin work in O(1) per lookup. Declared before
    // repair_, which holds a reference to it.
    ElementIndex<Solution> index_;
    // Ruin & repair, shared by the stagnation kick here and the strategies
    // in search/ that do the same thing as an ordinary move.
    GreedyRepair<Solution> repair_;
    std::uniform_real_distribution<double> unit_{0.0, 1.0};

    std::unique_ptr<Acceptor> make_acceptor(const Score& initial_score) const {
        if (acceptor_factory_) return acceptor_factory_(initial_score);
        const AcceptanceSettings& c = config_.search.acceptance;
        switch (c.rule) {
            case AcceptanceRule::HillClimbing:
                return std::make_unique<HillClimbing>();
            case AcceptanceRule::EntityTabu:
                return std::make_unique<EntityTabu>(c.tabu_tenure);
            case AcceptanceRule::SimulatedAnnealing:
                return std::make_unique<SimulatedAnnealing>(
                        c.start_temperature);
            case AcceptanceRule::GreatDeluge:
                return std::make_unique<GreatDeluge>(initial_score,
                                                             c.water_level_step);
            case AcceptanceRule::StepCountingHillClimbing:
                return std::make_unique<StepCountingHillClimbing>(
                        c.recount_interval, initial_score);
            case AcceptanceRule::RecordToRecordTravel: {
                // The threshold schedule spans the whole solve, so it needs a
                // step count. Prefer the configured limit; otherwise assume a
                // long run (10^6 steps) — overshooting only makes the
                // threshold decay more slowly, which RRT tolerates, whereas
                // undershooting collapses it to hill climbing immediately.
                const std::int64_t steps = config_.limits.step_limit > 0
                                                   ? config_.limits.step_limit
                                                   : 1000000;
                return std::make_unique<RecordToRecordTravel>(
                        RecordToRecordTravel::autofit(
                                initial_score.soft, c.rrt_start_gap,
                                c.rrt_end_gap,
                                c.rrt_steps > 0 ? c.rrt_steps
                                                                : steps,
                                c.rrt_exponential
                                        ? RecordToRecordTravel::Method::Exponential
                                        : RecordToRecordTravel::Method::Linear,
                                c.rrt_compare_to_best));
            }
            case AcceptanceRule::LateAcceptance:
            default:
                return std::make_unique<LateAcceptance>(c.history_length,
                                                                initial_score);
        }
    }

    bool use_nearby(std::mt19937_64& rng) {
        if (!nearby_.enabled()) return false;
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        return unit(rng) < config_.search.nearby.probability;
    }

    int random_of(const std::vector<int>& v, std::mt19937_64& rng) {
        return v[rng() % v.size()];
    }

    // Uniformly draw from [0, n) except one value, without rejection. This is
    // used by moves where drawing the current entity/position is a guaranteed
    // no-op. Besides wasting a score evaluation, accepted no-ops advance Late
    // Acceptance history and dilute the useful neighbourhood.
    static int random_other(int n, int excluded, std::mt19937_64& rng) {
        int picked = static_cast<int>(rng() % static_cast<std::uint64_t>(n - 1));
        if (picked >= excluded) ++picked;
        return picked;
    }

    // --- moves ---

    // One applied move, with what is needed to reject it and to feed the
    // acceptor's tabu memory.
    struct AppliedMove {
        int kind = 0;  // assignment only: 0=change 1=swap (sequence moves are `list`)
        int a = 0, b = 0, c = 0, d = 0;  // assignment: (entity, previous value) pairs
        ListMove list;                   // sequence: the move itself
        int entities[2] = {0, 0};
        int entity_count = 0;
    };

    // With nothing movable no move, built-in or custom, has a legal target, so
    // run() stops here rather than have every selection handle that case.
    static bool all_entities_pinned(const Solution& sol) {
        const int n = Traits::entity_count(sol);
        for (int e = 0; e < n; ++e)
            if (!entity_is_pinned<Traits>(sol, e)) return false;
        return true;
    }

    AppliedMove apply_basic_move(Editor<Solution>& editor, std::mt19937_64& rng) {
        Solution& solution = editor.solution();
        const SearchSettings& ls = config_.search;
        const MoveMix& ms = ls.moves;
        const int entity_count = Traits::entity_count(solution);
        const int value_count = Traits::value_count(solution);
        std::uniform_int_distribution<int> entity_pick(0, entity_count - 1);
        std::uniform_int_distribution<int> value_pick(0, value_count - 1);
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        int e = ls.entity_hint ? ls.entity_hint(rng) : -1;
        if (e < 0) e = entity_pick(rng);
        e = movable_entity(solution, e, entity_pick, rng);  // never move a pinned entity
        const int old_value = Traits::value_of(solution, e);

        const double swap_weight = entity_count < 2 ? 0.0 : ms.swap_weight;
        const double total = ms.change_weight + swap_weight;

        AppliedMove move;
        if (unit(rng) * total < ms.change_weight || total <= 0) {
            int v = ls.value_hint ? ls.value_hint(rng, e) : -1;
            if (v < 0 || (value_count > 1 && v == old_value)) {
                if (value_count > 1 && old_value >= 0 && old_value < value_count) {
                    v = static_cast<int>(rng() % static_cast<std::uint64_t>(value_count - 1));
                    if (v >= old_value) ++v;
                } else {
                    v = value_pick(rng);
                }
            }
            editor.assign(e, v);
            move.kind = 0;
            move.a = e;
            move.b = old_value;
            move.entities[0] = e;
            move.entity_count = 1;
        } else {
            // Swap partner: nearby-biased when a distance meter is configured.
            int e2;
            if (use_nearby(rng) && !nearby_.of(e).empty()) {
                e2 = random_of(nearby_.of(e), rng);
            } else {
                e2 = entity_pick(rng);
            }
            e2 = movable_entity(solution, e2, entity_pick, rng);  // no pinned swap partner
            for (int attempt = 0; attempt < 8 &&
                                  (e2 == e || Traits::value_of(solution, e2) == old_value);
                 ++attempt) {
                e2 = movable_entity(solution, entity_pick(rng), entity_pick, rng);
            }
            if (e2 == e || Traits::value_of(solution, e2) == old_value) {
                for (int candidate = 0; candidate < entity_count; ++candidate) {
                    if (candidate != e && !entity_is_pinned<Traits>(solution, candidate) &&
                        Traits::value_of(solution, candidate) != old_value) {
                        e2 = candidate;
                        break;
                    }
                }
            }
            const int old_value2 = Traits::value_of(solution, e2);
            editor.begin_batch();
            editor.assign(e, old_value2);
            editor.assign(e2, old_value);
            editor.end_batch();
            move.kind = 1;
            move.a = e;
            move.b = old_value;
            move.c = e2;
            move.d = old_value2;
            move.entities[0] = e;
            move.entities[1] = e2;
            move.entity_count = 2;
        }
        return move;
    }

    static int count_list_elements(const Solution& solution) {
        int total = 0;
        for (int l = 0; l < Traits::list_count(solution); ++l) {
            total += Traits::list_size(solution, l);
        }
        return total;
    }

    // Draw one sequence move without touching the solution. Every coordinate is
    // in the CURRENT solution, with the one convention the objective previews
    // share: a relocate / or-opt insertion index (`d`) indexes the destination
    // list AFTER the moved run has been removed, which only differs from the
    // pre-move index when source and destination are the same list.
    AppliedMove sample_list_move(Editor<Solution>& editor, std::mt19937_64& rng) {
        Solution& solution = editor.solution();
        const SearchSettings& ls = config_.search;
        const MoveMix& ms = ls.moves;
        const int list_count = Traits::list_count(solution);
        const int element_count = Traits::element_count(solution);
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        // During local search every element is assigned, so the element index
        // gives a uniformly random (list, position) in O(1).
        int e1 = static_cast<int>(rng() % static_cast<std::uint64_t>(element_count));
        int l1 = index_.list_of(e1), i1 = index_.position_of(e1);

        // Union move selector: weighted pick among the applicable moves.
        const double w_relocate = ms.relocate_weight;
        const double w_swap = ms.list_swap_weight;
        const int size1 = Traits::list_size(solution, l1);
        const double w_two_opt = size1 < 2 ? 0.0 : ms.two_opt_weight;
        const double w_or_opt = element_count < 3 ? 0.0 : ms.or_opt_weight;
        const double w_tail_swap = list_count < 2 ? 0.0 : ms.tail_swap_weight;
        const double w_inter_exchange =
                list_count < 2 ? 0.0 : ms.cross_exchange_weight;
        const double total =
                w_relocate + w_swap + w_two_opt + w_or_opt + w_tail_swap + w_inter_exchange;

        double dice = unit(rng) * total;
        AppliedMove move;
        if (dice < w_relocate || element_count < 2 || total <= 0) {
            // Relocate: move one element; the target is next to a nearby
            // neighbor when a distance meter is configured, else uniform.
            int l2, i2;
            if (use_nearby(rng) && !nearby_.of(e1).empty()) {
                int nb = random_of(nearby_.of(e1), rng);
                l2 = index_.list_of(nb);
                int q = index_.position_of(nb);
                if (l2 == l1 && q > i1) --q;  // nb shifts down once e1 is out
                i2 = q + 1;                   // insert right after the neighbor
            } else {
                l2 = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
                int size2 = Traits::list_size(solution, l2) - (l2 == l1 ? 1 : 0);
                i2 = static_cast<int>(rng() % static_cast<std::uint64_t>(size2 + 1));
            }
            move.list = ListMove::segment(l1, i1, 1, l2, i2, e1);
            move.entities[0] = e1;
            move.entity_count = 1;
        } else if (dice < w_relocate + w_swap) {
            // Swap two elements (possibly across lists); nearby-biased partner.
            int e2;
            if (use_nearby(rng) && !nearby_.of(e1).empty()) {
                e2 = random_of(nearby_.of(e1), rng);
                if (e2 == e1) e2 = random_other(element_count, e1, rng);
            } else {
                e2 = random_other(element_count, e1, rng);
            }
            int l2 = index_.list_of(e2), i2 = index_.position_of(e2);
            move.list = ListMove::swap(l1, i1, l2, i2, e1, e2);
            move.entities[0] = e1;
            move.entities[1] = e2;
            move.entity_count = 2;
        } else if (dice < w_relocate + w_swap + w_two_opt) {
            // 2-opt: reverse a segment within e1's list.
            int i2 = random_other(size1, i1, rng);
            int first = std::min(i1, i2), last = std::max(i1, i2);
            move.list = ListMove::reverse(l1, first, last);
            move.entities[0] = Traits::element_at(solution, l1, first);
            move.entities[1] = Traits::element_at(solution, l1, last);
            move.entity_count = 2;
        } else if (dice < w_relocate + w_swap + w_two_opt + w_or_opt) {
            // Or-opt: relocate a short segment (2-3 consecutive elements)
            // intact, nearby-biased like relocate.
            int seg_len = 2 + static_cast<int>(rng() % 2);
            if (seg_len > size1) seg_len = size1;
            int start = std::min(i1, size1 - seg_len);
            const int seg0 = Traits::element_at(solution, l1, start);
            int l2 = -1, i2 = 0;
            if (use_nearby(rng) && !nearby_.of(seg0).empty()) {
                int nb = random_of(nearby_.of(seg0), rng);
                const int nb_list = index_.list_of(nb);
                int q = index_.position_of(nb);
                const bool in_segment = nb_list == l1 && q >= start && q < start + seg_len;
                if (!in_segment) {  // neighbor not part of the segment
                    l2 = nb_list;
                    if (l2 == l1 && q >= start + seg_len) q -= seg_len;
                    i2 = q + 1;
                }
            }
            if (l2 < 0) {
                l2 = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
                int size2 = Traits::list_size(solution, l2) - (l2 == l1 ? seg_len : 0);
                i2 = static_cast<int>(rng() % static_cast<std::uint64_t>(size2 + 1));
            }
            move.list = ListMove::segment(l1, start, seg_len, l2, i2, seg0);
            move.entities[0] = seg0;
            move.entities[1] = Traits::element_at(solution, l1, start + seg_len - 1);
            move.entity_count = 2;
        } else if (dice < w_relocate + w_swap + w_two_opt + w_or_opt + w_tail_swap) {
            // 2-opt* (tail swap): exchange the tails of two lists — the
            // classic inter-route move. Cut l1 right after e1's position.
            int l2 = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count - 1));
            if (l2 >= l1) ++l2;
            int c1 = i1;
            int size2 = Traits::list_size(solution, l2);
            int c2 = static_cast<int>(rng() % static_cast<std::uint64_t>(size2 + 1));
            move.entities[0] = e1;
            move.entities[1] = c2 < size2 ? Traits::element_at(solution, l2, c2) : e1;
            move.entity_count = 2;
            move.list = ListMove::tail_swap(l1, c1, l2, c2);
        } else {
            // CROSS-exchange: swap a run starting at e1 with a run in another
            // list, the two runs sized independently. The partner list is
            // chosen through a nearby neighbour when a distance meter is
            // configured — exchanging with a spatially unrelated route is
            // almost never an improvement.
            int l2 = -1, i2 = 0;
            if (use_nearby(rng) && !nearby_.of(e1).empty()) {
                const int nb = random_of(nearby_.of(e1), rng);
                if (index_.list_of(nb) >= 0 && index_.list_of(nb) != l1) {
                    l2 = index_.list_of(nb);
                    i2 = index_.position_of(nb);
                }
            }
            if (l2 < 0) {
                l2 = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count - 1));
                if (l2 >= l1) ++l2;
                const int size2 = Traits::list_size(solution, l2);
                i2 = size2 == 0 ? 0
                                : static_cast<int>(rng() %
                                                   static_cast<std::uint64_t>(size2));
            }
            const int size2 = Traits::list_size(solution, l2);
            const int max_segment = std::max(1, ms.cross_exchange_max_run);
            // One side may take zero elements, which degenerates to relocating
            // the other side's run — the cheap way to shorten a route.
            int len1 = static_cast<int>(rng() %
                                        static_cast<std::uint64_t>(max_segment + 1));
            int len2 = static_cast<int>(rng() %
                                        static_cast<std::uint64_t>(max_segment + 1));
            len1 = std::min(len1, size1 - i1);
            len2 = std::min(len2, size2 - i2);
            if (len1 <= 0 && len2 <= 0) {
                // e1 guarantees that this side has at least one available
                // element. Turn the zero/zero draw into a one-way segment
                // relocate instead of spending the step on a no-op.
                len1 = 1;
            }
            move.entities[0] = e1;
            move.entities[1] = len2 > 0 ? Traits::element_at(solution, l2, i2) : e1;
            move.entity_count = 2;
            move.list = ListMove::exchange(l1, i1, len1, l2, i2, len2);
        }
        return move;
    }

    // Ask the objective for the score the sampled move would produce. False
    // when it offers no preview for this move: apply it and score it instead.
    bool preview_list_move(Editor<Solution>& editor, const AppliedMove& move,
                         Score& out) {
        return ListMoves<Solution>::preview(editor.objective(), move.list, out);
    }

    // Score a sampled sequence move for the step and polish loops. Returns true
    // when the move is NOT applied on return: its preview was served and
    // `score` is that preview, so the caller applies the move only if it
    // accepts it. Returns false when the move IS applied on return (the
    // preview was declined, or ScoreChecks::Full applied it to check the preview
    // against the applied score, throwing ScoreMismatchError on a
    // mismatch) and `score` is the applied score; the caller undoes a
    // rejection. One evaluate_with_preview with no undo, so every preview in
    // the engine is verified by the same lines.
    bool score_list_move(Editor<Solution>& editor, const AppliedMove& move,
                       Score& score) {
        bool applied = false;
        score = evaluate_with_preview(
                editor,
                [&](Objective<Solution>&, Score& out) {
                    return preview_list_move(editor, move, out);
                },
                [&] { apply_list_move(editor, move); }, [] {}, /*served=*/nullptr, &applied);
        return !applied;  // the no-op undo left an applied move in place
    }

    // Apply a sampled sequence move; undo_move() inverts it.
    void apply_list_move(Editor<Solution>& editor, const AppliedMove& move) {
        ListMoves<Solution>::apply(editor, index_, move.list);
    }

    void undo_move(Editor<Solution>& editor, const AppliedMove& move) {
        if constexpr (Traits::shape == Shape::Assignment) {
            switch (move.kind) {
                case 0:  // change
                    editor.assign(move.a, move.b);
                    break;
                case 1:  // swap
                    editor.begin_batch();
                    editor.assign(move.a, move.b);
                    editor.assign(move.c, move.d);
                    editor.end_batch();
                    break;
            }
        } else {
            ListMoves<Solution>::undo(editor, index_, move.list);
        }
    }

    // --- LNS: adaptive ruin & recreate ---

    // One ruin-and-recreate kick. The result becomes the new incumbent
    // unconditionally (perturbation-style); outcomes are still classified to
    // adapt the ruin-operator weights.
    Score lns_iteration(Editor<Solution>& editor, Score current_score,
                               const Score& best_score,
                               OperatorSelectionScheme& ruin_selector,
                               const std::vector<RuinOperator<Solution>*>& ruins,
                               OperatorContext<Solution>& operator_context,
                               std::mt19937_64& rng) {
        Solution& solution = editor.solution();
        const RuinRecreateSettings& lns = config_.search.ruin_recreate;
        const int n = GreedyRepair<Solution>::candidate_count(solution);
        std::uniform_real_distribution<double> fraction(lns.min_ruin_fraction,
                                                        lns.max_ruin_fraction);
        int ruin_count = std::clamp(static_cast<int>(fraction(rng) * n), 1,
                                   std::min(n - 1, lns.max_ruin_count));

        int op = ruin_selector.select(rng);
        std::vector<int> ruined = ruins[op]->select_ruin_set(operator_context, ruin_count, rng);
        // Defensive: a ruin set must be duplicate-free (the built-ins are by
        // construction; a custom operator is not required to be) and must not
        // contain a pinned entity. dedupe() is order-preserving, which is
        // load-bearing — see its comment.
        repair_.dedupe(editor, ruined, n);
        repair_.ruin(editor, ruined);
        repair_.recreate(editor, ruined, rng);

        // Polish the recreated solution with hill-climbing small moves so the
        // outcome classification below reflects the kick's real potential.
        // sequence moves are previewed like the main step loop's: a rejected
        // polish move is never applied.
        Score candidate = editor.score();
        for (std::int64_t i = 0; i < lns.polish_steps; ++i) {
            AppliedMove move;
            Score polished;
            bool pending = false;  // scored by preview, not applied
            if constexpr (Traits::shape == Shape::Assignment) {
                move = apply_basic_move(editor, rng);
                polished = editor.score();
            } else {
                move = sample_list_move(editor, rng);
                pending = score_list_move(editor, move, polished);
            }
            if (polished >= candidate) {
                if constexpr (Traits::shape == Shape::Sequence) {
                    if (pending) apply_list_move(editor, move);
                }
                candidate = polished;
            } else if (!pending) {
                undo_move(editor, move);
            }
        }
        MoveOutcome outcome;
        if (candidate > best_score) {
            outcome = MoveOutcome::NewBest;
        } else if (candidate > current_score) {
            outcome = MoveOutcome::Better;
        } else if (candidate == current_score) {
            outcome = MoveOutcome::Accepted;
        } else {
            outcome = MoveOutcome::Rejected;  // kept anyway; scored for adaptation
        }
        ruin_selector.update(op, outcome);
        return candidate;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_LOCAL_SEARCH_HPP
