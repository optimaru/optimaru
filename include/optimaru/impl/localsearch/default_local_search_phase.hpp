#ifndef OPTIMARU_IMPL_LOCALSEARCH_DEFAULT_LOCAL_SEARCH_PHASE_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_DEFAULT_LOCAL_SEARCH_PHASE_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "optimaru/impl/heuristic/selector/common/nearby/nearby_neighborhoods.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin_recreate_selector.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin/random_ruin.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin/related_ruin.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin/route_ruin.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin/sisr_ruin.hpp"
#include "optimaru/impl/heuristic/selector/move/generic/ruin/worst_ruin.hpp"
#include "optimaru/impl/localsearch/decider/acceptor/acceptor.hpp"
#include "optimaru/impl/localsearch/decider/acceptor/greatdeluge/great_deluge_acceptor.hpp"
#include "optimaru/impl/localsearch/decider/acceptor/hillclimbing/hill_climbing_acceptor.hpp"
#include "optimaru/impl/localsearch/decider/acceptor/lateacceptance/late_acceptance_acceptor.hpp"
#include "optimaru/impl/localsearch/decider/acceptor/recordtorecordtravel/record_to_record_travel_acceptor.hpp"
#include "optimaru/impl/localsearch/decider/acceptor/simulatedannealing/simulated_annealing_acceptor.hpp"
#include "optimaru/impl/localsearch/decider/acceptor/stepcountinghillclimbing/step_counting_hill_climbing_acceptor.hpp"
#include "optimaru/impl/localsearch/decider/acceptor/tabu/entity_tabu_acceptor.hpp"
#include "optimaru/impl/localsearch/element_index.hpp"
#include "optimaru/impl/localsearch/greedy_repair.hpp"
#include "optimaru/impl/localsearch/list_move.hpp"
#include "optimaru/impl/localsearch/movable_entity.hpp"
#include "optimaru/impl/localsearch/solution_snapshot.hpp"
#include "optimaru/impl/localsearch/strategy/descent.hpp"
#include "optimaru/impl/localsearch/strategy/ejection_chain.hpp"
#include "optimaru/impl/localsearch/preview_evaluation.hpp"
#include "optimaru/impl/localsearch/strategy/iterated_local_search.hpp"
#include "optimaru/impl/localsearch/strategy/route_elimination.hpp"
#include "optimaru/impl/localsearch/strategy/ruin_recreate_move.hpp"
#include "optimaru/impl/solver/termination/termination.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// DefaultLocalSearchPhase
// ---------------------------------------------------------------------------
// Mirrors impl.localsearch.DefaultLocalSearchPhase: one candidate move per
// step, judged by the configured Acceptor, until the SolverTermination fires;
// optionally interleaved with adaptive ruin-and-recreate kicks when the
// search stagnates.
//   BASIC moves: ChangeMove (reassign one entity), SwapMove.
//   LIST moves:  relocate (move one element), element swap, 2-opt
//                (reverse a segment within one list), Or-opt (move a 2-3
//                element segment), 2-opt* (tail swap) and CROSS-exchange;
//                mixed by MoveSelectorConfig weights (the last three default to 0).
// Moves follow a do/undo protocol: apply through the
// ScoreDirector, read calculateScore(), and revert when rejected.
//
// Anything more elaborate than the random move mix above is a plugin, not a
// branch in this file: the built-in strategies under strategy/ implement the
// same MoveOperator interface a caller gets from Solver::addMoveOperator(),
// and compete for selection in one adaptive pool with them.
template <typename Solution>
class DefaultLocalSearchPhase {
    using Descriptor = SolutionDescriptor<Solution>;
    using Snapshot = SolutionSnapshot<Solution>;

public:
    DefaultLocalSearchPhase(
            const SolverConfig& config, const NearbyNeighborhoods& nearby,
            const std::vector<std::unique_ptr<MoveOperator<Solution>>>& moveOperators,
            const std::vector<std::unique_ptr<RuinOperator<Solution>>>& ruinOperators,
            const std::function<std::unique_ptr<Acceptor>(const HardSoftScore&)>& acceptorFactory)
        : config_(config),
          nearby_(nearby),
          moveOperators_(moveOperators),
          ruinOperators_(ruinOperators),
          acceptorFactory_(acceptorFactory),
          repair_(index_, nearby, config.localSearch.ruinRecreate,
                  config.localSearch.unassignedList) {}

    HardSoftScore run(ScoreDirector<Solution>& director, SolverTermination& termination) {
        Solution& solution = director.workingSolution();
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            if (Descriptor::entityCount(solution) == 0 || Descriptor::valueCount(solution) == 0) {
                return director.calculateScore();
            }
        } else {
            if (Descriptor::listCount(solution) == 0 || countListElements(solution) == 0) {
                return director.calculateScore();
            }
            index_.build(solution);
        }

        const RuinRecreateConfig& lns = config_.localSearch.ruinRecreate;
        const LocalSearchConfig& ls = config_.localSearch;
        HardSoftScore currentScore = director.calculateScore();
        HardSoftScore bestScore = currentScore;
        auto best = Snapshot::take(solution);
        if (config_.onNewBest) config_.onNewBest(0, bestScore);

        std::unique_ptr<Acceptor> acceptor = makeAcceptor(currentScore);
        // Custom acceptors may consume the time gradient. Of the built-ins,
        // only simulated annealing does; avoid a duration conversion and
        // floating-point division on every move for the default LA path.
        const bool needsTimeGradient =
                static_cast<bool>(acceptorFactory_) ||
                config_.localSearch.acceptor.acceptorType ==
                        AcceptorType::SIMULATED_ANNEALING;
        // The built-in ruins are plugins like any other: one file each under
        // generic/ruin/, registered ahead of the caller's operators so the
        // adaptive selector sees a single uniform list.
        RandomRuin<Solution> randomRuin;
        RelatedRuin<Solution> relatedRuin;
        WorstRuin<Solution> worstRuin;
        [[maybe_unused]] SisrRuin<Solution> sisrRuin(lns.sisrMaxStringRemovals,
                                                    lns.sisrMaxStringSize);
        [[maybe_unused]] RouteRuin<Solution> routeRuin;
        std::vector<RuinOperator<Solution>*> ruins{&randomRuin, &relatedRuin, &worstRuin};
        // LIST kind only — and not merely as an optimization. Both delegate to
        // the random ruin on BASIC solutions (a "substring" and a "route" mean
        // nothing without an ordered list), so registering them there would put
        // three copies of the random ruin in a five-operator pool and silently
        // triple its selection weight for every BASIC use case.
        if constexpr (Descriptor::kind == VariableKind::LIST) {
            if (lns.sisrRuin) ruins.push_back(&sisrRuin);
            if (lns.routeRuin) ruins.push_back(&routeRuin);
        }
        for (const auto& op : ruinOperators_) ruins.push_back(op.get());
        auto ruinSelector = makeOperatorSelectionScheme(
                lns.selectionType, static_cast<int>(ruins.size()), lns.rewards, lns.operatorDecay,
                lns.selectionSegmentLength, lns.selectionAlpha);

        // Adaptive selection over {built-in move mix, custom operators...};
        // only consulted when custom move operators are registered.
        auto moveSelector = makeOperatorSelectionScheme(
                lns.selectionType, 1 + static_cast<int>(moveOperators_.size()), lns.rewards,
                lns.operatorDecay, lns.selectionSegmentLength, lns.selectionAlpha);
        OperatorContext<Solution> operatorContext{director, nearby_, index_.lists(),
                                                  index_.positions()};

        // The built-in strategies: plugins like any other MoveOperator, drawn
        // by weight before the ordinary move mix gets its turn. Their weights
        // are small by necessity — each costs orders of magnitude more than one
        // sampled move — and the header of LocalSearchConfig records why that
        // is a configured number here rather than something the adaptive
        // selector above is asked to discover.
        [[maybe_unused]] RouteElimination<Solution> routeElimination(index_, repair_,
                                                                     ls.routeEliminationMaxSize);
        Descent<Solution> descent(index_, nearby_, ls.descentMaxSweeps, ls.entityRecommender,
                                  ls.descentListAdvanced, ls.descentListMaxSegment,
                                  ls.descentListCandidateRoutes, ls.unassignedList);
        RuinRecreateMove<Solution> ruinRecreateMove(index_, repair_, ruins, *ruinSelector,
                                                    lns.asMoveMaxRuin);
        [[maybe_unused]] EjectionChain<Solution> ejectionChain(
                index_, nearby_, ls.unassignedList, ls.ejectionChainMaxSteps,
                ls.ejectionChainCandidateLists, ls.ejectionChainMaxEvaluations);
        [[maybe_unused]] GranularLocalSearch<Solution> granularLocalSearch(
                index_, nearby_, ls.iteratedLocalSearchMaxSegment);
        // Its own adaptive ruin weights: an ILS outcome is measured after the
        // descent and would otherwise retrain the destroy-repair move's.
        auto ilsRuinSelector = makeOperatorSelectionScheme(
                lns.selectionType, static_cast<int>(ruins.size()), lns.rewards, lns.operatorDecay,
                lns.selectionSegmentLength, lns.selectionAlpha);
        [[maybe_unused]] IteratedLocalSearchMove<Solution> iteratedLocalSearch(
                index_, repair_, granularLocalSearch, ruins, *ilsRuinSelector,
                ls.iteratedLocalSearchMinRuin, ls.iteratedLocalSearchMaxRuin,
                ls.unassignedList, ls.iteratedLocalSearchMaxEvaluations);
        std::vector<WeightedStrategy> strategies;
        // Route elimination is LIST-only, for the same reason the SISR and
        // route ruins are: it declines on BASIC solutions, so registering it
        // there would only burn steps on an operator that can never move.
        // Descent has a neighbourhood on both kinds and is registered on both.
        if constexpr (Descriptor::kind == VariableKind::LIST) {
            if (ls.routeEliminationWeight > 0.0) {
                strategies.push_back({ls.routeEliminationWeight, &routeElimination});
            }
            if (ls.ejectionChainWeight > 0.0) {
                strategies.push_back({ls.ejectionChainWeight, &ejectionChain});
            }
            if (ls.iteratedLocalSearchWeight > 0.0) {
                strategies.push_back({ls.iteratedLocalSearchWeight, &iteratedLocalSearch});
            }
        }
        if (ls.descentWeight > 0.0) {
            strategies.push_back({ls.descentWeight, &descent});
        }
        if (lns.asMoveWeight > 0.0) {
            strategies.push_back({lns.asMoveWeight, &ruinRecreateMove});
        }
        std::mt19937_64 rng(config_.randomSeed);
        // Reused by caller-provided operators. Keeping it out of AppliedMove
        // avoids constructing and destroying a std::vector on every built-in
        // move, which is the hot path for all use cases.
        std::vector<int> customEntities;
        std::int64_t step = 0;
        std::int64_t stepsSinceBestOrLns = 0;
        // The strategies that evaluate thousands of candidates inside one
        // move (the iterated local search's descent, the ejection chain's
        // repair) consult the termination between batches of evaluations, so
        // one move cannot run far past a time limit the step loop only reads
        // between moves. Only the clock-reading criteria matter here; the
        // step-based ones see the current step.
        const std::function<bool()> stopRequested = [&] {
            return termination.isTerminated(step, bestScore, /*forceTimeCheck=*/true);
        };
        operatorContext.stopRequested = &stopRequested;
        // Kick cadence. "Stagnated" must be judged relative to how often this
        // instance normally improves: on a large instance plain LA improves
        // every ~10^5-10^6 steps, so a fixed step trigger would kick mid-
        // progress and turn the search into a perpetual restart. Two guards:
        //  - dynamic base: 8x the EMA of recent steps-between-new-bests,
        //    never below the configured trigger;
        //  - exponential backoff: every kick that fails to reach a new best
        //    before the next trigger doubles the interval (capped so kicks
        //    never stop entirely); any new best resets it.
        std::int64_t lnsTrigger = lns.unimprovedStepTrigger;
        const std::int64_t lnsTriggerCap = lns.unimprovedStepTrigger << 12;
        std::int64_t lastBestStep = 0;
        std::int64_t emaBestGap = 0;
        bool forceTimeCheck = false;

        while (!termination.isTerminated(step, bestScore, forceTimeCheck)) {
            forceTimeCheck = false;
            if (lns.enabled && stepsSinceBestOrLns >= std::max(lnsTrigger, 8 * emaBestGap)) {
                currentScore = lnsIteration(director, currentScore, bestScore, *ruinSelector,
                                            ruins, operatorContext, rng);
                forceTimeCheck = true;
                // A perturbation kick becomes the new incumbent uncondition-
                // ally and the acceptor restarts fresh, so
                // the following small moves re-polish the kicked solution
                // instead of instantly rejecting it (best is snapshotted, so
                // a bad kick can never worsen the returned solution).
                acceptor = makeAcceptor(currentScore);
                stepsSinceBestOrLns = 0;
                lnsTrigger = std::min(lnsTrigger * 2, lnsTriggerCap);
            } else {
                // A built-in strategy plugin if one is drawn, otherwise the
                // ordinary move mix / a caller's operator under the adaptive
                // selector. `selectorIndex` < 0 means nothing to report back.
                MoveOperator<Solution>* op = drawStrategy(strategies, rng);
                int selectorIndex = -1;
                bool custom = false;
                if (op == nullptr) {
                    int moveOp = 0;
                    bool selectorConsulted = false;
                    if (!moveOperators_.empty()) {
                        const double gate = ls.customMoveProbability;
                        if (gate >= 1.0 || (gate > 0.0 && unit_(rng) < gate)) {
                            moveOp = moveSelector->select(rng);
                            selectorConsulted = true;
                        }
                    }
                    if (moveOp != 0) {
                        op = moveOperators_[static_cast<std::size_t>(moveOp) - 1].get();
                        custom = true;
                    }
                    if (selectorConsulted) selectorIndex = moveOp;
                }
                AppliedMove move;
                bool applied = true;
                // A previewed move is scored from the calculator's cached
                // route data without touching the solution, and applied only
                // if the acceptor takes it. Most moves are rejected, so this
                // is where the apply/score/undo cost of a step disappears.
                // `previewed`: the move is not applied and `sampledScore` is
                // its preview; `scored`: `sampledScore` is its score either
                // way (see scoreListMove).
                bool previewed = false;
                bool scored = false;
                HardSoftScore sampledScore;
                director.beginMoveEvaluation();
                if (op == nullptr) {
                    if constexpr (Descriptor::kind == VariableKind::BASIC) {
                        move = applyBasicMove(director, rng);
                    } else {
                        move = sampleListMove(director, rng);
                        previewed = scoreListMove(director, move, sampledScore);
                        scored = true;
                    }
                } else {
                    if (custom) director.startTouchedListTracking();
                    customEntities.clear();
                    applied = op->apply(operatorContext, rng, customEntities);
                }
                if (applied) {
                    HardSoftScore candidateScore = scored ? sampledScore
                                                          : director.calculateScore();
                    const int* moveEntities = op == nullptr ? move.entities
                                                            : customEntities.data();
                    const int moveEntityCount =
                            op == nullptr ? move.entityCount
                                          : static_cast<int>(customEntities.size());
                    bool accepted = acceptor->isAccepted(candidateScore, currentScore, bestScore,
                                                         moveEntities, moveEntityCount,
                                                         needsTimeGradient
                                                                 ? termination.timeGradient()
                                                                 : 0.0,
                                                         rng);
                    const LnsOutcome outcome =
                            candidateScore > bestScore      ? LnsOutcome::NEW_BEST
                            : candidateScore > currentScore ? LnsOutcome::BETTER
                            : accepted                      ? LnsOutcome::ACCEPTED
                                                            : LnsOutcome::REJECTED;
                    if (accepted) {
                        currentScore = candidateScore;
                        acceptor->moveAccepted(moveEntities, moveEntityCount);
                        if (previewed) {
                            if constexpr (Descriptor::kind == VariableKind::LIST) {
                                applyListMove(director, move);
                            }
                        }
                    } else if (!previewed) {  // a previewed move was never applied
                        director.beginMoveRollback();
                        if (op == nullptr) {
                            undoMove(director, move);
                        } else {
                            op->undoLast(operatorContext);
                        }
                    }
                    director.endMoveEvaluation(accepted);
                    if (selectorIndex >= 0) moveSelector->update(selectorIndex, outcome);
                    if (op != nullptr) op->outcome(outcome);
                } else {
                    director.endMoveEvaluation(false);
                }
                if (op != nullptr && custom) {
                    // Resynchronize the element index with whatever lists the
                    // custom operator touched (apply and/or undo).
                    if constexpr (Descriptor::kind == VariableKind::LIST) {
                        for (int l : director.touchedLists()) index_.reindex(solution, l);
                    }
                    director.stopTouchedListTracking();
                }
                // Built-in strategies and caller-provided operators may scan
                // a neighbourhood or run a repair. Do not defer the clock
                // sample after those potentially expensive steps.
                if (op != nullptr) forceTimeCheck = true;
            }
            acceptor->stepEnded(currentScore);

            if (currentScore > bestScore) {
                bestScore = currentScore;
                best = Snapshot::take(solution);
                termination.onImprovement(step);
                stepsSinceBestOrLns = 0;
                lnsTrigger = lns.unimprovedStepTrigger;
                const std::int64_t gap = step - lastBestStep;
                emaBestGap = emaBestGap == 0 ? gap : (3 * emaBestGap + gap) / 4;
                lastBestStep = step;
                if (config_.onNewBest) config_.onNewBest(step, bestScore);
            } else {
                ++stepsSinceBestOrLns;
            }
            ++step;
        }

        Snapshot::restore(director, best);
        return bestScore;
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
    static MoveOperator<Solution>* drawStrategy(const std::vector<WeightedStrategy>& strategies,
                                                std::mt19937_64& rng) {
        if (strategies.empty()) return nullptr;
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        for (const WeightedStrategy& s : strategies) {
            if (unit(rng) < s.weight) return s.op;
        }
        return nullptr;
    }

    const SolverConfig& config_;
    const NearbyNeighborhoods& nearby_;
    const std::vector<std::unique_ptr<MoveOperator<Solution>>>& moveOperators_;
    const std::vector<std::unique_ptr<RuinOperator<Solution>>>& ruinOperators_;
    const std::function<std::unique_ptr<Acceptor>(const HardSoftScore&)>& acceptorFactory_;
    // LIST kind: element id -> (list, position), kept in sync with every move
    // so nearby targets and LNS ruin work in O(1) per lookup. Declared before
    // repair_, which holds a reference to it.
    ElementIndex<Solution> index_;
    // Ruin & repair, shared by the stagnation kick here and the strategies
    // under strategy/ that do the same thing as an ordinary move.
    GreedyRepair<Solution> repair_;
    std::uniform_real_distribution<double> unit_{0.0, 1.0};

    std::unique_ptr<Acceptor> makeAcceptor(const HardSoftScore& initialScore) const {
        if (acceptorFactory_) return acceptorFactory_(initialScore);
        const LocalSearchAcceptorConfig& c = config_.localSearch.acceptor;
        switch (c.acceptorType) {
            case AcceptorType::HILL_CLIMBING:
                return std::make_unique<HillClimbingAcceptor>();
            case AcceptorType::ENTITY_TABU:
                return std::make_unique<EntityTabuAcceptor>(c.entityTabuSize);
            case AcceptorType::SIMULATED_ANNEALING:
                return std::make_unique<SimulatedAnnealingAcceptor>(
                        c.simulatedAnnealingStartingTemperature);
            case AcceptorType::GREAT_DELUGE:
                return std::make_unique<GreatDelugeAcceptor>(initialScore,
                                                             c.greatDelugeWaterLevelIncrement);
            case AcceptorType::STEP_COUNTING_HILL_CLIMBING:
                return std::make_unique<StepCountingHillClimbingAcceptor>(
                        c.stepCountingHillClimbingSize, initialScore);
            case AcceptorType::RECORD_TO_RECORD_TRAVEL: {
                // The threshold schedule spans the whole solve, so it needs a
                // step count. Prefer the configured limit; otherwise assume a
                // long run (10^6 steps) — overshooting only makes the
                // threshold decay more slowly, which RRT tolerates, whereas
                // undershooting collapses it to hill climbing immediately.
                const std::int64_t steps = config_.termination.stepCountLimit > 0
                                                   ? config_.termination.stepCountLimit
                                                   : 1000000;
                return std::make_unique<RecordToRecordTravelAcceptor>(
                        RecordToRecordTravelAcceptor::autofit(
                                initialScore.soft, c.recordToRecordTravelStartGap,
                                c.recordToRecordTravelEndGap,
                                c.recordToRecordTravelSteps > 0 ? c.recordToRecordTravelSteps
                                                                : steps,
                                c.recordToRecordTravelExponential
                                        ? RecordToRecordTravelAcceptor::Method::EXPONENTIAL
                                        : RecordToRecordTravelAcceptor::Method::LINEAR,
                                c.recordToRecordTravelCompareToBest));
            }
            case AcceptorType::LATE_ACCEPTANCE:
            default:
                return std::make_unique<LateAcceptanceAcceptor>(c.lateAcceptanceSize,
                                                                initialScore);
        }
    }

    bool useNearby(std::mt19937_64& rng) {
        if (!nearby_.enabled()) return false;
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        return unit(rng) < config_.localSearch.nearby.probability;
    }

    int randomOf(const std::vector<int>& v, std::mt19937_64& rng) {
        return v[rng() % v.size()];
    }

    // Uniformly draw from [0, n) except one value, without rejection. This is
    // used by moves where drawing the current entity/position is a guaranteed
    // no-op. Besides wasting a score evaluation, accepted no-ops advance Late
    // Acceptance history and dilute the useful neighbourhood.
    static int randomOther(int n, int excluded, std::mt19937_64& rng) {
        int picked = static_cast<int>(rng() % static_cast<std::uint64_t>(n - 1));
        if (picked >= excluded) ++picked;
        return picked;
    }

    // --- moves ---

    // One applied move, with what is needed to reject it and to feed the
    // acceptor's tabu memory.
    struct AppliedMove {
        int kind = 0;  // BASIC only: 0=change 1=swap (LIST moves are `list`)
        int a = 0, b = 0, c = 0, d = 0;  // BASIC: (entity, previous value) pairs
        ListMove list;                   // LIST: the move itself
        int entities[2] = {0, 0};
        int entityCount = 0;
    };

    AppliedMove applyBasicMove(ScoreDirector<Solution>& director, std::mt19937_64& rng) {
        Solution& solution = director.workingSolution();
        const LocalSearchConfig& ls = config_.localSearch;
        const MoveSelectorConfig& ms = ls.moveSelector;
        const int entityCount = Descriptor::entityCount(solution);
        const int valueCount = Descriptor::valueCount(solution);
        std::uniform_int_distribution<int> entityPick(0, entityCount - 1);
        std::uniform_int_distribution<int> valuePick(0, valueCount - 1);
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        int e = ls.entityRecommender ? ls.entityRecommender(rng) : -1;
        if (e < 0) e = entityPick(rng);
        e = movableEntity(solution, e, entityPick, rng);  // never move a pinned entity
        const int oldValue = Descriptor::getVariable(solution, e);

        const double swapWeight = entityCount < 2 ? 0.0 : ms.swapMoveWeight;
        const double total = ms.changeMoveWeight + swapWeight;

        AppliedMove move;
        if (unit(rng) * total < ms.changeMoveWeight || total <= 0) {
            int v = ls.valueRecommender ? ls.valueRecommender(rng, e) : -1;
            if (v < 0 || (valueCount > 1 && v == oldValue)) {
                if (valueCount > 1 && oldValue >= 0 && oldValue < valueCount) {
                    v = static_cast<int>(rng() % static_cast<std::uint64_t>(valueCount - 1));
                    if (v >= oldValue) ++v;
                } else {
                    v = valuePick(rng);
                }
            }
            director.changeVariable(e, v);
            move.kind = 0;
            move.a = e;
            move.b = oldValue;
            move.entities[0] = e;
            move.entityCount = 1;
        } else {
            // Swap partner: nearby-biased when a distance meter is configured.
            int e2;
            if (useNearby(rng) && !nearby_.of(e).empty()) {
                e2 = randomOf(nearby_.of(e), rng);
            } else {
                e2 = entityPick(rng);
            }
            e2 = movableEntity(solution, e2, entityPick, rng);  // no pinned swap partner
            for (int attempt = 0; attempt < 8 &&
                                  (e2 == e || Descriptor::getVariable(solution, e2) == oldValue);
                 ++attempt) {
                e2 = movableEntity(solution, entityPick(rng), entityPick, rng);
            }
            if (e2 == e || Descriptor::getVariable(solution, e2) == oldValue) {
                for (int candidate = 0; candidate < entityCount; ++candidate) {
                    if (candidate != e && !entityPinned<Descriptor>(solution, candidate) &&
                        Descriptor::getVariable(solution, candidate) != oldValue) {
                        e2 = candidate;
                        break;
                    }
                }
            }
            const int oldValue2 = Descriptor::getVariable(solution, e2);
            director.beginVariableChanges();
            director.changeVariable(e, oldValue2);
            director.changeVariable(e2, oldValue);
            director.endVariableChanges();
            move.kind = 1;
            move.a = e;
            move.b = oldValue;
            move.c = e2;
            move.d = oldValue2;
            move.entities[0] = e;
            move.entities[1] = e2;
            move.entityCount = 2;
        }
        return move;
    }

    static int countListElements(const Solution& solution) {
        int total = 0;
        for (int l = 0; l < Descriptor::listCount(solution); ++l) {
            total += Descriptor::listSize(solution, l);
        }
        return total;
    }

    // Draw one LIST move without touching the solution. Every coordinate is
    // in the CURRENT solution, with the one convention the calculator previews
    // share: a relocate / or-opt insertion index (`d`) indexes the destination
    // list AFTER the moved run has been removed, which only differs from the
    // pre-move index when source and destination are the same list.
    AppliedMove sampleListMove(ScoreDirector<Solution>& director, std::mt19937_64& rng) {
        Solution& solution = director.workingSolution();
        const LocalSearchConfig& ls = config_.localSearch;
        const MoveSelectorConfig& ms = ls.moveSelector;
        const int listCount = Descriptor::listCount(solution);
        const int elementCount = Descriptor::elementCount(solution);
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        // During local search every element is assigned, so the element index
        // gives a uniformly random (list, position) in O(1).
        int e1 = static_cast<int>(rng() % static_cast<std::uint64_t>(elementCount));
        int l1 = index_.listOf(e1), i1 = index_.positionOf(e1);

        // Union move selector: weighted pick among the applicable moves.
        const double wRelocate = ms.relocateMoveWeight;
        const double wSwap = ms.listSwapMoveWeight;
        const int size1 = Descriptor::listSize(solution, l1);
        const double wTwoOpt = size1 < 2 ? 0.0 : ms.twoOptMoveWeight;
        const double wOrOpt = elementCount < 3 ? 0.0 : ms.orOptMoveWeight;
        const double wTailSwap = listCount < 2 ? 0.0 : ms.tailSwapMoveWeight;
        const double wInterExchange =
                listCount < 2 ? 0.0 : ms.interExchangeMoveWeight;
        const double total =
                wRelocate + wSwap + wTwoOpt + wOrOpt + wTailSwap + wInterExchange;

        double dice = unit(rng) * total;
        AppliedMove move;
        if (dice < wRelocate || elementCount < 2 || total <= 0) {
            // Relocate: move one element; the target is next to a nearby
            // neighbor when a distance meter is configured, else uniform.
            int l2, i2;
            if (useNearby(rng) && !nearby_.of(e1).empty()) {
                int nb = randomOf(nearby_.of(e1), rng);
                l2 = index_.listOf(nb);
                int q = index_.positionOf(nb);
                if (l2 == l1 && q > i1) --q;  // nb shifts down once e1 is out
                i2 = q + 1;                   // insert right after the neighbor
            } else {
                l2 = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
                int size2 = Descriptor::listSize(solution, l2) - (l2 == l1 ? 1 : 0);
                i2 = static_cast<int>(rng() % static_cast<std::uint64_t>(size2 + 1));
            }
            move.list = ListMove::segment(l1, i1, 1, l2, i2, e1);
            move.entities[0] = e1;
            move.entityCount = 1;
        } else if (dice < wRelocate + wSwap) {
            // Swap two elements (possibly across lists); nearby-biased partner.
            int e2;
            if (useNearby(rng) && !nearby_.of(e1).empty()) {
                e2 = randomOf(nearby_.of(e1), rng);
                if (e2 == e1) e2 = randomOther(elementCount, e1, rng);
            } else {
                e2 = randomOther(elementCount, e1, rng);
            }
            int l2 = index_.listOf(e2), i2 = index_.positionOf(e2);
            move.list = ListMove::swap(l1, i1, l2, i2, e1, e2);
            move.entities[0] = e1;
            move.entities[1] = e2;
            move.entityCount = 2;
        } else if (dice < wRelocate + wSwap + wTwoOpt) {
            // 2-opt: reverse a segment within e1's list.
            int i2 = randomOther(size1, i1, rng);
            int first = std::min(i1, i2), last = std::max(i1, i2);
            move.list = ListMove::reverse(l1, first, last);
            move.entities[0] = Descriptor::getElement(solution, l1, first);
            move.entities[1] = Descriptor::getElement(solution, l1, last);
            move.entityCount = 2;
        } else if (dice < wRelocate + wSwap + wTwoOpt + wOrOpt) {
            // Or-opt: relocate a short segment (2-3 consecutive elements)
            // intact, nearby-biased like relocate.
            int segLen = 2 + static_cast<int>(rng() % 2);
            if (segLen > size1) segLen = size1;
            int start = std::min(i1, size1 - segLen);
            const int seg0 = Descriptor::getElement(solution, l1, start);
            int l2 = -1, i2 = 0;
            if (useNearby(rng) && !nearby_.of(seg0).empty()) {
                int nb = randomOf(nearby_.of(seg0), rng);
                const int nbList = index_.listOf(nb);
                int q = index_.positionOf(nb);
                const bool inSegment = nbList == l1 && q >= start && q < start + segLen;
                if (!inSegment) {  // neighbor not part of the segment
                    l2 = nbList;
                    if (l2 == l1 && q >= start + segLen) q -= segLen;
                    i2 = q + 1;
                }
            }
            if (l2 < 0) {
                l2 = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
                int size2 = Descriptor::listSize(solution, l2) - (l2 == l1 ? segLen : 0);
                i2 = static_cast<int>(rng() % static_cast<std::uint64_t>(size2 + 1));
            }
            move.list = ListMove::segment(l1, start, segLen, l2, i2, seg0);
            move.entities[0] = seg0;
            move.entities[1] = Descriptor::getElement(solution, l1, start + segLen - 1);
            move.entityCount = 2;
        } else if (dice < wRelocate + wSwap + wTwoOpt + wOrOpt + wTailSwap) {
            // 2-opt* (tail swap): exchange the tails of two lists — the
            // classic inter-route move. Cut l1 right after e1's position.
            int l2 = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount - 1));
            if (l2 >= l1) ++l2;
            int c1 = i1;
            int size2 = Descriptor::listSize(solution, l2);
            int c2 = static_cast<int>(rng() % static_cast<std::uint64_t>(size2 + 1));
            move.entities[0] = e1;
            move.entities[1] = c2 < size2 ? Descriptor::getElement(solution, l2, c2) : e1;
            move.entityCount = 2;
            move.list = ListMove::tailSwap(l1, c1, l2, c2);
        } else {
            // CROSS-exchange: swap a run starting at e1 with a run in another
            // list, the two runs sized independently. The partner list is
            // chosen through a nearby neighbour when a distance meter is
            // configured — exchanging with a spatially unrelated route is
            // almost never an improvement.
            int l2 = -1, i2 = 0;
            if (useNearby(rng) && !nearby_.of(e1).empty()) {
                const int nb = randomOf(nearby_.of(e1), rng);
                if (index_.listOf(nb) >= 0 && index_.listOf(nb) != l1) {
                    l2 = index_.listOf(nb);
                    i2 = index_.positionOf(nb);
                }
            }
            if (l2 < 0) {
                l2 = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount - 1));
                if (l2 >= l1) ++l2;
                const int size2 = Descriptor::listSize(solution, l2);
                i2 = size2 == 0 ? 0
                                : static_cast<int>(rng() %
                                                   static_cast<std::uint64_t>(size2));
            }
            const int size2 = Descriptor::listSize(solution, l2);
            const int maxSegment = std::max(1, ms.interExchangeMaxSegment);
            // One side may take zero elements, which degenerates to relocating
            // the other side's run — the cheap way to shorten a route.
            int len1 = static_cast<int>(rng() %
                                        static_cast<std::uint64_t>(maxSegment + 1));
            int len2 = static_cast<int>(rng() %
                                        static_cast<std::uint64_t>(maxSegment + 1));
            len1 = std::min(len1, size1 - i1);
            len2 = std::min(len2, size2 - i2);
            if (len1 <= 0 && len2 <= 0) {
                // e1 guarantees that this side has at least one available
                // element. Turn the zero/zero draw into a one-way segment
                // relocate instead of spending the step on a no-op.
                len1 = 1;
            }
            move.entities[0] = e1;
            move.entities[1] = len2 > 0 ? Descriptor::getElement(solution, l2, i2) : e1;
            move.entityCount = 2;
            move.list = ListMove::exchange(l1, i1, len1, l2, i2, len2);
        }
        return move;
    }

    // Ask the calculator for the score the sampled move would produce. False
    // when it offers no preview for this move: apply it and score it instead.
    bool previewListMove(ScoreDirector<Solution>& director, const AppliedMove& move,
                         HardSoftScore& out) {
        return ListMoves<Solution>::preview(director.calculator(), move.list, out);
    }

    // Score a sampled LIST move for the step and polish loops. Returns true
    // when the move is NOT applied on return: its preview was served and
    // `score` is that preview, so the caller applies the move only if it
    // accepts it. Returns false when the move IS applied on return (the
    // preview was declined, or FULL_ASSERT applied it to check the preview
    // against the applied score, throwing ScoreCorruptionError on a
    // mismatch) and `score` is the applied score; the caller undoes a
    // rejection. One evaluateWithPreview with no undo, so every preview in
    // the engine is verified by the same lines.
    bool scoreListMove(ScoreDirector<Solution>& director, const AppliedMove& move,
                       HardSoftScore& score) {
        bool applied = false;
        score = evaluateWithPreview(
                director,
                [&](IncrementalScoreCalculator<Solution>&, HardSoftScore& out) {
                    return previewListMove(director, move, out);
                },
                [&] { applyListMove(director, move); }, [] {}, /*served=*/nullptr, &applied);
        return !applied;  // the no-op undo left an applied move in place
    }

    // Apply a sampled LIST move; undoMove() inverts it.
    void applyListMove(ScoreDirector<Solution>& director, const AppliedMove& move) {
        ListMoves<Solution>::apply(director, index_, move.list);
    }

    void undoMove(ScoreDirector<Solution>& director, const AppliedMove& move) {
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            switch (move.kind) {
                case 0:  // change
                    director.changeVariable(move.a, move.b);
                    break;
                case 1:  // swap
                    director.beginVariableChanges();
                    director.changeVariable(move.a, move.b);
                    director.changeVariable(move.c, move.d);
                    director.endVariableChanges();
                    break;
            }
        } else {
            ListMoves<Solution>::undo(director, index_, move.list);
        }
    }

    // --- LNS: adaptive ruin & recreate ---

    // One ruin-and-recreate kick. The result becomes the new incumbent
    // unconditionally (perturbation-style); outcomes are still classified to
    // adapt the ruin-operator weights.
    HardSoftScore lnsIteration(ScoreDirector<Solution>& director, HardSoftScore currentScore,
                               const HardSoftScore& bestScore,
                               OperatorSelectionScheme& ruinSelector,
                               const std::vector<RuinOperator<Solution>*>& ruins,
                               OperatorContext<Solution>& operatorContext,
                               std::mt19937_64& rng) {
        Solution& solution = director.workingSolution();
        const RuinRecreateConfig& lns = config_.localSearch.ruinRecreate;
        const int n = GreedyRepair<Solution>::candidateCount(solution);
        std::uniform_real_distribution<double> fraction(lns.minRuinFraction,
                                                        lns.maxRuinFraction);
        int ruinCount = std::clamp(static_cast<int>(fraction(rng) * n), 1,
                                   std::min(n - 1, lns.maxRuinCount));

        int op = ruinSelector.select(rng);
        std::vector<int> ruined = ruins[op]->selectRuinSet(operatorContext, ruinCount, rng);
        // Defensive: a ruin set must be duplicate-free (the built-ins are by
        // construction; a custom operator is not required to be) and must not
        // contain a pinned entity. dedupe() is order-preserving, which is
        // load-bearing — see its comment.
        repair_.dedupe(director, ruined, n);
        repair_.ruin(director, ruined);
        repair_.recreate(director, ruined, rng);

        // Polish the recreated solution with hill-climbing small moves so the
        // outcome classification below reflects the kick's real potential.
        // LIST moves are previewed like the main step loop's: a rejected
        // polish move is never applied.
        HardSoftScore candidate = director.calculateScore();
        for (std::int64_t i = 0; i < lns.polishSteps; ++i) {
            AppliedMove move;
            HardSoftScore polished;
            bool pending = false;  // scored by preview, not applied
            if constexpr (Descriptor::kind == VariableKind::BASIC) {
                move = applyBasicMove(director, rng);
                polished = director.calculateScore();
            } else {
                move = sampleListMove(director, rng);
                pending = scoreListMove(director, move, polished);
            }
            if (polished >= candidate) {
                if constexpr (Descriptor::kind == VariableKind::LIST) {
                    if (pending) applyListMove(director, move);
                }
                candidate = polished;
            } else if (!pending) {
                undoMove(director, move);
            }
        }
        LnsOutcome outcome;
        if (candidate > bestScore) {
            outcome = LnsOutcome::NEW_BEST;
        } else if (candidate > currentScore) {
            outcome = LnsOutcome::BETTER;
        } else if (candidate == currentScore) {
            outcome = LnsOutcome::ACCEPTED;
        } else {
            outcome = LnsOutcome::REJECTED;  // kept anyway; scored for adaptation
        }
        ruinSelector.update(op, outcome);
        return candidate;
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_DEFAULT_LOCAL_SEARCH_PHASE_HPP
