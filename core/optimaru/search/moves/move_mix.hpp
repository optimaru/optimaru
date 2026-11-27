// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_MOVE_MIX_HPP
#define OPTIMARU_SEARCH_MOVES_MOVE_MIX_HPP

#include <algorithm>
#include <random>
#include <vector>

#include "optimaru/model/editor.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/model/solution_traits.hpp"
#include "optimaru/portable_random.hpp"
#include "optimaru/score/objective.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/search/moves/movable_entity.hpp"
#include "optimaru/search/moves/preview_evaluation.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// One move of the mix, with what is needed to reject it and to feed the
// acceptor's tabu memory.
struct SampledMove {
    int entities[2] = {0, 0};  // the entities / elements the move is about
    int entity_count = 0;
    int previous[2] = {0, 0};  // assignment: the value each entity held before
    ListMove list;             // sequence: the move itself
};

// ---------------------------------------------------------------------------
// MoveMixSampler
// ---------------------------------------------------------------------------
// The ordinary moves of a solve, one random draw at a time:
//   assignment shape: change (give one entity another value) and swap (two
//                     entities exchange values);
//   sequence shape:   relocate (move one element), element swap, 2-opt
//                     (reverse a segment within one list), Or-opt (move a
//                     2-3 element segment), 2-opt* (tail swap) and
//                     CROSS-exchange; mixed by MoveMix weights (the last three
//                     default to 0).
// Moves follow a do/undo protocol: sample() scores one (previewed, or applied
// through the Editor), and the caller keeps it (apply_previewed) or rejects it
// (undo). The solver's step loop and the stagnation kick's polish both take
// their candidates from here.
template <typename Solution>
class MoveMixSampler {
    using Traits = SolutionTraits<Solution>;

public:
    // `index` is the solve's element index (sequence shape), which every
    // applied move keeps in sync.
    MoveMixSampler(const Settings& config, const Neighborhoods& nearby,
                   ElementIndex<Solution>& index)
        : config_(config), nearby_(nearby), index_(index) {}

    // Draw one move of the mix and score it. Returns true
    // when the move is NOT applied on return: its preview was served and
    // `score` is that preview, so the caller applies it (apply_previewed) only
    // if it keeps it. Returns false when the move IS applied on return and
    // `score` is the applied score; the caller undoes a rejection (undo).
    //
    // An assignment move is always applied. A sequence move is applied when
    // the objective declines its preview, or when ScoreChecks::Full applies it
    // to check the preview against the applied score (ScoreMismatchError on a
    // mismatch): one evaluate_with_preview with a no-op undo, so every preview
    // in the engine is verified by the same lines.
    bool sample(Editor<Solution>& editor, std::mt19937_64& rng, SampledMove& move, Score& score) {
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

    // Apply a move sample() only previewed.
    void apply_previewed(Editor<Solution>& editor, const SampledMove& move) {
        if constexpr (Traits::shape == Shape::Sequence) {
            ListMoves<Solution>::apply(editor, index_, move.list);
        } else {
            (void)editor;
            (void)move;
        }
    }

    // Revert a move sample() left applied.
    void undo(Editor<Solution>& editor, const SampledMove& move) {
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

private:
    const Settings& config_;
    const Neighborhoods& nearby_;
    ElementIndex<Solution>& index_;
    std::uniform_real_distribution<double> unit_{0.0, 1.0};

    // A random nearby neighbour of `e`, drawn with NeighborSettings::probability
    // when nearby selection is configured; -1 means draw uniformly instead.
    int nearby_pick(int e, std::mt19937_64& rng) {
        if (!nearby_.enabled() || !(unit_(rng) < config_.selection.nearby.probability)) return -1;
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

    SampledMove apply_basic_move(Editor<Solution>& editor, std::mt19937_64& rng) {
        Solution& solution = editor.solution();
        const SelectionSettings& select = config_.selection;
        const MoveMix& ms = config_.moves;
        const int entity_count = Traits::entity_count(solution);
        const int value_count = Traits::value_count(solution);
        std::uniform_int_distribution<int> entity_pick(0, entity_count - 1);
        std::uniform_int_distribution<int> value_pick(0, value_count - 1);

        int e = select.entity_hint ? select.entity_hint(rng) : -1;
        if (e < 0) e = entity_pick(rng);
        e = movable_entity(solution, e, entity_pick, rng);  // never move a pinned entity
        const int old_value = Traits::value_of(solution, e);

        const double swap_weight = entity_count < 2 ? 0.0 : ms.swap_weight;
        const double total = ms.change_weight + swap_weight;

        SampledMove move;
        move.entities[0] = e;
        move.previous[0] = old_value;
        if (unit_(rng) * total < ms.change_weight || total <= 0) {
            // Change: another value for e (never its current one, when there
            // is a choice).
            int v = select.value_hint ? select.value_hint(rng, e) : -1;
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

    // Draw one sequence move without touching the solution. Every coordinate is
    // in the CURRENT solution, with the one convention the objective previews
    // share: a relocate / or-opt insertion index (`d`) indexes the destination
    // list AFTER the moved run has been removed, which only differs from the
    // pre-move index when source and destination are the same list.
    SampledMove sample_list_move(Editor<Solution>& editor, std::mt19937_64& rng) {
        Solution& solution = editor.solution();
        const MoveMix& ms = config_.moves;
        const int list_count = Traits::list_count(solution);
        const int element_count = Traits::element_count(solution);

        // During the search every element is assigned, so the element index
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
        SampledMove move;
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
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_MOVE_MIX_HPP
