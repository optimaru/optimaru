// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_MOVES_EJECTION_CHAIN_HPP
#define OPTIMARU_MOVES_EJECTION_CHAIN_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/model/neighborhoods.hpp"
#include "optimaru/model/element_index.hpp"
#include "optimaru/moves/list_move.hpp"
#include "optimaru/stop/stop_throttle.hpp"
#include "optimaru/search/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Ejection chain — break a hard constraint on purpose, then repair it
// ---------------------------------------------------------------------------
// On a tightly packed instance the hard-feasible region is a set of islands.
// CVRPLIB X-n895-k37 loads its 37 vehicles to 99.8% with demands of 50-100:
// the last visit fits nowhere, and the only way to make room is to shift
// slack between routes through a chain of swaps, every one of which is
// uphill in soft score and none of which pays off until the last. Late
// acceptance never takes a hard step back, so the chain is unreachable one
// move at a time.
//
// This operator applies the whole chain as one move:
//
//  1. Seed. With a configured source list (the unassigned pool) that is not
//     empty, half the time take one of its elements and insert it at its
//     best position in any other list, even if that breaks a hard
//     constraint. Otherwise relocate a random element to the nearby
//     position with the best soft score among those that break a hard
//     constraint — the move the feasibility wall was hiding.
//     The repair may never put the seeded element back where it started:
//     that is a free full restoration and would turn the chain into a no-op.
//  2. Repair. While the hard score is below where it started, apply the best
//     strictly hard-improving relocate or swap out of the lists the chain last
//     touched, into the lists of each element's nearby neighbours. A swap
//     between an overloaded and a slack list moves exactly the slack the
//     single-move search could not reach.
//  3. If the hard score is back, the acceptor judges the net result. If the
//     chain stalls or hits its step cap, everything is undone and the move
//     reports itself as not applied. So does a chain that exhausts its score
//     evaluation budget: the step cap alone does not bound the cost, since
//     one step scans every element of up to two lists against every
//     candidate slot and swap partner.
//
// The repair never moves anything into the source list: "unassign something
// else" would be a lexicographic improvement at every step and would turn the
// chain into a trade that leaves the pool exactly as large as before.
//
// sequence shape only; apply() declines on assignment.
template <typename Solution>
class EjectionChain final : public MoveOperator<Solution> {
    using Traits = SolutionTraits<Solution>;

public:
    EjectionChain(ElementIndex<Solution>& index, const Neighborhoods& nearby,
                  int source_list, int max_steps, int candidate_lists, std::int64_t max_evaluations)
        : index_(index),
          nearby_(nearby),
          source_list_(source_list),
          max_steps_(std::max(1, max_steps)),
          candidate_lists_(std::max(1, candidate_lists)),
          max_evaluations_(std::max<std::int64_t>(1, max_evaluations)) {}

    const char* name() const override { return "ejectionChain"; }

    bool apply(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        applied_.clear();
        if constexpr (Traits::shape != Shape::Sequence) {
            (void)ctx;
            (void)rng;
            (void)entities;
            return false;
        } else {
            Editor<Solution>& editor = ctx.editor;
            const Solution& solution = editor.solution();
            if (Traits::list_count(solution) < 2 || !nearby_.enabled()) return false;

            const Score start = editor.score();
            evaluations_ = 0;
            // Half the chains start from the pool while it is non-empty: an
            // element that can never be placed (an impossible time window)
            // would otherwise sink every attempt into a failing repair and
            // starve the slack-shifting chains that start from routed
            // elements.
            const bool from_source =
                    has_source(solution) && std::uniform_int_distribution<int>(0, 1)(rng) == 0;
            const bool seeded = from_source ? seed_from_source(editor, rng)
                                           : seed_infeasible_relocate(editor, rng, start);
            if (!seeded) return false;
            seed_element_ = applied_.front().element1;
            seed_origin_ = applied_.front().l1;

            Score current = editor.score();
            // The repair asks the stop condition between steps once enough
            // evaluations have accumulated: on an objective whose previews
            // cost O(route) the evaluation budget alone is seconds.
            throttle_.reset(ctx.stop_requested);
            for (int step = 0; step < max_steps_ && current.hard < start.hard &&
                               evaluations_ < max_evaluations_ && !throttle_.reached(evaluations_);
                 ++step) {
                if (!best_repair(editor, current)) break;
                current = editor.score();
            }
            if (current.hard < start.hard) {
                undo_all(editor);
                return false;
            }

            entities.clear();
            for (const ListMove& op : applied_) {
                entities.push_back(op.element1);
                if (op.kind == ListMove::Kind::SWAP) entities.push_back(op.element2);
            }
            return true;
        }
    }

    void undo_last(OperatorContext<Solution>& ctx) override {
        if constexpr (Traits::shape != Shape::Sequence) {
            (void)ctx;
        } else {
            undo_all(ctx.editor);
        }
    }

private:
    using Moves = ListMoves<Solution>;

    struct Slot {
        int list;
        int position;
    };

    // A chain's operations are ListMoves of two kinds: a SEGMENT of length 1
    // (element1 moved from (l1, p1) to (l2, p2)) and a SWAP (element1 at
    // (l1, p1) and element2 at (l2, p2) traded places).

    bool has_source(const Solution& solution) const {
        return source_list_ >= 0 && source_list_ < Traits::list_count(solution) &&
               Traits::list_size(solution, source_list_) > 0;
    }

    void undo_all(Editor<Solution>& editor) {
        for (auto it = applied_.rbegin(); it != applied_.rend(); ++it) {
            Moves::undo(editor, index_, *it);
        }
        applied_.clear();
    }

    // Score of the solution with `op` applied (see ListMoves::evaluate).
    // Relocations always cross lists here, so `p2` indexes the target list
    // as it is now, which is also what the preview contract expects.
    Score evaluate(Editor<Solution>& editor, const ListMove& op) {
        return Moves::evaluate(editor, index_, op);
    }

    // Relocation targets for `e`: the slots beside its nearby neighbours in
    // other lists, plus each such list's end so a near-empty list is still
    // reachable. `from` and the source list are never targets.
    void collect_relocations(const Solution& solution, int e, int from) {
        relocations_.clear();
        lists_.clear();
        for (int nb : nearby_.of(e)) {
            const int list = index_.list_of(nb);
            if (list < 0 || list == from || list == source_list_) continue;
            const bool known = std::find(lists_.begin(), lists_.end(), list) != lists_.end();
            if (!known) {
                if (static_cast<int>(lists_.size()) >= candidate_lists_) continue;
                lists_.push_back(list);
            }
            add_slot({list, index_.position_of(nb)});
            add_slot({list, index_.position_of(nb) + 1});
        }
        for (int list : lists_) add_slot({list, Traits::list_size(solution, list)});
    }

    // Neighbours that sit next to each other in one list share a slot; each
    // slot is scored once so the evaluation budget goes to distinct moves.
    void add_slot(const Slot& slot) {
        for (const Slot& known : relocations_) {
            if (known.list == slot.list && known.position == slot.position) return;
        }
        relocations_.push_back(slot);
    }

    bool seed_from_source(Editor<Solution>& editor, std::mt19937_64& rng) {
        const Solution& solution = editor.solution();
        const int size = Traits::list_size(solution, source_list_);
        const int p1 = static_cast<int>(rng() % static_cast<std::uint64_t>(size));
        const int e = Traits::element_at(solution, source_list_, p1);

        bool found = false;
        ListMove best{};
        Score best_score;
        // Targets never include the source list, so their positions are the
        // same whether or not `e` has been taken out yet.
        collect_relocations(solution, e, source_list_);
        // A source element whose neighbours are all unassigned still needs a
        // landing spot: every list's end.
        if (relocations_.empty()) {
            for (int l = 0; l < Traits::list_count(solution); ++l) {
                if (l != source_list_) relocations_.push_back({l, Traits::list_size(solution, l)});
            }
        }
        for (const Slot& slot : relocations_) {
            if (slot.position > Traits::list_size(solution, slot.list)) continue;
            const ListMove op = ListMove::segment(source_list_, p1, 1, slot.list, slot.position, e);
            const Score score = evaluate(editor, op);
            ++evaluations_;
            if (!found || score > best_score) {
                found = true;
                best_score = score;
                best = op;
            }
        }
        if (!found) return false;
        Moves::apply(editor, index_, best);
        applied_.push_back(best);
        return true;
    }

    // The best-soft relocation of a random element among those that break a
    // hard constraint and would improve soft score — the move the
    // feasibility wall hides. Declines when there is none.
    bool seed_infeasible_relocate(Editor<Solution>& editor, std::mt19937_64& rng,
                                const Score& start) {
        const Solution& solution = editor.solution();
        const int element_count = Traits::element_count(solution);
        if (element_count < 2) return false;
        // A few draws to land on a routed element: with a large pool, one
        // draw that hits it would throw the whole step away.
        int e = -1;
        int from = -1;
        for (int draw = 0; draw < 8 && (from < 0 || from == source_list_); ++draw) {
            e = static_cast<int>(rng() % static_cast<std::uint64_t>(element_count));
            from = index_.list_of(e);
        }
        if (from < 0 || from == source_list_) return false;
        const int p1 = index_.position_of(e);

        bool found = false;
        ListMove best{};
        double best_soft = start.soft;
        collect_relocations(solution, e, from);
        for (const Slot& slot : relocations_) {
            if (slot.position > Traits::list_size(solution, slot.list)) continue;
            const ListMove op = ListMove::segment(from, p1, 1, slot.list, slot.position, e);
            const Score score = evaluate(editor, op);
            ++evaluations_;
            if (score.hard < start.hard && score.soft > best_soft) {
                found = true;
                best_soft = score.soft;
                best = op;
            }
        }
        if (!found) return false;
        Moves::apply(editor, index_, best);
        applied_.push_back(best);
        return true;
    }

    // One repair step: the best strictly hard-improving relocate or swap out
    // of the lists the previous operation touched.
    bool best_repair(Editor<Solution>& editor, const Score& current) {
        const Solution& solution = editor.solution();
        const ListMove& last = applied_.back();
        active_.clear();
        active_.push_back(last.l2);
        if (last.l1 != last.l2 && last.l1 != source_list_) active_.push_back(last.l1);

        bool found = false;
        ListMove best{};
        Score best_score = current;
        auto consider = [&](const ListMove& op) {
            if (evaluations_ >= max_evaluations_ || returns_seed(op)) return;
            // The repair never feeds the source list (see the header): every
            // target comes from collect_relocations / lists_, which skip it.
            assert(op.l2 != source_list_);
            ++evaluations_;
            const Score score = evaluate(editor, op);
            if (score.hard > current.hard && (!found || score > best_score)) {
                found = true;
                best_score = score;
                best = op;
            }
        };

        for (int a : active_) {
            for (int p1 = 0; p1 < Traits::list_size(solution, a); ++p1) {
                // Out of budget: the chain will be undone, stop scanning.
                if (evaluations_ >= max_evaluations_) break;
                const int e = Traits::element_at(solution, a, p1);
                collect_relocations(solution, e, a);
                // Relocation positions are measured with e still in place;
                // removing it first shifts nothing in another list.
                for (const Slot& slot : relocations_) {
                    consider(ListMove::segment(a, p1, 1, slot.list, slot.position, e));
                }
                // Swap partners: every element of the candidate lists while
                // that set is no larger than the nearby size, otherwise just
                // e's nearby neighbours on them. Either way one step stays
                // O(L x nearby); the full scan on 25-stop routes is what let
                // a chain burn its whole budget (CVRPLIB X-n895-k37), while
                // on 5-stop routes (X-n289-k60) the partner outside the
                // neighbourhood is the one that carries the slack.
                int partner_count = 0;
                for (int list : lists_) partner_count += Traits::list_size(solution, list);
                if (partner_count <= static_cast<int>(nearby_.of(e).size())) {
                    for (int list : lists_) {
                        for (int p2 = 0; p2 < Traits::list_size(solution, list); ++p2) {
                            if (evaluations_ >= max_evaluations_) break;
                            consider(ListMove::swap(a, p1, list, p2, e,
                                                    Traits::element_at(solution, list, p2)));
                        }
                    }
                } else {
                    for (int nb : nearby_.of(e)) {
                        if (evaluations_ >= max_evaluations_) break;
                        const int list = index_.list_of(nb);
                        if (std::find(lists_.begin(), lists_.end(), list) == lists_.end())
                            continue;
                        consider(ListMove::swap(a, p1, list, index_.position_of(nb), e, nb));
                    }
                }
            }
        }
        if (!found) return false;
        Moves::apply(editor, index_, best);
        applied_.push_back(best);
        return true;
    }

    // Would `op` put the seeded element back on the list it started from?
    // That restores the starting hard score exactly at no soft cost, so it
    // would outbid every partial repair and collapse the chain into a no-op
    // the acceptor then spends a step on.
    bool returns_seed(const ListMove& op) const {
        if (op.element1 == seed_element_ && op.l2 == seed_origin_) return true;
        return op.kind == ListMove::Kind::SWAP && op.element2 == seed_element_ &&
               op.l1 == seed_origin_;
    }

    ElementIndex<Solution>& index_;
    const Neighborhoods& nearby_;
    int source_list_;
    int max_steps_;
    int candidate_lists_;
    std::int64_t max_evaluations_;
    std::int64_t evaluations_ = 0;
    StopThrottle throttle_;
    int seed_element_ = -1;
    int seed_origin_ = -1;

    std::vector<ListMove> applied_;
    std::vector<Slot> relocations_;
    std::vector<int> lists_;
    std::vector<int> active_;
};

}  // namespace optimaru

#endif  // OPTIMARU_MOVES_EJECTION_CHAIN_HPP
