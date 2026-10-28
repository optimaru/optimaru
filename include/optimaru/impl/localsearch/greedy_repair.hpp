#ifndef OPTIMARU_IMPL_LOCALSEARCH_GREEDY_REPAIR_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_GREEDY_REPAIR_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/config/localsearch/local_search_config.hpp"
#include "optimaru/impl/constructionheuristic/default_construction_heuristic_phase.hpp"
#include "optimaru/impl/heuristic/selector/common/nearby/nearby_neighborhoods.hpp"
#include "optimaru/impl/localsearch/element_index.hpp"
#include "optimaru/impl/localsearch/preview_evaluation.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// GreedyRepair
// ---------------------------------------------------------------------------
// The destroy-and-repair half of the local search, factored out of
// DefaultLocalSearchPhase so that the strategies under strategy/ can use it
// without reaching into the phase: the LNS kick, destroy-repair-as-a-move and
// route elimination all ruin a set and rebuild it the same way, and only the
// phase used to own the machinery.
//
// ruin() unassigns; recreate() puts everything back by cheapest insertion, or
// by regret-k when configured. All scratch buffers are members and reused, so
// a repair allocates nothing after the first.
template <typename Solution>
class GreedyRepair {
    using Descriptor = SolutionDescriptor<Solution>;
    using ConstructionHeuristic = DefaultConstructionHeuristicPhase<Solution>;

public:
    // `unassignedList` (LIST kind, -1 = none) is the list the repair never
    // places into: LocalSearchConfig::unassignedList.
    GreedyRepair(ElementIndex<Solution>& index, const NearbyNeighborhoods& nearby,
                 const RuinRecreateConfig& config, int unassignedList = -1)
        : index_(index), nearby_(nearby), config_(config), unassignedList_(unassignedList) {}

    // The list the repair is configured never to place into (-1 = none).
    int excludedList() const { return unassignedList_; }

    // How many things there are to ruin: entities for BASIC, elements for LIST.
    static int candidateCount(const Solution& solution) {
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            return Descriptor::entityCount(solution);
        } else {
            return Descriptor::elementCount(solution);
        }
    }

    // Order-preserving, range-checked dedup of a ruin set, plus (BASIC) the
    // removal of pinned entities.
    //
    // Order-preserving ON PURPOSE, and this is load-bearing: recreate()
    // shuffles the set it is handed, and a shuffle's result depends on its
    // input order, so sorting here would silently change the trajectory of
    // every built-in ruin. It matters structurally too — the SISR ruin returns
    // contiguous substrings and the route ruin returns whole routes, and
    // sorting by element id throws away exactly the grouping that makes those
    // operators what they are.
    void dedupe(ScoreDirector<Solution>& director, std::vector<int>& ruined, int n) {
        seen_.assign(static_cast<std::size_t>(n), 0);
        std::size_t kept = 0;
        for (int e : ruined) {
            if (e < 0 || e >= n || seen_[static_cast<std::size_t>(e)]) continue;
            seen_[static_cast<std::size_t>(e)] = 1;
            ruined[kept++] = e;
        }
        ruined.resize(kept);
        if constexpr (Descriptor::kind == VariableKind::BASIC) {  // never ruin a pinned entity
            ruined.erase(std::remove_if(ruined.begin(), ruined.end(),
                                        [&](int e) {
                                            return entityPinned<Descriptor>(
                                                    director.workingSolution(), e);
                                        }),
                         ruined.end());
        }
    }

    void ruin(ScoreDirector<Solution>& director, const std::vector<int>& ruined) {
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            director.beginVariableChanges();
            for (int e : ruined) {
                if (Descriptor::getVariable(director.workingSolution(), e) < 0) continue;
                director.changeVariable(e, -1);
            }
            director.endVariableChanges();
        } else {
            // Remember what came out of the excluded list: the exclusion means
            // "a repair never loses an assignment", not "an element that was
            // already unassigned must be forced onto a route whatever the
            // breach" — that would make every ruin touching the pool an
            // infeasible incumbent (as a kick) or a dead move (as a move)
            // whenever some element genuinely fits nowhere.
            // Sized once; only the entries flagged last time are cleared, so
            // a ruin costs O(ruined) rather than O(elements) here.
            fromExcluded_.resize(
                    static_cast<std::size_t>(Descriptor::elementCount(director.workingSolution())),
                    0);
            for (int e : flagged_) fromExcluded_[static_cast<std::size_t>(e)] = 0;
            flagged_.clear();
            for (int e : ruined) {
                if (index_.listOf(e) < 0) continue;  // defensive: already unassigned
                if (index_.listOf(e) == unassignedList_) {
                    fromExcluded_[static_cast<std::size_t>(e)] = 1;
                    flagged_.push_back(e);
                }
                index_.removeAt(director, index_.listOf(e), index_.positionOf(e));
            }
        }
    }

    // The list the configured exclusion forbids for element e: none when e was
    // taken out of that list by this very ruin.
    int excludedListFor(int e) const {
        if (e >= 0 && static_cast<std::size_t>(e) < fromExcluded_.size() &&
            fromExcluded_[static_cast<std::size_t>(e)]) {
            return -1;
        }
        return unassignedList_;
    }

    // Reassign everything in `ruined`. excludeList (LIST kind, -1 = none) is
    // off limits for every placement, which is how route elimination keeps the
    // list it just emptied from being refilled with what it just lost.
    void recreate(ScoreDirector<Solution>& director, std::vector<int> ruined,
                  std::mt19937_64& rng, int excludeList = -1) {
        Solution& solution = director.workingSolution();
        std::shuffle(ruined.begin(), ruined.end(), rng);
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            // BASIC repair stays plain best-value: with one variable per
            // entity there is no insertion position to regret, and the value
            // range is enumerated in full anyway.
            (void)excludeList;
            const int valueCount = Descriptor::valueCount(solution);
            for (int e : ruined) ConstructionHeuristic::placeBestValue(director, e, valueCount);
        } else {
            recreateList(director, std::move(ruined), rng, excludeList);
        }
    }

    // Place one element at its cheapest position. Used by route elimination,
    // which reinserts one at a time rather than as a regret-ordered batch.
    // `unassignedList` overrides the configured exclusion for this element
    // (recreate() lifts it for an element the ruin took out of that list).
    // Returns the list the element landed on.
    int insertCheapest(ScoreDirector<Solution>& director, int element, std::mt19937_64& rng,
                       int excludeList, int unassignedList) {
        RegretEntry entry;
        evaluatePlacements(director, element, 1, config_.blinkRate, rng, excludeList,
                           unassignedList, entry);
        if (!entry.evaluated) {
            // Nothing was scored, so bestList/bestPosition mean nothing. The
            // element still has to land somewhere — leaving it unassigned
            // would break the invariant every move and every score calculator
            // relies on — so append it to the first list it is allowed on.
            const Solution& solution = director.workingSolution();
            const int listCount = Descriptor::listCount(solution);
            for (int l = 0; l < listCount; ++l) {
                if (l == excludeList || l == unassignedList) continue;
                index_.insertAt(director, l, Descriptor::listSize(solution, l), element);
                return l;
            }
            // Nothing but the configured exclusion is left (one vehicle plus
            // the pool): the pool beats losing the element.
            for (int l = 0; l < listCount; ++l) {
                if (l == excludeList) continue;
                index_.insertAt(director, l, Descriptor::listSize(solution, l), element);
                return l;
            }
            return -1;  // one list, and it is the excluded one: caller's bug
        }
        index_.insertAt(director, entry.bestList, entry.bestPosition, element);
        return entry.bestList;
    }

private:
    // A candidate placement for a LIST element.
    struct Placement {
        int list;
        int position;
    };

    // One pending element's placement evaluation: where it wants to go, and
    // how much worse its k-th choice is (its regret).
    struct RegretEntry {
        HardSoftScore bestScore;
        int bestList = 0;
        int bestPosition = 0;
        // Regret is kept per score level so the HardSoft lexicographic order
        // survives: summing hard and soft into one number would let a large
        // soft regret outrank a hard-feasibility one.
        std::int64_t regretHard = 0;
        double regretSoft = 0.0;
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
    void collectCandidates(const Solution& solution, int e, int excludeList,
                           int unassignedList, std::vector<Placement>& out) {
        out.clear();
        const int listCount = Descriptor::listCount(solution);
        if (nearby_.enabled()) {
            for (int nb : nearby_.of(e)) {
                const int list = index_.listOf(nb);
                if (list < 0) continue;  // neighbour currently ruined
                if (list == excludeList || list == unassignedList) continue;
                out.push_back({list, index_.positionOf(nb)});
                out.push_back({list, index_.positionOf(nb) + 1});
            }
            for (int l = 0; l < listCount; ++l) {
                if (l == excludeList || l == unassignedList) continue;
                out.push_back({l, Descriptor::listSize(solution, l)});
            }
        } else {
            collectEveryPosition(solution, listCount, excludeList, unassignedList, out);
        }
    }

    static void collectEveryPosition(const Solution& solution, int listCount, int excludeList,
                                     int unassignedList, std::vector<Placement>& out) {
        for (int l = 0; l < listCount; ++l) {
            if (l == excludeList || l == unassignedList) continue;
            const int size = Descriptor::listSize(solution, l);
            for (int p = 0; p <= size; ++p) out.push_back({l, p});
        }
    }

    // Trial-inserts e at every candidate placement and records the best one
    // plus the regret against the k-th best. `blinkRate` is SISR's blinking:
    // each candidate after the first is skipped with that probability, so the
    // repair is not deterministically greedy and repeated kicks on the same
    // hole explore different rebuilds.
    void evaluatePlacements(ScoreDirector<Solution>& director, int e, int k, double blinkRate,
                            std::mt19937_64& rng, int excludeList, int unassignedList,
                            RegretEntry& entry) {
        Solution& solution = director.workingSolution();
        collectCandidates(solution, e, excludeList, unassignedList, candidateScratch_);
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        topScores_.clear();  // k best scores, descending; k is small
        entry.evaluated = false;
        for (const Placement& c : candidateScratch_) {
            if (c.position > Descriptor::listSize(solution, c.list)) continue;
            if (entry.evaluated && blinkRate > 0.0 && unit(rng) < blinkRate) continue;
            const HardSoftScore score = evaluateWithPreview(
                    director,
                    [&](IncrementalScoreCalculator<Solution>& calc, HardSoftScore& out) {
                        return calc.previewInsert(c.list, c.position, e, out);
                    },
                    [&] { director.insertListElement(c.list, c.position, e); },
                    [&] { director.removeListElement(c.list, c.position); });

            if (!entry.evaluated || score > entry.bestScore) {
                entry.bestScore = score;
                entry.bestList = c.list;
                entry.bestPosition = c.position;
                entry.evaluated = true;
            }
            auto at = std::find_if(topScores_.begin(), topScores_.end(),
                                   [&](const HardSoftScore& s) { return score > s; });
            if (at != topScores_.end() || static_cast<int>(topScores_.size()) < k) {
                topScores_.insert(at, score);
                if (static_cast<int>(topScores_.size()) > k) topScores_.pop_back();
            }
        }

        if (static_cast<int>(topScores_.size()) < k) {
            // Fewer distinct options than k: this element is the constrained
            // one — place it before anything else narrows its choices further.
            entry.regretHard = std::numeric_limits<std::int64_t>::max();
            entry.regretSoft = std::numeric_limits<double>::infinity();
            return;
        }
        entry.regretHard = 0;
        entry.regretSoft = 0.0;
        for (std::size_t j = 1; j < topScores_.size(); ++j) {
            entry.regretHard += topScores_[0].hard - topScores_[j].hard;
            entry.regretSoft += topScores_[0].soft - topScores_[j].soft;
        }
    }

    // Regret ordering: place the element that loses the most by waiting.
    static bool moreUrgent(const RegretEntry& a, const RegretEntry& b) {
        if (a.regretHard != b.regretHard) return a.regretHard > b.regretHard;
        if (a.regretSoft != b.regretSoft) return a.regretSoft > b.regretSoft;
        return a.bestScore > b.bestScore;
    }

    // LIST repair. regretK < 2 is plain cheapest insertion in random order;
    // otherwise regret-k: repeatedly place the element with the widest gap
    // between its best and k-th best placement.
    //
    // The evaluations are cached across rounds. Inserting into list L only
    // changes the insertion costs inside L, so only the pending elements whose
    // best placement was in L need re-evaluating — that turns the naive
    // O(pending^2) regret sweep into roughly one full pass plus a few
    // per-round refreshes.
    void recreateList(ScoreDirector<Solution>& director, std::vector<int> ruined,
                      std::mt19937_64& rng, int excludeList) {
        const int k = config_.regretK;
        if (k < 2) {
            for (int e : ruined) {
                if (insertCheapest(director, e, rng, excludeList, excludedListFor(e)) < 0) {
                    throw std::logic_error("GreedyRepair: no list may take a ruined element");
                }
            }
            return;
        }

        const std::size_t pending = ruined.size();
        regretEntries_.assign(pending, RegretEntry{});
        placed_.assign(pending, 0);
        for (std::size_t round = 0; round < pending; ++round) {
            std::size_t choice = pending;
            for (std::size_t i = 0; i < pending; ++i) {
                if (placed_[i]) continue;
                if (!regretEntries_[i].evaluated) {
                    evaluatePlacements(director, ruined[i], k, config_.blinkRate, rng,
                                       excludeList, excludedListFor(ruined[i]),
                                       regretEntries_[i]);
                }
                if (choice == pending || moreUrgent(regretEntries_[i], regretEntries_[choice])) {
                    choice = i;
                }
            }
            // Every element has at least one admissible slot (some list is
            // neither excluded nor the configured exclusion, or the element
            // may return to the latter), so the winner was scored. Should a
            // caller ever exclude the last list, insertCheapest's fallback
            // still lands the element somewhere rather than at a stale
            // (bestList, bestPosition).
            int winnerList;
            if (regretEntries_[choice].evaluated) {
                winnerList = regretEntries_[choice].bestList;
                index_.insertAt(director, winnerList, regretEntries_[choice].bestPosition,
                                ruined[choice]);
            } else {
                winnerList = insertCheapest(director, ruined[choice], rng, excludeList,
                                            excludedListFor(ruined[choice]));
                if (winnerList < 0) {
                    // Every list excluded: a caller's configuration error, and
                    // silently losing the element would corrupt the solution.
                    throw std::logic_error("GreedyRepair: no list may take a ruined element");
                }
            }
            placed_[choice] = 1;
            for (std::size_t i = 0; i < pending; ++i) {
                if (!placed_[i] && regretEntries_[i].bestList == winnerList) {
                    regretEntries_[i].evaluated = false;
                }
            }
        }
    }

    ElementIndex<Solution>& index_;
    const NearbyNeighborhoods& nearby_;
    const RuinRecreateConfig& config_;
    int unassignedList_;

    // Scratch, reused across repairs so a repair allocates nothing.
    std::vector<Placement> candidateScratch_;
    std::vector<HardSoftScore> topScores_;
    std::vector<RegretEntry> regretEntries_;
    std::vector<char> placed_;
    std::vector<char> seen_;
    std::vector<char> fromExcluded_;
    std::vector<int> flagged_;  // elements set in fromExcluded_ by the last ruin
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_GREEDY_REPAIR_HPP
