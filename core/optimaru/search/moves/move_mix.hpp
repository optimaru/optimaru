// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_MOVES_MOVE_MIX_HPP
#define OPTIMARU_SEARCH_MOVES_MOVE_MIX_HPP

#include <random>

#include "optimaru/model/editor.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/objective.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/search/moves/change.hpp"
#include "optimaru/search/moves/cross_exchange.hpp"
#include "optimaru/search/moves/element_swap.hpp"
#include "optimaru/search/moves/list_move.hpp"
#include "optimaru/search/moves/movable_entity.hpp"
#include "optimaru/search/moves/move_draw.hpp"
#include "optimaru/search/moves/or_opt.hpp"
#include "optimaru/search/moves/preview_evaluation.hpp"
#include "optimaru/search/moves/relocate.hpp"
#include "optimaru/search/moves/swap.hpp"
#include "optimaru/search/moves/two_opt.hpp"
#include "optimaru/search/moves/two_opt_star.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/utils/random.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// MoveMixSampler
// ---------------------------------------------------------------------------
// The ordinary moves of a solve, one random draw at a time, each in a file of
// its own:
//   assignment shape: change.hpp (give one entity another value) and swap.hpp
//                     (two entities exchange values);
//   sequence shape:   relocate.hpp (move one element), element_swap.hpp,
//                     two_opt.hpp (reverse a segment within one list),
//                     or_opt.hpp (move a 2-3 element segment), two_opt_star.hpp
//                     (tail swap) and cross_exchange.hpp; mixed by MoveMix
//                     weights (the last three default to 0).
// Moves follow a do/undo protocol: sample() scores one (previewed, or applied
// through the Editor), and the caller keeps it (apply_previewed) or rejects it
// (undo). The solver's step loop and the stagnation kick's polish both take
// their candidates from here.
//
// A new move of the mix is a file here with its draw, a weight in MoveMix and
// a branch in draw_assignment_move or draw_list_move below; a move that needs
// more than one draw at a time is a MoveOperator instead (operators.hpp).
template <typename Solution>
class MoveMixSampler {
    using Traits = SolutionTraits<Solution>;

public:
    // `index` is the solve's element index (sequence shape), which every
    // applied move keeps in sync.
    MoveMixSampler(const Settings& config, const Neighborhoods& nearby,
                   ElementIndex<Solution>& index)
        : draw_{config, nearby, index} {}

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
            move = draw_assignment_move(editor, rng);
            score = editor.score();
            return false;
        } else {
            move = draw_list_move(editor.solution(), rng);
            bool applied = false;
            score = evaluate_with_preview(
                    editor,
                    [&](Objective<Solution>& objective, Score& out) {
                        return ListMoves<Solution>::preview(objective, move.list, out);
                    },
                    [&] { ListMoves<Solution>::apply(editor, draw_.index, move.list); }, [] {},
                    /*served=*/nullptr, &applied);
            return !applied;  // the no-op undo left an applied move in place
        }
    }

    // Apply a move sample() only previewed.
    void apply_previewed(Editor<Solution>& editor, const SampledMove& move) {
        if constexpr (Traits::shape == Shape::Sequence) {
            ListMoves<Solution>::apply(editor, draw_.index, move.list);
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
            ListMoves<Solution>::undo(editor, draw_.index, move.list);
        }
    }

private:
    MoveDraw<Solution> draw_;

    // Pick a movable entity, then change or swap by MoveMix weights, and apply
    // the move.
    SampledMove draw_assignment_move(Editor<Solution>& editor, std::mt19937_64& rng) {
        Solution& solution = editor.solution();
        const SelectionSettings& select = draw_.config.selection;
        const MoveMix& ms = draw_.config.moves;
        const int entity_count = Traits::entity_count(solution);
        const int value_count = Traits::value_count(solution);
        std::uniform_int_distribution<int> entity_pick(0, entity_count - 1);
        std::uniform_int_distribution<int> value_pick(0, value_count - 1);

        int e = select.entity_hint ? select.entity_hint(rng) : -1;
        if (e < 0) e = entity_pick(rng);
        e = movable_entity(solution, e, entity_pick, rng);  // never move a pinned entity

        const double swap_weight = entity_count < 2 ? 0.0 : ms.swap_weight;
        const double total = ms.change_weight + swap_weight;

        SampledMove move;
        move.entities[0] = e;
        move.previous[0] = Traits::value_of(solution, e);
        if (draw_.unit(rng) * total < ms.change_weight || total <= 0) {
            apply_change(editor, draw_, value_pick, rng, move);
        } else {
            apply_swap(editor, draw_, entity_pick, rng, move);
        }
        return move;
    }

    // Pick an element, then one of the applicable sequence moves by MoveMix
    // weights, without touching the solution. During the search every element
    // is assigned, so the element index gives a uniformly random (list,
    // position) in O(1).
    SampledMove draw_list_move(const Solution& solution, std::mt19937_64& rng) {
        const MoveMix& ms = draw_.config.moves;
        const int list_count = Traits::list_count(solution);
        const int element_count = Traits::element_count(solution);

        const int e1 = random_index(rng, element_count);
        const int size1 = Traits::list_size(solution, draw_.index.list_of(e1));

        // Union move selector: weighted pick among the applicable moves.
        const double w_relocate = ms.relocate_weight;
        const double w_swap = ms.list_swap_weight;
        const double w_two_opt = size1 < 2 ? 0.0 : ms.two_opt_weight;
        const double w_or_opt = element_count < 3 ? 0.0 : ms.or_opt_weight;
        const double w_tail_swap = list_count < 2 ? 0.0 : ms.tail_swap_weight;
        const double w_inter_exchange = list_count < 2 ? 0.0 : ms.cross_exchange_weight;
        const double total =
                w_relocate + w_swap + w_two_opt + w_or_opt + w_tail_swap + w_inter_exchange;

        const double dice = draw_.unit(rng) * total;
        if (dice < w_relocate || element_count < 2 || total <= 0) {
            return sample_relocate(solution, draw_, rng, e1);
        }
        if (dice < w_relocate + w_swap) return sample_element_swap(solution, draw_, rng, e1);
        if (dice < w_relocate + w_swap + w_two_opt) return sample_two_opt(solution, draw_, rng, e1);
        if (dice < w_relocate + w_swap + w_two_opt + w_or_opt) {
            return sample_or_opt(solution, draw_, rng, e1);
        }
        if (dice < w_relocate + w_swap + w_two_opt + w_or_opt + w_tail_swap) {
            return sample_two_opt_star(solution, draw_, rng, e1);
        }
        return sample_cross_exchange(solution, draw_, rng, e1);
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_MOVES_MOVE_MIX_HPP
