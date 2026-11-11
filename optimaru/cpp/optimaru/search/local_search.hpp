// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_LOCAL_SEARCH_HPP
#define OPTIMARU_SEARCH_LOCAL_SEARCH_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/score/levels.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/portable_random.hpp"
#include "optimaru/ruin_recreate/removal_gain.hpp"
#include "optimaru/select/schemes.hpp"
#include "optimaru/ruin_recreate/random_ruin.hpp"
#include "optimaru/ruin_recreate/related_ruin.hpp"
#include "optimaru/ruin_recreate/route_ruin.hpp"
#include "optimaru/ruin_recreate/sisr_ruin.hpp"
#include "optimaru/ruin_recreate/worst_ruin.hpp"
#include "optimaru/accept/acceptors.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/ruin_recreate/greedy_repair.hpp"
#include "optimaru/moves/list_move.hpp"
#include "optimaru/moves/movable_entity.hpp"
#include "optimaru/model/snapshot.hpp"
#include "optimaru/moves/descent.hpp"
#include "optimaru/moves/ejection_chain.hpp"
#include "optimaru/moves/preview_evaluation.hpp"
#include "optimaru/moves/iterated_local_search.hpp"
#include "optimaru/moves/route_elimination.hpp"
#include "optimaru/ruin_recreate/ruin_recreate_move.hpp"
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
// branch in this file: the built-in strategies in moves/ and ruin_recreate/ implement the
// same MoveOperator interface a caller gets from Solver::add_move_operator(),
// and compete for selection in one adaptive pool with them.
template <typename Solution>
class LocalSearch {
    using Traits = SolutionTraits<Solution>;
    using Snapshot = SolutionSnapshot<Solution>;

public:
    LocalSearch(const Settings& config, const Neighborhoods& nearby,
                const std::vector<std::unique_ptr<MoveOperator<Solution>>>& move_operators,
                const std::vector<std::unique_ptr<RuinOperator<Solution>>>& ruin_operators,
                const std::function<std::unique_ptr<Acceptor>(const Score&)>& acceptor_factory)
        : config_(config),
          nearby_(nearby),
          move_operators_(move_operators),
          ruin_operators_(ruin_operators),
          acceptor_factory_(acceptor_factory),
          repair_(index_, nearby, config.search.ruin_recreate, config.search.unassigned_list) {}

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
        Snapshot best = Snapshot::take(solution);
        if (config_.on_new_best) config_.on_new_best(0, best_score);

        std::unique_ptr<Acceptor> acceptor = make_acceptor(current_score);
        // Custom acceptors may consume the time gradient. Of the built-ins,
        // only simulated annealing does; avoid a duration conversion and
        // floating-point division on every move for the default LA path.
        const bool needs_time_gradient =
                static_cast<bool>(acceptor_factory_) ||
                config_.search.acceptance.rule == AcceptanceRule::SimulatedAnnealing;
        // The built-in ruins are plugins like any other: one file each in
        // ruin_recreate/, registered ahead of the caller's operators so the
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
        const auto make_selector = [&lns](std::size_t operator_count) {
            return make_operator_selection_scheme(
                    lns.selection_type, static_cast<int>(operator_count), lns.rewards,
                    lns.operator_decay, lns.selection_segment_length, lns.selection_alpha);
        };
        auto ruin_selector = make_selector(ruins.size());
        // Adaptive selection over {built-in move mix, custom operators...};
        // only consulted when custom move operators are registered.
        auto move_selector = make_selector(1 + move_operators_.size());
        OperatorContext<Solution> operator_context{editor, nearby_, index_.lists(),
                                                   index_.positions()};

        // The built-in strategies: plugins like any other MoveOperator, drawn
        // by weight before the ordinary move mix gets its turn. Their weights
        // are small by necessity — each costs orders of magnitude more than one
        // sampled move — and the header of SearchSettings records why that
        // is a configured number here rather than something the adaptive
        // selector above is asked to discover.
        [[maybe_unused]] RouteElimination<Solution> route_elimination(
                index_, repair_, ls.route_elimination_max_size);
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
        auto ils_ruin_selector = make_selector(ruins.size());
        [[maybe_unused]] IteratedLocalSearchMove<Solution> iterated_local_search(
                index_, repair_, granular_local_search, ruins, *ils_ruin_selector,
                ls.iterated_local_search_min_ruin, ls.iterated_local_search_max_ruin,
                ls.unassigned_list, ls.iterated_local_search_max_evaluations);
        std::vector<WeightedStrategy> strategies;
        // Route elimination is sequence-only, for the same reason the SISR and
        // route ruins are: it declines on assignment solutions, so registering it
        // there would only burn steps on an operator that can never move.
        // Descent has a neighbourhood on both shapes and is registered on both.
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
        // Late acceptance in stages of growing history
        // (AcceptanceSettings::history_growth).
        HistoryStages stages(config_, acceptor_factory_ != nullptr, current_score, stop.progress());

        while (!stop.should_stop(step, best_score, force_time_check)) {
            force_time_check = false;
            std::int64_t patience = std::max(lns_trigger, 8 * ema_best_gap);
            // A stage has converged only when a whole history has gone by
            // without an improving move.
            if (stages.enabled()) patience = std::max(patience, stages.min_patience());
            // Stalled: the next stage if one is due, else a kick, else wait.
            const bool stalled = steps_since_best_or_lns >= patience;
            if (stalled && stages.enabled() && stages.on_converged(stop.progress(), !lns.enabled)) {
                acceptor = stages.start_next(stop.progress());
                steps_since_best_or_lns = 0;
            } else if (stalled && lns.enabled) {
                current_score = lns_iteration(editor, current_score, best_score, *ruin_selector,
                                              ruins, operator_context, rng);
                force_time_check = true;
                // A perturbation kick becomes the new incumbent uncondition-
                // ally and the acceptor restarts fresh, so
                // the following small moves re-polish the kicked solution
                // instead of instantly rejecting it (best is snapshotted, so
                // a bad kick can never worsen the returned solution).
                // (In stages: with the history of the stage the search is in.)
                acceptor = stages.enabled() ? stages.acceptor(current_score)
                                            : make_acceptor(current_score);
                steps_since_best_or_lns = 0;
                lns_trigger = std::min(lns_trigger * 2, lns_trigger_cap);
            } else if (stalled && stages.enabled()) {
                // No kicks, and the next stage is not due: wait and ask again.
                // (Once a stage no longer fits in the budget none will: the
                // search goes on with the history it has.)
                steps_since_best_or_lns = 0;
            } else {
                // A built-in strategy plugin if one is drawn; otherwise a
                // caller's operator or the ordinary move mix (op == nullptr),
                // chosen by the adaptive selector when custom operators are
                // registered. `selector_index` >= 0: the selector was asked
                // and hears the outcome (0 is the move mix).
                MoveOperator<Solution>* op = draw_strategy(strategies, rng);
                int selector_index = -1;
                if (op == nullptr && custom_operators_get_a_turn(rng)) {
                    selector_index = move_selector->select(rng);
                    if (selector_index > 0) {
                        op = move_operators_[static_cast<std::size_t>(selector_index) - 1].get();
                    }
                }
                const bool custom = selector_index > 0;

                // The candidate: its score, the entities it touched (for the
                // acceptor's tabu memory), and whether it is in place.
                // `previewed`: scored from the objective's cached data without
                // touching the solution, and applied only if accepted. Most
                // moves are rejected, so this is where the apply/score/undo
                // cost of a step disappears.
                AppliedMove move;  // the move mix's own move
                bool previewed = false;
                bool applied = true;  // false: the operator found no move
                Score candidate_score;
                const int* entities = move.entities;
                int entity_count = 0;
                editor.begin_trial();
                if (op == nullptr) {
                    previewed = sample_move(editor, rng, move, candidate_score);
                    entity_count = move.entity_count;
                } else {
                    if (custom) editor.track_touched_lists();
                    custom_entities.clear();
                    applied = op->apply(operator_context, rng, custom_entities);
                    if (applied) candidate_score = editor.score();
                    entities = custom_entities.data();
                    entity_count = static_cast<int>(custom_entities.size());
                }
                if (applied) {
                    const bool accepted = acceptor->accepts(
                            candidate_score, current_score, best_score, entities, entity_count,
                            needs_time_gradient ? stop.progress() : 0.0, rng);
                    const MoveOutcome outcome =
                            classify(candidate_score, current_score, best_score, accepted);
                    if (accepted) {
                        // In stages, stagnation counts from the last improving
                        // move: a stage starts far below the best. (Strictly
                        // improving, on the score as it is compared: a use case
                        // whose soft level carries rounding noise would see
                        // sideways moves as improving here and never stall.)
                        if (stages.enabled() && candidate_score > current_score) {
                            steps_since_best_or_lns = -1;
                        }
                        current_score = candidate_score;
                        acceptor->on_accepted(entities, entity_count);
                        if (previewed) apply_previewed(editor, move);
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
                if (custom) {
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
                Snapshot::capture(best, solution);  // reuses the snapshot's storage
                stop.on_improvement(step);
                stages.on_new_best(stop.progress());
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
    // Late acceptance in stages of growing history: see
    // AcceptanceSettings::history_growth. Times are fractions of the solve
    // (StopCondition::progress()).
    class HistoryStages {
    public:
        HistoryStages(const Settings& config, bool custom_acceptor, const Score& start_score,
                      double now)
            : growth_(config.search.acceptance.history_growth),
              enabled_(growth_ > 1.0 && !custom_acceptor &&
                       config.search.acceptance.rule == AcceptanceRule::LateAcceptance),
              history_(std::clamp<std::size_t>(config.search.acceptance.history_length, 1,
                                               kMaxHistory)),
              start_score_(start_score),
              stage_start_(now),
              last_best_(now) {}

        bool enabled() const { return enabled_; }
        std::int64_t min_patience() const { return 2 * static_cast<std::int64_t>(history_); }

        // Tells the stages that the search has converged, and returns whether
        // the next stage starts now. It records how long the stage that just
        // ended took (the first call: a stage of the configured length).
        bool on_converged(double now, bool no_kicks) {
            if (stage_cost_ < 0.0 || in_stage_) stage_cost_ = now - stage_start_;
            in_stage_ = false;
            const double next_cost = stage_cost_ * static_cast<double>(next_history()) /
                                     static_cast<double>(history_);
            const bool stalled = no_kicks || now - last_best_ >= 0.5 * next_cost;
            return stalled && now + next_cost <= 1.0;
        }

        std::unique_ptr<Acceptor> start_next(double now) {
            history_ = next_history();
            stage_start_ = now;
            in_stage_ = true;
            return acceptor(start_score_);
        }

        std::unique_ptr<Acceptor> acceptor(const Score& initial) const {
            return std::make_unique<LateAcceptance>(history_, initial);
        }

        void on_new_best(double now) { last_best_ = now; }

    private:
        // A history holds one Score per step it remembers, so its length is
        // memory: a long budget on a small instance would otherwise double it
        // without end. At the cap a stage restarts with the same length, an
        // independent run of the search.
        static constexpr std::size_t kMaxHistory = std::size_t{1} << 20;

        // Always longer than the current one below the cap, also for a factor
        // close to 1 on a short history.
        std::size_t next_history() const {
            const double grown = std::ceil(static_cast<double>(history_) * growth_);
            const std::size_t next = grown >= static_cast<double>(kMaxHistory)
                                             ? kMaxHistory
                                             : static_cast<std::size_t>(grown);
            return std::min(kMaxHistory, std::max(history_ + 1, next));
        }

        double growth_;
        bool enabled_;
        std::size_t history_;
        Score start_score_;
        double stage_start_;
        double stage_cost_ = -1.0;
        double last_best_;
        bool in_stage_ = false;
    };

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
    MoveOperator<Solution>* draw_strategy(const std::vector<WeightedStrategy>& strategies,
                                          std::mt19937_64& rng) {
        for (const WeightedStrategy& s : strategies) {
            if (unit_(rng) < s.weight) return s.op;
        }
        return nullptr;
    }

    // Whether this step asks the adaptive selector to choose between the move
    // mix and the caller's operators (SearchSettings::custom_move_probability).
    bool custom_operators_get_a_turn(std::mt19937_64& rng) {
        if (move_operators_.empty()) return false;
        const double gate = config_.search.custom_move_probability;
        return gate >= 1.0 || (gate > 0.0 && unit_(rng) < gate);
    }

    // How a candidate fared, for the adaptive selectors.
    static MoveOutcome classify(const Score& candidate, const Score& current, const Score& best,
                                bool accepted) {
        return candidate > best      ? MoveOutcome::NewBest
               : candidate > current ? MoveOutcome::Better
               : accepted            ? MoveOutcome::Accepted
                                     : MoveOutcome::Rejected;
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
    // in moves/ and ruin_recreate/ that do the same thing as an ordinary move.
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
            case AcceptanceRule::SimulatedAnnealing: {
                const bool automatic =
                        c.start_temperature == Score{0, 0.0} && c.start_temperature_fraction > 0.0;
                return std::make_unique<SimulatedAnnealing>(
                        automatic ? automatic_start_temperature(
                                            unfolded_soft<Solution>(initial_score),
                                            c.start_temperature_fraction)
                                  : c.start_temperature);
            }
            case AcceptanceRule::GreatDeluge:
                return std::make_unique<GreatDeluge>(initial_score, c.water_level_step);
            case AcceptanceRule::StepCountingHillClimbing:
                return std::make_unique<StepCountingHillClimbing>(c.recount_interval,
                                                                  initial_score);
            case AcceptanceRule::RecordToRecordTravel: {
                // The threshold schedule spans the whole solve, so it needs a
                // step count. Prefer the configured limit; otherwise assume a
                // long run (10^6 steps) — overshooting only makes the
                // threshold decay more slowly, which RRT tolerates, whereas
                // undershooting collapses it to hill climbing immediately.
                const std::int64_t steps =
                        config_.limits.step_limit > 0 ? config_.limits.step_limit : 1000000;
                return std::make_unique<RecordToRecordTravel>(RecordToRecordTravel::autofit(
                        initial_score.soft, c.rrt_start_gap, c.rrt_end_gap, steps));
            }
            case AcceptanceRule::LateAcceptance:
            default:
                return std::make_unique<LateAcceptance>(c.history_length, initial_score);
        }
    }

    // A random nearby neighbour of `e`, drawn with NeighborSettings::probability
    // when nearby selection is configured; -1 means draw uniformly instead.
    int nearby_pick(int e, std::mt19937_64& rng) {
        if (!nearby_.enabled() || !(unit_(rng) < config_.search.nearby.probability)) return -1;
        const std::vector<int>& neighbours = nearby_.of(e);
        return neighbours.empty() ? -1 : neighbours[rng() % neighbours.size()];
    }

    // Uniformly draw from [0, n) except one value, without rejection. This is
    // used by moves where drawing the current entity/position is a guaranteed
    // no-op. Besides wasting a score evaluation, accepted no-ops advance Late
    // Acceptance history and dilute the useful neighbourhood.
    static int random_other(int n, int excluded, std::mt19937_64& rng) {
        int picked = random_index(rng, n - 1);
        if (picked >= excluded) ++picked;
        return picked;
    }

    // --- moves ---

    // One applied move, with what is needed to reject it and to feed the
    // acceptor's tabu memory.
    struct AppliedMove {
        int entities[2] = {0, 0};  // the entities / elements the move is about
        int entity_count = 0;
        int previous[2] = {0, 0};  // assignment: the value each entity held before
        ListMove list;             // sequence: the move itself
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

        int e = ls.entity_hint ? ls.entity_hint(rng) : -1;
        if (e < 0) e = entity_pick(rng);
        e = movable_entity(solution, e, entity_pick, rng);  // never move a pinned entity
        const int old_value = Traits::value_of(solution, e);

        const double swap_weight = entity_count < 2 ? 0.0 : ms.swap_weight;
        const double total = ms.change_weight + swap_weight;

        AppliedMove move;
        move.entities[0] = e;
        move.previous[0] = old_value;
        if (unit_(rng) * total < ms.change_weight || total <= 0) {
            // Change: another value for e (never its current one, when there
            // is a choice).
            int v = ls.value_hint ? ls.value_hint(rng, e) : -1;
            if (v < 0 || (value_count > 1 && v == old_value)) {
                if (value_count > 1 && old_value >= 0 && old_value < value_count) {
                    v = random_other(value_count, old_value, rng);
                } else {
                    v = value_pick(rng);
                }
            }
            editor.assign(e, v);
            move.entity_count = 1;
        } else {
            // Swap partner: nearby-biased when nearby selection is configured.
            int e2 = nearby_pick(e, rng);
            if (e2 < 0) e2 = entity_pick(rng);
            e2 = movable_entity(solution, e2, entity_pick, rng);  // no pinned swap partner
            for (int attempt = 0;
                 attempt < 8 && (e2 == e || Traits::value_of(solution, e2) == old_value);
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
            move.entities[1] = e2;
            move.previous[1] = old_value2;
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

        // During local search every element is assigned, so the element index
        // gives a uniformly random (list, position) in O(1).
        const int e1 = random_index(rng, element_count);
        const int l1 = index_.list_of(e1), i1 = index_.position_of(e1);

        // Union move selector: weighted pick among the applicable moves.
        const double w_relocate = ms.relocate_weight;
        const double w_swap = ms.list_swap_weight;
        const int size1 = Traits::list_size(solution, l1);
        const double w_two_opt = size1 < 2 ? 0.0 : ms.two_opt_weight;
        const double w_or_opt = element_count < 3 ? 0.0 : ms.or_opt_weight;
        const double w_tail_swap = list_count < 2 ? 0.0 : ms.tail_swap_weight;
        const double w_inter_exchange = list_count < 2 ? 0.0 : ms.cross_exchange_weight;
        const double total =
                w_relocate + w_swap + w_two_opt + w_or_opt + w_tail_swap + w_inter_exchange;

        const double dice = unit_(rng) * total;
        AppliedMove move;
        if (dice < w_relocate || element_count < 2 || total <= 0) {
            // Relocate: move one element; the target is next to a nearby
            // neighbour when nearby selection is configured, else uniform.
            int l2, i2;
            const int nb = nearby_pick(e1, rng);
            if (nb >= 0) {
                l2 = index_.list_of(nb);
                int q = index_.position_of(nb);
                if (l2 == l1 && q > i1) --q;  // nb shifts down once e1 is out
                i2 = q + 1;                   // insert right after the neighbour
            } else {
                l2 = random_index(rng, list_count);
                const int size2 = Traits::list_size(solution, l2) - (l2 == l1 ? 1 : 0);
                i2 = random_index(rng, size2 + 1);
            }
            move.list = ListMove::segment(l1, i1, 1, l2, i2, e1);
            move.entities[0] = e1;
            move.entity_count = 1;
        } else if (dice < w_relocate + w_swap) {
            // Swap two elements (possibly across lists); nearby-biased partner.
            int e2 = nearby_pick(e1, rng);
            if (e2 < 0 || e2 == e1) e2 = random_other(element_count, e1, rng);
            const int l2 = index_.list_of(e2), i2 = index_.position_of(e2);
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
            int seg_len = 2 + random_index(rng, 2);
            if (seg_len > size1) seg_len = size1;
            int start = std::min(i1, size1 - seg_len);
            const int seg0 = Traits::element_at(solution, l1, start);
            int l2 = -1, i2 = 0;
            const int nb = nearby_pick(seg0, rng);
            if (nb >= 0) {
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
                l2 = random_index(rng, list_count);
                const int size2 = Traits::list_size(solution, l2) - (l2 == l1 ? seg_len : 0);
                i2 = random_index(rng, size2 + 1);
            }
            move.list = ListMove::segment(l1, start, seg_len, l2, i2, seg0);
            move.entities[0] = seg0;
            move.entities[1] = Traits::element_at(solution, l1, start + seg_len - 1);
            move.entity_count = 2;
        } else if (dice < w_relocate + w_swap + w_two_opt + w_or_opt + w_tail_swap) {
            // 2-opt* (tail swap): exchange the tails of two lists — the
            // classic inter-route move. Cut l1 right after e1's position.
            const int l2 = random_other(list_count, l1, rng);
            const int size2 = Traits::list_size(solution, l2);
            const int c2 = random_index(rng, size2 + 1);
            move.entities[0] = e1;
            move.entities[1] = c2 < size2 ? Traits::element_at(solution, l2, c2) : e1;
            move.entity_count = 2;
            move.list = ListMove::tail_swap(l1, i1, l2, c2);
        } else {
            // CROSS-exchange: swap a run starting at e1 with a run in another
            // list, the two runs sized independently. The partner list is
            // chosen through a nearby neighbour when nearby selection is
            // configured — exchanging with a spatially unrelated route is
            // almost never an improvement.
            int l2 = -1, i2 = 0;
            const int nb = nearby_pick(e1, rng);
            if (nb >= 0 && index_.list_of(nb) >= 0 && index_.list_of(nb) != l1) {
                l2 = index_.list_of(nb);
                i2 = index_.position_of(nb);
            }
            if (l2 < 0) {
                l2 = random_other(list_count, l1, rng);
                const int size2 = Traits::list_size(solution, l2);
                i2 = size2 == 0 ? 0 : random_index(rng, size2);
            }
            const int size2 = Traits::list_size(solution, l2);
            const int max_segment = std::max(1, ms.cross_exchange_max_run);
            // One side may take zero elements, which degenerates to relocating
            // the other side's run — the cheap way to shorten a route.
            int len1 = random_index(rng, max_segment + 1);
            int len2 = random_index(rng, max_segment + 1);
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

    // Draw one move of the built-in mix and score it; the step loop and the
    // kick's polish loop both take their candidates from here. Returns true
    // when the move is NOT applied on return: its preview was served and
    // `score` is that preview, so the caller applies it (apply_previewed) only
    // if it keeps it. Returns false when the move IS applied on return and
    // `score` is the applied score; the caller undoes a rejection (undo_move).
    //
    // An assignment move is always applied. A sequence move is applied when
    // the objective declines its preview, or when ScoreChecks::Full applies it
    // to check the preview against the applied score (ScoreMismatchError on a
    // mismatch): one evaluate_with_preview with a no-op undo, so every preview
    // in the engine is verified by the same lines.
    bool sample_move(Editor<Solution>& editor, std::mt19937_64& rng, AppliedMove& move,
                     Score& score) {
        if constexpr (Traits::shape == Shape::Assignment) {
            move = apply_basic_move(editor, rng);
            score = editor.score();
            return false;
        } else {
            move = sample_list_move(editor, rng);
            bool applied = false;
            score = evaluate_with_preview(
                    editor,
                    [&](Objective<Solution>& objective, Score& out) {
                        return ListMoves<Solution>::preview(objective, move.list, out);
                    },
                    [&] { ListMoves<Solution>::apply(editor, index_, move.list); }, [] {},
                    /*served=*/nullptr, &applied);
            return !applied;  // the no-op undo left an applied move in place
        }
    }

    // Apply a move sample_move() only previewed.
    void apply_previewed(Editor<Solution>& editor, const AppliedMove& move) {
        if constexpr (Traits::shape == Shape::Sequence) {
            ListMoves<Solution>::apply(editor, index_, move.list);
        } else {
            (void)editor;
            (void)move;
        }
    }

    // Revert a move sample_move() left applied.
    void undo_move(Editor<Solution>& editor, const AppliedMove& move) {
        if constexpr (Traits::shape == Shape::Assignment) {
            if (move.entity_count == 1) {  // change
                editor.assign(move.entities[0], move.previous[0]);
            } else {  // swap
                editor.begin_batch();
                editor.assign(move.entities[0], move.previous[0]);
                editor.assign(move.entities[1], move.previous[1]);
                editor.end_batch();
            }
        } else {
            ListMoves<Solution>::undo(editor, index_, move.list);
        }
    }

    // --- LNS: adaptive ruin & recreate ---

    // One ruin-and-recreate kick. The result becomes the new incumbent
    // unconditionally (perturbation-style); outcomes are still classified to
    // adapt the ruin-operator weights.
    Score lns_iteration(Editor<Solution>& editor, const Score& current_score,
                        const Score& best_score, OperatorSelectionScheme& ruin_selector,
                        const std::vector<RuinOperator<Solution>*>& ruins,
                        OperatorContext<Solution>& operator_context, std::mt19937_64& rng) {
        const RuinRecreateSettings& lns = config_.search.ruin_recreate;
        const int n = ruin_candidate_count(operator_context);
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
        Score candidate = editor.score();
        for (std::int64_t i = 0; i < lns.polish_steps; ++i) {
            AppliedMove move;
            Score polished;
            const bool previewed = sample_move(editor, rng, move, polished);
            if (polished >= candidate) {
                if (previewed) apply_previewed(editor, move);
                candidate = polished;
            } else if (!previewed) {
                undo_move(editor, move);
            }
        }
        // The kick is kept whatever it scored; the outcome only adapts the
        // ruin weights ("accepted" = it held the current score).
        const MoveOutcome outcome =
                classify(candidate, current_score, best_score, candidate == current_score);
        ruin_selector.update(op, outcome);
        return candidate;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_LOCAL_SEARCH_HPP
