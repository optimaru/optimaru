// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_GREEDY_REPAIR_HPP
#define OPTIMARU_SEARCH_GREEDY_REPAIR_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "optimaru/solution_traits.hpp"
#include "optimaru/editor.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/search/construction.hpp"
#include "optimaru/search/neighborhoods.hpp"
#include "optimaru/search/element_index.hpp"
#include "optimaru/search/preview_evaluation.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// GreedyRepair
// ---------------------------------------------------------------------------
// The destroy-and-repair half of the local search, factored out of
// LocalSearch so that the strategies under strategy/ can use it
// without reaching into local search: the LNS kick, destroy-repair-as-a-move and
// route elimination all ruin a set and rebuild it the same way, and only the
// local search used to own the machinery.
//
// ruin() unassigns; recreate() puts everything back by cheapest insertion, or
// by regret-k when configured. All scratch buffers are members and reused, so
// a repair allocates nothing after the first.
template <typename Solution>
class GreedyRepair {
    using Traits = SolutionTraits<Solution>;
    using ConstructionHeuristic = Construction<Solution>;

public:
    // `unassigned_list` (sequence shape, -1 = none) is the list the repair never
    // places into: SearchSettings::unassigned_list.
    GreedyRepair(ElementIndex<Solution>& index, const Neighborhoods& nearby,
                 const RuinRecreateSettings& config, int unassigned_list = -1)
        : index_(index), nearby_(nearby), config_(config), unassigned_list_(unassigned_list) {}

    // The list the repair is configured never to place into (-1 = none).
    int excluded_list() const { return unassigned_list_; }

    // How many things there are to ruin: entities for assignment, elements for sequence.
    static int candidate_count(const Solution& solution) {
        if constexpr (Traits::shape == Shape::Assignment) {
            return Traits::entity_count(solution);
        } else {
            return Traits::element_count(solution);
        }
    }

    // Order-preserving, range-checked dedup of a ruin set, plus (assignment) the
    // removal of pinned entities.
    //
    // Order-preserving ON PURPOSE, and this is load-bearing: recreate()
    // shuffles the set it is handed, and a shuffle's result depends on its
    // input order, so sorting here would silently change the trajectory of
    // every built-in ruin. It matters structurally too — the SISR ruin returns
    // contiguous substrings and the route ruin returns whole routes, and
    // sorting by element id throws away exactly the grouping that makes those
    // operators what they are.
    void dedupe(Editor<Solution>& editor, std::vector<int>& ruined, int n) {
        seen_.assign(static_cast<std::size_t>(n), 0);
        std::size_t kept = 0;
        for (int e : ruined) {
            if (e < 0 || e >= n || seen_[static_cast<std::size_t>(e)]) continue;
            seen_[static_cast<std::size_t>(e)] = 1;
            ruined[kept++] = e;
        }
        ruined.resize(kept);
        if constexpr (Traits::shape == Shape::Assignment) {  // never ruin a pinned entity
            ruined.erase(std::remove_if(ruined.begin(), ruined.end(),
                                        [&](int e) {
                                            return entity_is_pinned<Traits>(
                                                    editor.solution(), e);
                                        }),
                         ruined.end());
        }
    }

    void ruin(Editor<Solution>& editor, const std::vector<int>& ruined) {
        if constexpr (Traits::shape == Shape::Assignment) {
            editor.begin_batch();
            for (int e : ruined) {
                if (Traits::value_of(editor.solution(), e) < 0) continue;
                editor.assign(e, -1);
            }
            editor.end_batch();
        } else {
            // Remember what came out of the excluded list: the exclusion means
            // "a repair never loses an assignment", not "an element that was
            // already unassigned must be forced onto a route whatever the
            // breach" — that would make every ruin touching the pool an
            // infeasible incumbent (as a kick) or a dead move (as a move)
            // whenever some element genuinely fits nowhere.
            // Sized once; only the entries flagged last time are cleared, so
            // a ruin costs O(ruined) rather than O(elements) here.
            from_excluded_.resize(
                    static_cast<std::size_t>(Traits::element_count(editor.solution())),
                    0);
            for (int e : flagged_) from_excluded_[static_cast<std::size_t>(e)] = 0;
            flagged_.clear();
            for (int e : ruined) {
                if (index_.list_of(e) < 0) continue;  // defensive: already unassigned
                if (index_.list_of(e) == unassigned_list_) {
                    from_excluded_[static_cast<std::size_t>(e)] = 1;
                    flagged_.push_back(e);
                }
                index_.remove_at(editor, index_.list_of(e), index_.position_of(e));
            }
        }
    }

    // The list the configured exclusion forbids for element e: none when e was
    // taken out of that list by this very ruin.
    int excluded_list_for(int e) const {
        if (e >= 0 && static_cast<std::size_t>(e) < from_excluded_.size() &&
            from_excluded_[static_cast<std::size_t>(e)]) {
            return -1;
        }
        return unassigned_list_;
    }

    // Reassign everything in `ruined`. exclude_list (sequence shape, -1 = none) is
    // off limits for every placement, which is how route elimination keeps the
    // list it just emptied from being refilled with what it just lost.
    void recreate(Editor<Solution>& editor, std::vector<int> ruined,
                  std::mt19937_64& rng, int exclude_list = -1) {
        Solution& solution = editor.solution();
        std::shuffle(ruined.begin(), ruined.end(), rng);
        if constexpr (Traits::shape == Shape::Assignment) {
            // assignment repair stays plain best-value: with one variable per
            // entity there is no insertion position to regret, and the value
            // range is enumerated in full anyway.
            (void)exclude_list;
            const int value_count = Traits::value_count(solution);
            for (int e : ruined) ConstructionHeuristic::place_best_value(editor, e, value_count);
        } else {
            recreate_list(editor, std::move(ruined), rng, exclude_list);
        }
    }

    // Place one element at its cheapest position. Used by route elimination,
    // which reinserts one at a time rather than as a regret-ordered batch.
    // `unassigned_list` overrides the configured exclusion for this element
    // (recreate() lifts it for an element the ruin took out of that list).
    // Returns the list the element landed on.
    int insert_cheapest(Editor<Solution>& editor, int element, std::mt19937_64& rng,
                       int exclude_list, int unassigned_list) {
        RegretEntry entry;
        evaluate_placements(editor, element, 1, config_.blink_rate, rng, exclude_list,
                           unassigned_list, entry);
        if (!entry.evaluated) {
            // Nothing was scored, so best_list/best_position mean nothing. The
            // element still has to land somewhere — leaving it unassigned
            // would break the invariant every move and every objective
            // relies on — so append it to the first list it is allowed on.
            const Solution& solution = editor.solution();
            const int list_count = Traits::list_count(solution);
            for (int l = 0; l < list_count; ++l) {
                if (l == exclude_list || l == unassigned_list) continue;
                index_.insert_at(editor, l, Traits::list_size(solution, l), element);
                return l;
            }
            // Nothing but the configured exclusion is left (one vehicle plus
            // the pool): the pool beats losing the element.
            for (int l = 0; l < list_count; ++l) {
                if (l == exclude_list) continue;
                index_.insert_at(editor, l, Traits::list_size(solution, l), element);
                return l;
            }
            return -1;  // one list, and it is the excluded one: caller's bug
        }
        index_.insert_at(editor, entry.best_list, entry.best_position, element);
        return entry.best_list;
    }

private:
    // A candidate placement for a sequence element.
    struct Placement {
        int list;
        int position;
    };

    // One pending element's placement evaluation: where it wants to go, and
    // how much worse its k-th choice is (its regret).
    struct RegretEntry {
        Score best_score;
        int best_list = 0;
        int best_position = 0;
        // Regret is kept per score level so the hard/soft lexicographic order
        // survives: summing hard and soft into one number would let a large
        // soft regret outrank a hard-feasibility one.
        std::int64_t regret_hard = 0;
        double regret_soft = 0.0;
        bool evaluated = false;
    };

    // Which placements are worth trying for e: with a distance meter, only the
    // slots adjacent to its assigned nearby neighbours (plus every list's end,
    // so empty lists stay reachable) — O(nearbySize) trial insertions instead
    // of O(total positions). Without one, every position.
    //
    // A KNOWN WEAKNESS, AND WHY THE OBVIOUS FIX IS NOT HERE. When e's nearby
    // neighbours are all currently ruined, the loop below skips every one of
    // them and the candidate set degenerates to "the end of each list" — which
    // is the slot just before the return leg, about the worst on offer. That is
    // not a rare edge case: a route's members are each other's nearest
    // neighbours, so emptying a route ruins a whole neighbourhood at once, and
    // the elements placed first (the ones that set up everything after them)
    // are exactly the ones that hit it. It affects the route ruin, SISR and
    // route elimination alike.
    //
    // Enumerating every position when no anchor survives was tried, and it
    // loses on both use cases that exercise it: vehicle-routing RC2_10_5 goes
    // +8.1% -> +11.4% (35 -> 36 vehicles), and pickup-delivery's pair-aware
    // operators drop from -120/-170/-150 medium to -230/-190/-210 on the tight
    // instance. The reason is cost, not direction — it turns an O(nearbySize)
    // repair into an O(elements) one precisely when the repair is busiest, and
    // the kicks it buys back are worth more than the placements it improves.
    // A cheaper anchor (a bounded sample of positions, or remembering where the
    // ruined neighbours sat before they were removed) is the open question;
    // enumerating everything is not the answer.
    void collect_candidates(const Solution& solution, int e, int exclude_list,
                           int unassigned_list, std::vector<Placement>& out) {
        out.clear();
        const int list_count = Traits::list_count(solution);
        if (nearby_.enabled()) {
            for (int nb : nearby_.of(e)) {
                const int list = index_.list_of(nb);
                if (list < 0) continue;  // neighbour currently ruined
                if (list == exclude_list || list == unassigned_list) continue;
                out.push_back({list, index_.position_of(nb)});
                out.push_back({list, index_.position_of(nb) + 1});
            }
            for (int l = 0; l < list_count; ++l) {
                if (l == exclude_list || l == unassigned_list) continue;
                out.push_back({l, Traits::list_size(solution, l)});
            }
        } else {
            collect_every_position(solution, list_count, exclude_list, unassigned_list, out);
        }
    }

    static void collect_every_position(const Solution& solution, int list_count, int exclude_list,
                                     int unassigned_list, std::vector<Placement>& out) {
        for (int l = 0; l < list_count; ++l) {
            if (l == exclude_list || l == unassigned_list) continue;
            const int size = Traits::list_size(solution, l);
            for (int p = 0; p <= size; ++p) out.push_back({l, p});
        }
    }

    // Trial-inserts e at every candidate placement and records the best one
    // plus the regret against the k-th best. `blink_rate` is SISR's blinking:
    // each candidate after the first is skipped with that probability, so the
    // repair is not deterministically greedy and repeated kicks on the same
    // hole explore different rebuilds.
    void evaluate_placements(Editor<Solution>& editor, int e, int k, double blink_rate,
                            std::mt19937_64& rng, int exclude_list, int unassigned_list,
                            RegretEntry& entry) {
        Solution& solution = editor.solution();
        collect_candidates(solution, e, exclude_list, unassigned_list, candidate_scratch_);
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        top_scores_.clear();  // k best scores, descending; k is small
        entry.evaluated = false;
        for (const Placement& c : candidate_scratch_) {
            if (c.position > Traits::list_size(solution, c.list)) continue;
            if (entry.evaluated && blink_rate > 0.0 && unit(rng) < blink_rate) continue;
            const Score score = evaluate_with_preview(
                    editor,
                    [&](Objective<Solution>& calc, Score& out) {
                        return calc.preview_insert(c.list, c.position, e, out);
                    },
                    [&] { editor.insert(c.list, c.position, e); },
                    [&] { editor.remove(c.list, c.position); });

            if (!entry.evaluated || score > entry.best_score) {
                entry.best_score = score;
                entry.best_list = c.list;
                entry.best_position = c.position;
                entry.evaluated = true;
            }
            auto at = std::find_if(top_scores_.begin(), top_scores_.end(),
                                   [&](const Score& s) { return score > s; });
            if (at != top_scores_.end() || static_cast<int>(top_scores_.size()) < k) {
                top_scores_.insert(at, score);
                if (static_cast<int>(top_scores_.size()) > k) top_scores_.pop_back();
            }
        }

        if (static_cast<int>(top_scores_.size()) < k) {
            // Fewer distinct options than k: this element is the constrained
            // one — place it before anything else narrows its choices further.
            entry.regret_hard = std::numeric_limits<std::int64_t>::max();
            entry.regret_soft = std::numeric_limits<double>::infinity();
            return;
        }
        entry.regret_hard = 0;
        entry.regret_soft = 0.0;
        for (std::size_t j = 1; j < top_scores_.size(); ++j) {
            entry.regret_hard += top_scores_[0].hard - top_scores_[j].hard;
            entry.regret_soft += top_scores_[0].soft - top_scores_[j].soft;
        }
    }

    // Regret ordering: place the element that loses the most by waiting.
    static bool more_urgent(const RegretEntry& a, const RegretEntry& b) {
        if (a.regret_hard != b.regret_hard) return a.regret_hard > b.regret_hard;
        if (a.regret_soft != b.regret_soft) return a.regret_soft > b.regret_soft;
        return a.best_score > b.best_score;
    }

    // sequence repair. regret_k < 2 is plain cheapest insertion in random order;
    // otherwise regret-k: repeatedly place the element with the widest gap
    // between its best and k-th best placement.
    //
    // The evaluations are cached across rounds. Inserting into list L only
    // changes the insertion costs inside L, so only the pending elements whose
    // best placement was in L need re-evaluating — that turns the naive
    // O(pending^2) regret sweep into roughly one full pass plus a few
    // per-round refreshes.
    void recreate_list(Editor<Solution>& editor, std::vector<int> ruined,
                      std::mt19937_64& rng, int exclude_list) {
        const int k = config_.regret_k;
        if (k < 2) {
            for (int e : ruined) {
                if (insert_cheapest(editor, e, rng, exclude_list, excluded_list_for(e)) < 0) {
                    throw std::logic_error("GreedyRepair: no list may take a ruined element");
                }
            }
            return;
        }

        const std::size_t pending = ruined.size();
        regret_entries_.assign(pending, RegretEntry{});
        placed_.assign(pending, 0);
        for (std::size_t round = 0; round < pending; ++round) {
            std::size_t choice = pending;
            for (std::size_t i = 0; i < pending; ++i) {
                if (placed_[i]) continue;
                if (!regret_entries_[i].evaluated) {
                    evaluate_placements(editor, ruined[i], k, config_.blink_rate, rng,
                                       exclude_list, excluded_list_for(ruined[i]),
                                       regret_entries_[i]);
                }
                if (choice == pending || more_urgent(regret_entries_[i], regret_entries_[choice])) {
                    choice = i;
                }
            }
            // Every element has at least one admissible slot (some list is
            // neither excluded nor the configured exclusion, or the element
            // may return to the latter), so the winner was scored. Should a
            // caller ever exclude the last list, insert_cheapest's fallback
            // still lands the element somewhere rather than at a stale
            // (best_list, best_position).
            int winner_list;
            if (regret_entries_[choice].evaluated) {
                winner_list = regret_entries_[choice].best_list;
                index_.insert_at(editor, winner_list, regret_entries_[choice].best_position,
                                ruined[choice]);
            } else {
                winner_list = insert_cheapest(editor, ruined[choice], rng, exclude_list,
                                            excluded_list_for(ruined[choice]));
                if (winner_list < 0) {
                    // Every list excluded: a caller's configuration error, and
                    // silently losing the element would corrupt the solution.
                    throw std::logic_error("GreedyRepair: no list may take a ruined element");
                }
            }
            placed_[choice] = 1;
            for (std::size_t i = 0; i < pending; ++i) {
                if (!placed_[i] && regret_entries_[i].best_list == winner_list) {
                    regret_entries_[i].evaluated = false;
                }
            }
        }
    }

    ElementIndex<Solution>& index_;
    const Neighborhoods& nearby_;
    const RuinRecreateSettings& config_;
    int unassigned_list_;

    // Scratch, reused across repairs so a repair allocates nothing.
    std::vector<Placement> candidate_scratch_;
    std::vector<Score> top_scores_;
    std::vector<RegretEntry> regret_entries_;
    std::vector<char> placed_;
    std::vector<char> seen_;
    std::vector<char> from_excluded_;
    std::vector<int> flagged_;  // elements set in from_excluded_ by the last ruin
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_GREEDY_REPAIR_HPP
