#ifndef OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_DESCENT_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_DESCENT_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/impl/heuristic/selector/common/nearby/nearby_neighborhoods.hpp"
#include "optimaru/impl/localsearch/element_index.hpp"
#include "optimaru/impl/localsearch/movable_entity.hpp"
#include "optimaru/impl/localsearch/list_move.hpp"
#include "optimaru/impl/localsearch/preview_evaluation.hpp"
#include "optimaru/impl/localsearch/termination_throttle.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Descent — best-improvement over one nearby pair
// ---------------------------------------------------------------------------
// The engine's ordinary step samples ONE random move and asks the acceptor.
// That is enough while the instance is small enough for a random draw to land
// on an improvement reasonably often; at a thousand elements it is not, and no
// amount of extra move TYPES fixes it — adding CROSS-exchange to the random
// mix measurably did nothing (see the vehicle-routing BENCHMARK). What changed
// the numbers was searching a neighbourhood exhaustively instead of sampling
// it.
//
// So: pick a nearby pair, then repeatedly apply the single best improving move
// over it until the pair is locally optimal or the sweep cap hits. This is the
// difference between "is this one random move an improvement?" and "what is
// the best move available here?". The pair is per kind:
//
//  - LIST:  two geometrically nearby lists; every relocation between them, in
//           both directions. An opt-in granular mode also enumerates
//           Exchange11, 2-opt, 2-opt*, and bounded ExchangeNM/CROSS moves.
//  - BASIC: a nearby entity pair; every value of each entity (the same
//           enumeration the construction heuristic and the LNS repair already
//           do for one entity) plus the swap of the two. What makes it a
//           descent rather than a greedy placement is that it keeps sweeping —
//           after e1 takes its best value, e2 is re-evaluated against the new
//           assignment, and so on.
//
// One application is worth O(|l1| * |l2|) score evaluations on LIST and
// ~2 * valueCount on BASIC, against 1 for an ordinary move. It earns its place
// in the pool only because it nearly always improves — which is also why the
// weight it is drawn at has to be set against what it costs, and why on BASIC
// that cost is set by the VALUE RANGE and so the usable weight differs by two
// orders of magnitude between domains (see LocalSearchConfig::descentWeight).
//
// As a plugin this now goes through the acceptor like any other move, where
// the phase used to apply it directly on the grounds that it is improving by
// construction. The outcome is the same — it only ever reports a strictly
// improving result, and every acceptor takes those — but the invariant moved
// from "bypasses the acceptor" to "relies on the acceptor accepting an
// improvement". Note also that an element relocated on more than one sweep
// appears in `entities` more than once, which entity tabu tolerates.
//
// Both kinds.
template <typename Solution>
class Descent final : public MoveOperator<Solution> {
    using Descriptor = SolutionDescriptor<Solution>;

public:
    // `entityRecommender` is the BASIC-only guided-selection hook: when a use
    // case supplies one, the descent starts where the violations are instead of
    // at a uniform draw. May be empty.
    // `unorderedList` (LIST kind, -1 = none) is a list whose element order
    // carries no score, such as an unassigned pool: reordering moves within
    // it (2-opt, same-list swap) are never improving and are not scanned.
    Descent(ElementIndex<Solution>& index, const NearbyNeighborhoods& nearby, int maxSweeps,
            const std::function<int(std::mt19937_64&)>& entityRecommender,
            bool advancedList = false, int listMaxSegment = 2,
            int listCandidateRoutes = 4, int unorderedList = -1)
        : index_(index),
          nearby_(nearby),
          maxSweeps_(maxSweeps),
          entityRecommender_(entityRecommender),
          advancedList_(advancedList),
          listMaxSegment_(std::max(1, listMaxSegment)),
          listCandidateRoutes_(std::max(1, listCandidateRoutes)),
          unorderedList_(unorderedList) {}

    const char* name() const override { return "descent"; }

    bool apply(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        applied_.clear();
        assignments_.clear();
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            return applyBasic(ctx, rng, entities);
        } else if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)ctx;
            (void)rng;
            (void)entities;
            return false;
        } else {
            ScoreDirector<Solution>& director = ctx.director;
            Solution& solution = director.workingSolution();
            const int listCount = Descriptor::listCount(solution);
            const int elementCount = Descriptor::elementCount(solution);
            if (listCount < 2 || elementCount < 2) return false;
            throttle_.reset(ctx.stopRequested);
            evaluations_ = 0;

            const int seed =
                    static_cast<int>(rng() % static_cast<std::uint64_t>(elementCount));
            const int l1 = index_.listOf(seed);
            if (l1 < 0) return false;

            // The pair: the seed's list and one of the closest routes that
            // contains a granular-neighbour. PyVRP samples from a bounded
            // candidate set rather than considering every route pair; doing
            // the same keeps the exhaustive move scan predictable on large
            // instances while avoiding unrelated route exchanges.
            int l2 = -1;
            std::vector<int> routeCandidates;
            if (advancedList_ && nearby_.enabled()) {
                for (int nb : nearby_.of(seed)) {
                    const int list = index_.listOf(nb);
                    if (list < 0 || list == l1) continue;
                    if (std::find(routeCandidates.begin(), routeCandidates.end(), list) ==
                        routeCandidates.end()) {
                        routeCandidates.push_back(list);
                        if (static_cast<int>(routeCandidates.size()) >= listCandidateRoutes_)
                            break;
                    }
                }
            } else if (nearby_.enabled()) {
                for (int nb : nearby_.of(seed)) {
                    const int list = index_.listOf(nb);
                    if (list >= 0 && list != l1) {
                        routeCandidates.push_back(list);
                        break;
                    }
                }
            }
            if (!routeCandidates.empty()) {
                l2 = routeCandidates[static_cast<std::size_t>(rng() % routeCandidates.size())];
            }
            if (l2 < 0) {
                l2 = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount - 1));
                if (l2 >= l1) ++l2;
            }

            HardSoftScore current = director.calculateScore();
            for (int sweep = 0; sweep < maxSweeps_ && !throttle_.stopped(); ++sweep) {
                if (advancedList_ ? !bestGranularMove(director, current, l1, l2)
                                  : !bestRelocateBetween(director, current, l1, l2))
                    break;
                current = director.calculateScore();
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

    void undoLast(OperatorContext<Solution>& ctx) override {
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            // Reverse order: each entry restores the value that entity held
            // before its own change, so replaying backwards unwinds a swap
            // correctly too.
            ctx.director.beginVariableChanges();
            for (auto it = assignments_.rbegin(); it != assignments_.rend(); ++it) {
                ctx.director.changeVariable(it->entity, it->previousValue);
            }
            ctx.director.endVariableChanges();
            assignments_.clear();
        } else if constexpr (Descriptor::kind != VariableKind::LIST) {
            (void)ctx;
        } else {
            // Reverse order, so each operation sees the list state it created.
            for (auto it = applied_.rbegin(); it != applied_.rend(); ++it) {
                Moves::undo(ctx.director, index_, *it);
            }
            applied_.clear();
        }
    }

private:
    using Moves = ListMoves<Solution>;

    // BASIC: one variable write, with what it overwrote.
    struct Assignment {
        int entity;
        int previousValue;
    };

    // --- BASIC ---

    // Picks an entity (through the recommender when the use case supplies one)
    // plus a nearby partner, then repeatedly applies the single best improving
    // change-or-swap over the two until none is left or the sweep cap hits.
    bool applyBasic(OperatorContext<Solution>& ctx, std::mt19937_64& rng,
                    std::vector<int>& entities) {
        if constexpr (Descriptor::kind != VariableKind::BASIC) {
            (void)ctx;
            (void)rng;
            (void)entities;
            return false;
        } else {
            ScoreDirector<Solution>& director = ctx.director;
            Solution& solution = director.workingSolution();
            const int entityCount = Descriptor::entityCount(solution);
            const int valueCount = Descriptor::valueCount(solution);
            if (entityCount < 1 || valueCount < 2) return false;

            std::uniform_int_distribution<int> entityPick(0, entityCount - 1);
            int e1 = entityRecommender_ ? entityRecommender_(rng) : -1;
            if (e1 < 0 || e1 >= entityCount) e1 = entityPick(rng);
            e1 = movableEntity(solution, e1, entityPick, rng);
            // Degenerate all-pinned case: movableEntity hands back the pinned
            // entity it was given rather than corrupting the assignment.
            if (entityPinned<Descriptor>(solution, e1)) return false;

            // Partner: a nearby entity when a distance meter is configured
            // (classmates in timetabling, same-day shifts in rostering), else a
            // uniform draw. -1 = descend on e1 alone.
            int e2 = -1;
            if (entityCount >= 2) {
                if (nearby_.enabled() && !nearby_.of(e1).empty()) {
                    e2 = nearby_.of(e1)[rng() % nearby_.of(e1).size()];
                } else {
                    e2 = entityPick(rng);
                }
                if (e2 == e1 || e2 < 0 || e2 >= entityCount ||
                    entityPinned<Descriptor>(solution, e2)) {
                    e2 = -1;
                }
            }

            HardSoftScore current = director.calculateScore();
            for (int sweep = 0; sweep < maxSweeps_; ++sweep) {
                if (!bestChangeOrSwapBetween(director, current, e1, e2, valueCount)) break;
                current = director.calculateScore();
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
    bool bestChangeOrSwapBetween(ScoreDirector<Solution>& director,
                                 const HardSoftScore& currentScore, int e1, int e2,
                                 int valueCount) {
        Solution& solution = director.workingSolution();
        HardSoftScore best = currentScore;
        int bestEntity = -1, bestValue = -1;
        bool bestIsSwap = false;

        const int old1 = Descriptor::getVariable(solution, e1);
        const int old2 = e2 >= 0 ? Descriptor::getVariable(solution, e2) : -1;

        for (int pick = 0; pick < (e2 >= 0 ? 2 : 1); ++pick) {
            const int e = pick == 0 ? e1 : e2;
            const int old = pick == 0 ? old1 : old2;
            for (int v = 0; v < valueCount; ++v) {
                if (v == old) continue;
                director.changeVariable(e, v);
                const HardSoftScore candidate = director.calculateScore();
                if (candidate > best) {
                    best = candidate;
                    bestEntity = e;
                    bestValue = v;
                }
            }
            director.changeVariable(e, old);  // put it back
        }

        // The swap is evaluated last, so it only wins by beating every single
        // change above — which is what makes bestIsSwap unambiguous.
        if (e2 >= 0 && old1 != old2) {
            director.beginVariableChanges();
            director.changeVariable(e1, old2);
            director.changeVariable(e2, old1);
            director.endVariableChanges();
            const HardSoftScore candidate = director.calculateScore();
            if (candidate > best) {
                best = candidate;
                bestIsSwap = true;
            }
            director.beginVariableChanges();
            director.changeVariable(e1, old1);
            director.changeVariable(e2, old2);
            director.endVariableChanges();
        }

        if (bestIsSwap) {
            director.beginVariableChanges();
            director.changeVariable(e1, old2);
            director.changeVariable(e2, old1);
            director.endVariableChanges();
            assignments_.push_back({e1, old1});
            assignments_.push_back({e2, old2});
            return true;
        }
        if (bestEntity < 0) return false;
        director.changeVariable(bestEntity, bestValue);
        assignments_.push_back({bestEntity, bestEntity == e1 ? old1 : old2});
        return true;
    }

    // One pass: try relocating every element of l1 into every position of l2
    // and vice versa, keep the best strictly-improving one, apply it.
    // Returns whether anything was applied.
    // Score every relocation of an element of `from` into `to` and hand each
    // (score, p, q, element) to `visit`. `visit` must not read `from` itself:
    // on the slow path the element is out of the list while it runs. Each
    // slot is one evaluateWithPreview (a served preview is used as is, and
    // verified under FULL_ASSERT); once the calculator declines, the element
    // comes out of `from` once and every remaining slot costs one insert and
    // one remove. Trial insertions go straight through the director: the
    // index is not consulted between the remove and the matching put back,
    // and keeping it in sync here would cost a reindex per candidate.
    template <typename Visit>
    void scanRelocations(ScoreDirector<Solution>& director, int from, int to, Visit&& visit) {
        Solution& solution = director.workingSolution();
        const int fromSize = Descriptor::listSize(solution, from);
        for (int p = 0; p < fromSize && !throttle_.stopped(); ++p) {
            const int element = Descriptor::getElement(solution, from, p);
            const int toSize = Descriptor::listSize(solution, to);
            bool out = false;  // element taken out of `from` for the slow path
            for (int q = 0; q <= toSize && !throttle_.stopped(); ++q) {
                countEvaluation();
                if (!out) {
                    bool served = false;
                    const HardSoftScore score = evaluateWithPreview(
                            director,
                            [&](IncrementalScoreCalculator<Solution>& c, HardSoftScore& o) {
                                return Moves::preview(c, ListMove::segment(from, p, 1, to, q), o);
                            },
                            [&] {
                                director.removeListElement(from, p);
                                director.insertListElement(to, q, element);
                            },
                            [&] {
                                director.removeListElement(to, q);
                                director.insertListElement(from, p, element);
                            },
                            &served);
                    visit(score, p, q, element);
                    if (served) continue;
                    // Declined: the remaining slots take the slow path with
                    // the element out of `from` for all of them.
                    director.removeListElement(from, p);
                    out = true;
                    continue;
                }
                director.insertListElement(to, q, element);
                visit(director.calculateScore(), p, q, element);
                director.removeListElement(to, q);
            }
            if (out) director.insertListElement(from, p, element);
        }
    }

    bool bestRelocateBetween(ScoreDirector<Solution>& director, const HardSoftScore& currentScore,
                             int l1, int l2) {
        HardSoftScore best = currentScore;
        int bestFrom = -1, bestPos = -1, bestTo = -1, bestInsert = -1;

        for (int direction = 0; direction < 2; ++direction) {
            const int from = direction == 0 ? l1 : l2;
            const int to = direction == 0 ? l2 : l1;
            scanRelocations(director, from, to,
                            [&](const HardSoftScore& candidate, int p, int q, int /*element*/) {
                if (candidate > best) {
                    best = candidate;
                    bestFrom = from;
                    bestPos = p;
                    bestTo = to;
                    bestInsert = q;
                }
            });
        }

        if (bestFrom < 0) return false;
        const ListMove move = ListMove::segment(
                bestFrom, bestPos, 1, bestTo, bestInsert,
                Descriptor::getElement(director.workingSolution(), bestFrom, bestPos));
        Moves::apply(director, index_, move);
        applied_.push_back(move);
        return true;
    }

    // Granular route-pair descent. The ordinary LIST move selector samples
    // one candidate and scores it. This pass enumerates the bounded
    // neighbourhood used by PyVRP-style local search and applies the single
    // best improving move. All trial moves are reverted immediately, so the
    // live element index remains coherent and undoLast() can restore the
    // accepted sequence atomically if the acceptor rejects it.
    bool bestGranularMove(ScoreDirector<Solution>& director,
                          const HardSoftScore& currentScore, int l1, int l2) {
        Solution& solution = director.workingSolution();
        HardSoftScore best = currentScore;
        ListMove bestMove;

        auto remember = [&](const HardSoftScore& candidate, const ListMove& move) {
            if (candidate > best) {
                best = candidate;
                bestMove = move;
            }
        };

        // Exchange(1,0): relocate an element between the two routes. This is
        // the existing descent neighbourhood and is still the most valuable
        // move for capacity balancing.
        for (int direction = 0; direction < 2; ++direction) {
            const int from = direction == 0 ? l1 : l2;
            const int to = direction == 0 ? l2 : l1;
            scanRelocations(director, from, to,
                            [&](const HardSoftScore& candidate, int p, int q, int element) {
                                remember(candidate, ListMove::segment(from, p, 1, to, q, element));
                            });
        }

        // Exchange(1,1), both between routes and within each route. The
        // intra-route form is important for ordering and is the cheap
        // counterpart of PyVRP's Exchange11 operator.
        considerSwaps(director, l1, l2, remember);

        // 2-opt/2-opt inside either route.
        for (int list : {l1, l2}) {
            if (list == unorderedList_) continue;  // order carries no score
            const int size = Descriptor::listSize(solution, list);
            for (int first = 0; first + 1 < size && !throttle_.stopped(); ++first) {
                for (int last = first + 1; last < size && !throttle_.stopped(); ++last) {
                    const ListMove move = ListMove::reverse(
                            list, first, last, Descriptor::getElement(solution, list, first),
                            Descriptor::getElement(solution, list, last));
                    countEvaluation();
                    remember(Moves::evaluate(director, index_, move), move);
                }
            }
        }

        // 2-opt*: exchange route tails at every pair of cuts. Empty tails are
        // allowed, so this also covers a route-level relocate near a depot.
        const int size1 = Descriptor::listSize(solution, l1);
        const int size2 = Descriptor::listSize(solution, l2);
        for (int cut1 = 0; cut1 <= size1 && !throttle_.stopped(); ++cut1) {
            for (int cut2 = 0; cut2 <= size2 && !throttle_.stopped(); ++cut2) {
                if (cut1 == size1 && cut2 == size2) continue;
                const ListMove move = ListMove::tailSwap(
                        l1, cut1, l2, cut2,
                        cut1 < size1 ? Descriptor::getElement(solution, l1, cut1) : -1,
                        cut2 < size2 ? Descriptor::getElement(solution, l2, cut2) : -1);
                countEvaluation();
                remember(Moves::evaluate(director, index_, move), move);
            }
        }

        // Small CROSS-exchange (ExchangeNM). A bounded segment length keeps
        // the scan granular; larger segments are left to ruin/recreate and
        // the sampled inter-exchange move.
        const int maxSegment = std::max(1, listMaxSegment_);
        for (int p1 = 0; p1 < size1 && !throttle_.stopped(); ++p1) {
            for (int len1 = 0; len1 <= maxSegment && p1 + len1 <= size1; ++len1) {
                for (int p2 = 0; p2 < size2 && !throttle_.stopped(); ++p2) {
                    for (int len2 = 0; len2 <= maxSegment && p2 + len2 <= size2; ++len2) {
                        if (len1 == 0 && len2 == 0) continue;
                        const ListMove move = ListMove::exchange(
                                l1, p1, len1, l2, p2, len2,
                                p1 < size1 ? Descriptor::getElement(solution, l1, p1) : -1,
                                p2 < size2 ? Descriptor::getElement(solution, l2, p2) : -1);
                        countEvaluation();
                        remember(Moves::evaluate(director, index_, move), move);
                    }
                }
            }
        }

        // A scan cut short by the termination still applies what it found:
        // an improvement is an improvement, and the sweep loop stops after.
        if (!bestMove.valid()) return false;
        Moves::apply(director, index_, bestMove);
        applied_.push_back(bestMove);
        return true;
    }

    template <typename Remember>
    void considerSwaps(ScoreDirector<Solution>& director, int l1, int l2,
                       Remember&& remember) {
        Solution& solution = director.workingSolution();
        const int size1 = Descriptor::listSize(solution, l1);
        const int size2 = Descriptor::listSize(solution, l2);

        for (int p1 = 0; p1 < size1 && !throttle_.stopped(); ++p1) {
            for (int p2 = 0; p2 < size2 && !throttle_.stopped(); ++p2) {
                const int e1 = Descriptor::getElement(solution, l1, p1);
                const int e2 = Descriptor::getElement(solution, l2, p2);
                const ListMove move = ListMove::swap(l1, p1, l2, p2, e1, e2);
                countEvaluation();
                remember(Moves::evaluate(director, index_, move), move);
            }
        }

        // Same-route Exchange11. It is deliberately kept separate from the
        // cross-route loop because ElementIndex::swapElements handles both
        // positions correctly, but the pair enumeration must avoid p == q.
        for (int list : {l1, l2}) {
            if (list == unorderedList_) continue;  // order carries no score
            const int size = Descriptor::listSize(solution, list);
            for (int p1 = 0; p1 + 1 < size && !throttle_.stopped(); ++p1) {
                for (int p2 = p1 + 1; p2 < size && !throttle_.stopped(); ++p2) {
                    const int e1 = Descriptor::getElement(solution, list, p1);
                    const int e2 = Descriptor::getElement(solution, list, p2);
                    const ListMove move = ListMove::swap(list, p1, list, p2, e1, e2);
                    countEvaluation();
                    remember(Moves::evaluate(director, index_, move), move);
                }
            }
        }
    }

    // These two composite operations intentionally mirror the ordinary LIST
    // move implementation. Each list is bracketed once so incremental
    // calculators see a coherent route change, and both list indexes are
    // rebuilt before the next candidate is evaluated.
    // One scan of a route pair is thousands of evaluations; on a calculator
    // whose previews cost O(route) (a decomposed assembly, a plan with
    // waiting) that is long enough to overrun a short time limit, so the
    // termination is consulted through the throttle and the scan stops
    // where it is.
    void countEvaluation() { throttle_.reached(++evaluations_); }

    ElementIndex<Solution>& index_;
    const NearbyNeighborhoods& nearby_;
    int maxSweeps_;
    const std::function<int(std::mt19937_64&)>& entityRecommender_;
    bool advancedList_;
    int listMaxSegment_;
    int listCandidateRoutes_;
    int unorderedList_;
    TerminationThrottle throttle_;
    std::int64_t evaluations_ = 0;
    std::vector<ListMove> applied_;        // LIST
    std::vector<Assignment> assignments_;  // BASIC
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_STRATEGY_DESCENT_HPP
