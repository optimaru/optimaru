// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_MOVES_DESCENT_HPP
#define OPTIMARU_MOVES_DESCENT_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/moves/list_move.hpp"
#include "optimaru/moves/movable_entity.hpp"
#include "optimaru/moves/preview_evaluation.hpp"
#include "optimaru/stop/stop_throttle.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/portable_random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Descent — best-improvement over one nearby pair
// ---------------------------------------------------------------------------
// The engine's ordinary step samples ONE random move and asks the acceptor.
// That is enough while the instance is small enough for a random draw to land
// on an improvement reasonably often; at a thousand elements it is not, and no
// amount of extra move TYPES fixes it — adding CROSS-exchange to the random
// mix measurably did nothing. What changed the numbers was searching a
// neighbourhood exhaustively instead of sampling it.
//
// So: pick a nearby pair, then repeatedly apply the single best improving move
// over it until the pair is locally optimal or the sweep cap hits. This is the
// difference between "is this one random move an improvement?" and "what is
// the best move available here?". The pair is per shape:
//
//  - sequence:  two geometrically nearby lists; every relocation between them, in
//           both directions. An opt-in granular mode also enumerates
//           Exchange11, 2-opt, 2-opt*, and bounded ExchangeNM/CROSS moves.
//  - assignment: a nearby entity pair; every value of each entity (the same
//           enumeration the construction heuristic and the LNS repair already
//           do for one entity) plus the swap of the two. What makes it a
//           descent rather than a greedy placement is that it keeps sweeping —
//           after e1 takes its best value, e2 is re-evaluated against the new
//           assignment, and so on.
//
// One application is worth O(|l1| * |l2|) score evaluations on sequence and
// ~2 * value_count on assignment, against 1 for an ordinary move. It earns its place
// in the pool only because it nearly always improves — which is also why the
// weight it is drawn at has to be set against what it costs, and why on assignment
// that cost is set by the VALUE RANGE and so the usable weight differs by two
// orders of magnitude between domains (see SearchSettings::descent_weight).
//
// As a plugin this goes through the acceptor like any other move rather than
// being applied directly on the grounds that it is improving by construction.
// It only ever reports a strictly improving result, and every acceptor takes
// those, so the invariant is "relies on the acceptor accepting an
// improvement". Note also that an element relocated on more than one sweep
// appears in `entities` more than once, which entity tabu tolerates.
//
// Both shapes.
template <typename Solution>
class Descent final : public MoveOperator<Solution> {
    using Traits = SolutionTraits<Solution>;

public:
    // `entity_hint` is the assignment-only guided-selection hook: when a use
    // case supplies one, the descent starts where the violations are instead of
    // at a uniform draw. May be empty.
    // `unordered_list` (sequence shape, -1 = none) is a list whose element order
    // carries no score, such as an unassigned pool: reordering moves within
    // it (2-opt, same-list swap) are never improving and are not scanned.
    Descent(ElementIndex<Solution>& index, const Neighborhoods& nearby, int max_sweeps,
            const std::function<int(std::mt19937_64&)>& entity_hint, bool advanced_list = false,
            int list_max_segment = 2, int list_candidate_routes = 4, int unordered_list = -1)
        : index_(index),
          nearby_(nearby),
          max_sweeps_(max_sweeps),
          entity_hint_(entity_hint),
          advanced_list_(advanced_list),
          list_max_segment_(std::max(1, list_max_segment)),
          list_candidate_routes_(std::max(1, list_candidate_routes)),
          unordered_list_(unordered_list) {}

    const char* name() const override { return "descent"; }

    bool apply(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        applied_.clear();
        assignments_.clear();
        if constexpr (Traits::shape == Shape::Assignment) {
            return apply_basic(ctx, rng, entities);
        } else {
            Editor<Solution>& editor = ctx.editor;
            Solution& solution = editor.solution();
            const int list_count = Traits::list_count(solution);
            const int element_count = Traits::element_count(solution);
            if (list_count < 2 || element_count < 2) return false;
            throttle_.reset(ctx.stop_requested);
            evaluations_ = 0;

            const int seed = random_index(rng, element_count);
            const int l1 = index_.list_of(seed);
            if (l1 < 0) return false;

            // The pair: the seed's list and one of the closest routes that
            // contains a granular-neighbour. Sampling from a bounded candidate
            // set rather than considering every route pair keeps the
            // exhaustive move scan predictable on large instances while
            // avoiding unrelated route exchanges.
            int l2 = -1;
            std::vector<int> route_candidates;
            if (advanced_list_ && nearby_.enabled()) {
                for (int nb : nearby_.of(seed)) {
                    const int list = index_.list_of(nb);
                    if (list < 0 || list == l1) continue;
                    if (std::find(route_candidates.begin(), route_candidates.end(), list) ==
                        route_candidates.end()) {
                        route_candidates.push_back(list);
                        if (static_cast<int>(route_candidates.size()) >= list_candidate_routes_)
                            break;
                    }
                }
            } else if (nearby_.enabled()) {
                for (int nb : nearby_.of(seed)) {
                    const int list = index_.list_of(nb);
                    if (list >= 0 && list != l1) {
                        route_candidates.push_back(list);
                        break;
                    }
                }
            }
            if (!route_candidates.empty()) {
                l2 = route_candidates[static_cast<std::size_t>(rng() % route_candidates.size())];
            }
            if (l2 < 0) {
                l2 = random_index(rng, list_count - 1);
                if (l2 >= l1) ++l2;
            }

            Score current = editor.score();
            for (int sweep = 0; sweep < max_sweeps_ && !throttle_.stopped(); ++sweep) {
                if (advanced_list_ ? !best_granular_move(editor, current, l1, l2)
                                   : !best_relocate_between(editor, current, l1, l2))
                    break;
                current = editor.score();
            }
            if (applied_.empty()) return false;  // pair already locally optimal

            entities.clear();
            entities.reserve(applied_.size() * 2);
            for (const ListMove& move : applied_) {
                if (move.element1 >= 0) entities.push_back(move.element1);
                if (move.element2 >= 0 && move.element2 != move.element1)
                    entities.push_back(move.element2);
            }
            return true;
        }
    }

    void undo_last(OperatorContext<Solution>& ctx) override {
        if constexpr (Traits::shape == Shape::Assignment) {
            // Reverse order: each entry restores the value that entity held
            // before its own change, so replaying backwards unwinds a swap
            // correctly too.
            ctx.editor.begin_batch();
            for (auto it = assignments_.rbegin(); it != assignments_.rend(); ++it) {
                ctx.editor.assign(it->entity, it->previous_value);
            }
            ctx.editor.end_batch();
            assignments_.clear();
        } else {
            // Reverse order, so each operation sees the list state it created.
            for (auto it = applied_.rbegin(); it != applied_.rend(); ++it) {
                Moves::undo(ctx.editor, index_, *it);
            }
            applied_.clear();
        }
    }

private:
    using Moves = ListMoves<Solution>;

    // assignment: one variable write, with what it overwrote.
    struct Assignment {
        int entity;
        int previous_value;
    };

    // --- assignment ---

    // Picks an entity (through the entity hint when the use case supplies one)
    // plus a nearby partner, then repeatedly applies the single best improving
    // change-or-swap over the two until none is left or the sweep cap hits.
    bool apply_basic(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
                     std::vector<int>& entities) {
        if constexpr (Traits::shape != Shape::Assignment) {
            (void)ctx;
            (void)rng;
            (void)entities;
            return false;
        } else {
            Editor<Solution>& editor = ctx.editor;
            Solution& solution = editor.solution();
            const int entity_count = Traits::entity_count(solution);
            const int value_count = Traits::value_count(solution);
            if (entity_count < 1 || value_count < 2) return false;

            std::uniform_int_distribution<int> entity_pick(0, entity_count - 1);
            int e1 = entity_hint_ ? entity_hint_(rng) : -1;
            if (e1 < 0 || e1 >= entity_count) e1 = entity_pick(rng);
            e1 = movable_entity(solution, e1, entity_pick, rng);
            // Degenerate all-pinned case: movable_entity hands back the pinned
            // entity it was given rather than corrupting the assignment.
            if (entity_is_pinned<Traits>(solution, e1)) return false;

            // Partner: a nearby entity when nearby selection is configured
            // (classmates in timetabling, same-day shifts in rostering), else a
            // uniform draw. -1 = descend on e1 alone.
            int e2 = -1;
            if (entity_count >= 2) {
                if (nearby_.enabled() && !nearby_.of(e1).empty()) {
                    e2 = nearby_.of(e1)[rng() % nearby_.of(e1).size()];
                } else {
                    e2 = entity_pick(rng);
                }
                if (e2 == e1 || e2 < 0 || e2 >= entity_count ||
                    entity_is_pinned<Traits>(solution, e2)) {
                    e2 = -1;
                }
            }

            Score current = editor.score();
            for (int sweep = 0; sweep < max_sweeps_; ++sweep) {
                if (!best_change_or_swap_between(editor, current, e1, e2, value_count)) break;
                current = editor.score();
            }
            if (assignments_.empty()) return false;  // pair already locally optimal

            entities.clear();
            entities.reserve(assignments_.size());
            for (const Assignment& a : assignments_) entities.push_back(a.entity);
            return true;
        }
    }

    // One pass: try every value for e1, every value for e2, and the swap of the
    // two; keep the best strictly-improving one and apply it. Returns whether
    // anything was applied. e2 < 0 restricts the pass to e1.
    bool best_change_or_swap_between(Editor<Solution>& editor, const Score& current_score, int e1,
                                     int e2, int value_count) {
        Solution& solution = editor.solution();
        Score best = current_score;
        int best_entity = -1, best_value = -1;
        bool best_is_swap = false;

        const int old1 = Traits::value_of(solution, e1);
        const int old2 = e2 >= 0 ? Traits::value_of(solution, e2) : -1;

        for (int pick = 0; pick < (e2 >= 0 ? 2 : 1); ++pick) {
            const int e = pick == 0 ? e1 : e2;
            const int old = pick == 0 ? old1 : old2;
            for (int v = 0; v < value_count; ++v) {
                if (v == old) continue;
                editor.assign(e, v);
                const Score candidate = editor.score();
                if (candidate > best) {
                    best = candidate;
                    best_entity = e;
                    best_value = v;
                }
            }
            editor.assign(e, old);  // put it back
        }

        // The swap is evaluated last, so it only wins by beating every single
        // change above — which is what makes best_is_swap unambiguous.
        if (e2 >= 0 && old1 != old2) {
            editor.begin_batch();
            editor.assign(e1, old2);
            editor.assign(e2, old1);
            editor.end_batch();
            const Score candidate = editor.score();
            if (candidate > best) {
                best = candidate;
                best_is_swap = true;
            }
            editor.begin_batch();
            editor.assign(e1, old1);
            editor.assign(e2, old2);
            editor.end_batch();
        }

        if (best_is_swap) {
            editor.begin_batch();
            editor.assign(e1, old2);
            editor.assign(e2, old1);
            editor.end_batch();
            assignments_.push_back({e1, old1});
            assignments_.push_back({e2, old2});
            return true;
        }
        if (best_entity < 0) return false;
        editor.assign(best_entity, best_value);
        assignments_.push_back({best_entity, best_entity == e1 ? old1 : old2});
        return true;
    }

    // Score every relocation of an element of `from` into `to` and hand each
    // (score, p, q, element) to `visit`. `visit` must not read `from` itself:
    // on the slow path the element is out of the list while it runs. Each
    // slot is one evaluate_with_preview (a served preview is used as is, and
    // verified under ScoreChecks::Full); once the objective declines, the element
    // comes out of `from` once and every remaining slot costs one insert and
    // one remove. Trial insertions go straight through the editor: the
    // index is not consulted between the remove and the matching put back,
    // and keeping it in sync here would cost a reindex per candidate.
    template <typename Visit>
    void scan_relocations(Editor<Solution>& editor, int from, int to, Visit&& visit) {
        Solution& solution = editor.solution();
        const int from_size = Traits::list_size(solution, from);
        for (int p = 0; p < from_size && !throttle_.stopped(); ++p) {
            const int element = Traits::element_at(solution, from, p);
            const int to_size = Traits::list_size(solution, to);
            bool out = false;  // element taken out of `from` for the slow path
            for (int q = 0; q <= to_size && !throttle_.stopped(); ++q) {
                count_evaluation();
                if (!out) {
                    bool served = false;
                    const Score score = evaluate_with_preview(
                            editor,
                            [&](Objective<Solution>& c, Score& o) {
                                return Moves::preview(c, ListMove::segment(from, p, 1, to, q), o);
                            },
                            [&] {
                                editor.remove(from, p);
                                editor.insert(to, q, element);
                            },
                            [&] {
                                editor.remove(to, q);
                                editor.insert(from, p, element);
                            },
                            &served);
                    visit(score, p, q, element);
                    if (served) continue;
                    // Declined: the remaining slots take the slow path with
                    // the element out of `from` for all of them.
                    editor.remove(from, p);
                    out = true;
                    continue;
                }
                editor.insert(to, q, element);
                visit(editor.score(), p, q, element);
                editor.remove(to, q);
            }
            if (out) editor.insert(from, p, element);
        }
    }

    // One pass: try relocating every element of l1 into every position of l2
    // and vice versa, keep the best strictly-improving one, apply it.
    // Returns whether anything was applied.
    bool best_relocate_between(Editor<Solution>& editor, const Score& current_score, int l1,
                               int l2) {
        Score best = current_score;
        int best_from = -1, best_pos = -1, best_to = -1, best_insert = -1;

        for (int direction = 0; direction < 2; ++direction) {
            const int from = direction == 0 ? l1 : l2;
            const int to = direction == 0 ? l2 : l1;
            scan_relocations(editor, from, to,
                             [&](const Score& candidate, int p, int q, int /*element*/) {
                                 if (candidate > best) {
                                     best = candidate;
                                     best_from = from;
                                     best_pos = p;
                                     best_to = to;
                                     best_insert = q;
                                 }
                             });
        }

        if (best_from < 0) return false;
        const ListMove move =
                ListMove::segment(best_from, best_pos, 1, best_to, best_insert,
                                  Traits::element_at(editor.solution(), best_from, best_pos));
        Moves::apply(editor, index_, move);
        applied_.push_back(move);
        return true;
    }

    // Granular route-pair descent. The ordinary sequence move selector samples
    // one candidate and scores it. This pass enumerates the bounded
    // granular neighbourhood (Toth & Vigo 2003) and applies the single
    // best improving move. All trial moves are reverted immediately, so the
    // live element index remains coherent and undo_last() can restore the
    // accepted sequence atomically if the acceptor rejects it.
    bool best_granular_move(Editor<Solution>& editor, const Score& current_score, int l1, int l2) {
        Solution& solution = editor.solution();
        Score best = current_score;
        ListMove best_move;

        auto remember = [&](const Score& candidate, const ListMove& move) {
            if (candidate > best) {
                best = candidate;
                best_move = move;
            }
        };

        // Exchange(1,0): relocate an element between the two routes. This is
        // the existing descent neighbourhood and is still the most valuable
        // move for capacity balancing.
        for (int direction = 0; direction < 2; ++direction) {
            const int from = direction == 0 ? l1 : l2;
            const int to = direction == 0 ? l2 : l1;
            scan_relocations(editor, from, to,
                             [&](const Score& candidate, int p, int q, int element) {
                                 remember(candidate, ListMove::segment(from, p, 1, to, q, element));
                             });
        }

        // Exchange(1,1), both between routes and within each route. The
        // intra-route form is important for ordering and is the cheap
        // counterpart of the inter-route exchange.
        consider_swaps(editor, l1, l2, remember);

        // 2-opt/2-opt inside either route.
        for (int list : {l1, l2}) {
            if (list == unordered_list_) continue;  // order carries no score
            const int size = Traits::list_size(solution, list);
            for (int first = 0; first + 1 < size && !throttle_.stopped(); ++first) {
                for (int last = first + 1; last < size && !throttle_.stopped(); ++last) {
                    const ListMove move = ListMove::reverse(
                            list, first, last, Traits::element_at(solution, list, first),
                            Traits::element_at(solution, list, last));
                    count_evaluation();
                    remember(Moves::evaluate(editor, index_, move), move);
                }
            }
        }

        // 2-opt*: exchange route tails at every pair of cuts. Empty tails are
        // allowed, so this also covers a route-level relocate near a depot.
        const int size1 = Traits::list_size(solution, l1);
        const int size2 = Traits::list_size(solution, l2);
        for (int cut1 = 0; cut1 <= size1 && !throttle_.stopped(); ++cut1) {
            for (int cut2 = 0; cut2 <= size2 && !throttle_.stopped(); ++cut2) {
                if (cut1 == size1 && cut2 == size2) continue;
                const ListMove move = ListMove::tail_swap(
                        l1, cut1, l2, cut2,
                        cut1 < size1 ? Traits::element_at(solution, l1, cut1) : -1,
                        cut2 < size2 ? Traits::element_at(solution, l2, cut2) : -1);
                count_evaluation();
                remember(Moves::evaluate(editor, index_, move), move);
            }
        }

        // Small CROSS-exchange (ExchangeNM). A bounded segment length keeps
        // the scan granular; larger segments are left to ruin/recreate and
        // the sampled inter-exchange move.
        const int max_segment = std::max(1, list_max_segment_);
        for (int p1 = 0; p1 < size1 && !throttle_.stopped(); ++p1) {
            for (int len1 = 0; len1 <= max_segment && p1 + len1 <= size1; ++len1) {
                for (int p2 = 0; p2 < size2 && !throttle_.stopped(); ++p2) {
                    for (int len2 = 0; len2 <= max_segment && p2 + len2 <= size2; ++len2) {
                        if (len1 == 0 && len2 == 0) continue;
                        const ListMove move = ListMove::exchange(
                                l1, p1, len1, l2, p2, len2,
                                p1 < size1 ? Traits::element_at(solution, l1, p1) : -1,
                                p2 < size2 ? Traits::element_at(solution, l2, p2) : -1);
                        count_evaluation();
                        remember(Moves::evaluate(editor, index_, move), move);
                    }
                }
            }
        }

        // A scan cut short by the stop condition still applies what it found:
        // an improvement is an improvement, and the sweep loop stops after.
        if (!best_move.valid()) return false;
        Moves::apply(editor, index_, best_move);
        applied_.push_back(best_move);
        return true;
    }

    template <typename Remember>
    void consider_swaps(Editor<Solution>& editor, int l1, int l2, Remember&& remember) {
        Solution& solution = editor.solution();
        const int size1 = Traits::list_size(solution, l1);
        const int size2 = Traits::list_size(solution, l2);

        for (int p1 = 0; p1 < size1 && !throttle_.stopped(); ++p1) {
            for (int p2 = 0; p2 < size2 && !throttle_.stopped(); ++p2) {
                const int e1 = Traits::element_at(solution, l1, p1);
                const int e2 = Traits::element_at(solution, l2, p2);
                const ListMove move = ListMove::swap(l1, p1, l2, p2, e1, e2);
                count_evaluation();
                remember(Moves::evaluate(editor, index_, move), move);
            }
        }

        // Same-route Exchange11. It is deliberately kept separate from the
        // cross-route loop because ElementIndex::swap_elements handles both
        // positions correctly, but the pair enumeration must avoid p == q.
        for (int list : {l1, l2}) {
            if (list == unordered_list_) continue;  // order carries no score
            const int size = Traits::list_size(solution, list);
            for (int p1 = 0; p1 + 1 < size && !throttle_.stopped(); ++p1) {
                for (int p2 = p1 + 1; p2 < size && !throttle_.stopped(); ++p2) {
                    const int e1 = Traits::element_at(solution, list, p1);
                    const int e2 = Traits::element_at(solution, list, p2);
                    const ListMove move = ListMove::swap(list, p1, list, p2, e1, e2);
                    count_evaluation();
                    remember(Moves::evaluate(editor, index_, move), move);
                }
            }
        }
    }

    // One scan of a route pair is thousands of evaluations; on an objective
    // whose previews cost O(route) (a decomposed assembly, a plan with
    // waiting) that is long enough to overrun a short time limit, so the
    // stop condition is consulted through the throttle and the scan stops
    // where it is.
    void count_evaluation() { throttle_.reached(++evaluations_); }

    ElementIndex<Solution>& index_;
    const Neighborhoods& nearby_;
    int max_sweeps_;
    const std::function<int(std::mt19937_64&)>& entity_hint_;
    bool advanced_list_;
    int list_max_segment_;
    int list_candidate_routes_;
    int unordered_list_;
    StopThrottle throttle_;
    std::int64_t evaluations_ = 0;
    std::vector<ListMove> applied_;        // sequence
    std::vector<Assignment> assignments_;  // assignment
};

}  // namespace optimaru

#endif  // OPTIMARU_MOVES_DESCENT_HPP
