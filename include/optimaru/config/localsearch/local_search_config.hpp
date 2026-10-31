#ifndef OPTIMARU_CONFIG_LOCALSEARCH_LOCAL_SEARCH_CONFIG_HPP
#define OPTIMARU_CONFIG_LOCALSEARCH_LOCAL_SEARCH_CONFIG_HPP

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "optimaru/config/heuristic/selector/common/nearby/nearby_selection_config.hpp"
#include "optimaru/config/heuristic/selector/move_selector_config.hpp"
#include "optimaru/config/localsearch/decider/acceptor/local_search_acceptor_config.hpp"
// Only the plugin point, never the umbrella: ruin_recreate_selector.hpp pulls
// in every scheme AND this config back, so including it here is a cycle. Which
// side of it wins depends on include order, so it built for some translation
// units and left OperatorSelectionType undefined for others.
#include "optimaru/impl/heuristic/selector/move/generic/select/operator_selection_scheme.hpp"

namespace optimaru {

// The adaptive ruin-and-recreate (ALNS) settings — the analog of upstream's
// RuinRecreateMoveSelector (impl.heuristic.selector.move.generic), with
// adaptive operator selection. After
// unimprovedStepTrigger steps without a new best, one kick runs: an
// adaptively selected ruin operator (random / related / worst, SISR and route
// ruins when enabled below, registered customs) removes a fraction of the
// solution, the repair (regret-k by default, see regretK) rebuilds it,
// polishSteps hill-climbing moves repair the seams, and the result becomes
// the new incumbent unconditionally with a fresh acceptor
// (perturbation-style; the best-solution snapshot is the safety net).
struct RuinRecreateConfig {
    bool enabled = false;
    std::int64_t unimprovedStepTrigger = 5000;
    double minRuinFraction = 0.10;
    double maxRuinFraction = 0.30;
    // Absolute cap on entities ruined per kick (Ropke & Pisinger's q <= ~60):
    // on large instances an uncapped fractional ruin (10-30% of thousands)
    // makes every kick expensive and unrecoverable within polishSteps, so the
    // unconditional-incumbent kick destroys more progress than it enables.
    int maxRuinCount = 60;
    // Operator adaptation. ROULETTE_WHEEL is w = decay*w + (1-decay)*reward,
    // rewards indexed by LnsOutcome (NEW_BEST / BETTER / ACCEPTED / REJECTED);
    // see ruin_recreate_selector.hpp for the other three schemes.
    OperatorSelectionType selectionType = OperatorSelectionType::ROULETTE_WHEEL;
    double operatorDecay = 0.8;
    std::vector<double> rewards{33.0, 9.0, 3.0, 0.0};
    // SEGMENTED_ROULETTE_WHEEL: iterations per weight-update segment.
    std::int64_t selectionSegmentLength = 100;
    // ALPHA_UCB: exploration weight; <= 0.1 is the usual choice.
    double selectionAlpha = 0.05;

    // Which ruin operators join the adaptive pool, on top of the always-on
    // random / related / worst trio. Both are LIST-kind strategies and fall
    // back to the random ruin on BASIC solutions (see their headers).
    bool sisrRuin = true;    // substring removal (Christiaens & Vanden Berghe)
    bool routeRuin = true;   // empty whole lists
    int sisrMaxStringRemovals = 3;
    int sisrMaxStringSize = 12;

    // Recreate settings. (The list the repair never places into, a virtual
    // unassigned pool, is LocalSearchConfig::unassignedList.)
    // regretK >= 2 switches the repair from plain cheapest-insertion to
    // regret-k: instead of taking elements in random order, repeatedly insert
    // the element with the largest gap between its best and its k-th best
    // placement — the one that will suffer most if it waits. Costs one extra
    // pass over the pending elements per insertion, and on time-windowed
    // instances routinely buys more than it costs. 0/1 = plain greedy.
    int regretK = 3;
    // SISR's "blinking": skip evaluating a candidate placement with this
    // probability, so the repair is not deterministically greedy and repeated
    // kicks on the same hole explore different rebuilds. 0 disables it.
    double blinkRate = 0.01;

    // Hill-climbing small moves polishing each kick before it becomes the
    // incumbent. NOT counted against stepCountLimit.
    std::int64_t polishSteps = 5000;

    // Destroy-and-repair as an ORDINARY move, in addition to the stagnation
    // kick above: a small ruin is applied, repaired, scored, and UNDONE if the
    // acceptor rejects it. Implemented as a move operator plugin
    // (strategy/ruin_recreate_move.hpp) drawn with this probability per step.
    // The candidate is judged by the normal acceptance criterion. The kick
    // above is the opposite — rare and unconditional.
    // 0 disables it, which is the default.
    double asMoveWeight = 0.0;
    // Ruin size for the as-a-move form. Much smaller than a kick's: it has to
    // be cheap enough to run every few steps and small enough that the
    // acceptor sees a local change rather than a restart.
    int asMoveMaxRuin = 12;
};

// Mirrors config.localsearch.LocalSearchPhaseConfig: acceptor + move mix +
// the guided/nearby selection hooks.
struct LocalSearchConfig {
    LocalSearchAcceptorConfig acceptor;
    MoveSelectorConfig moveSelector;

    // Optional guided move selection, BASIC kind only. Either hook may return
    // -1 to fall back to uniform-random selection for that draw.
    std::function<int(std::mt19937_64&)> entityRecommender;
    std::function<int(std::mt19937_64&, int entityIndex)> valueRecommender;

    NearbySelectionConfig nearby;
    RuinRecreateConfig ruinRecreate;

    // LIST kind: the list holding unplaced elements, a virtual unassigned
    // pool, or -1 for none. One field, read by every built-in that must know
    // the pool:
    //  - the ruin-and-recreate repair, and the iterated local search's,
    //    never place into it. Without that, a ruin on a tightly loaded
    //    instance ends with the elements that fit nowhere parked in the pool,
    //    a lexicographic loss the kick then makes the incumbent and one the
    //    ordinary moves never undo (nothing but a chain re-assigns). With it
    //    the leftover lands on the least overloaded list instead, a hard
    //    breach the next few relocations and swaps mend. Measured on CVRPLIB
    //    X-n289-k60 (99.7% loaded): with the pool open the search stalls at
    //    its first kick;
    //  - the ejection chain seeds half its chains from it while it is
    //    non-empty, and never moves anything into it;
    //  - the descents (Descent, GranularLocalSearch) neither reorder it nor
    //    move elements in or out of it: its order carries no score, and
    //    assigning or unassigning is the repair's and the chain's job, where
    //    hard feasibility is judged as a whole.
    int unassignedList = -1;

    // Upper gate for caller-registered MoveOperators. The adaptive selector
    // still chooses between the built-in mix and custom operators inside the
    // gate; outside it, the ordinary move mix runs directly. This prevents an
    // expensive macro move with one recent reward from monopolizing the clock.
    // 1 preserves the original unrestricted behavior.
    double customMoveProbability = 1.0;

    // ---------------------------------------------------------------------
    // Built-in search strategies
    // ---------------------------------------------------------------------
    // Neighbourhoods too expensive to fire every step, and too useful to leave
    // out. Each is a MoveOperator under impl/localsearch/strategy/, one file
    // per strategy, implementing the same interface a caller gets from
    // Solver::addMoveOperator(); these weights are the probability that a step
    // draws that plugin instead of the ordinary move mix.
    //
    // WHY A WEIGHT AND NOT ADAPTIVE SELECTION. The obvious next step is to drop
    // these numbers into the adaptive pool that already weights the ruin
    // operators and let it learn them. That was tried and measured, and it
    // loses badly (RC2_10_5, 1000 customers, 120s, vs best known: +8.1% at
    // these weights, +12.9% adapting on reward-per-evaluation, +16.6% also
    // dividing the draw probability by measured cost). The reason is
    // structural, not a tuning miss: RouletteWheel adapts an EMA of the reward
    // with operatorDecay, whose default 0.8 remembers about five applications.
    // That is the right memory for the ruin pool — a handful of similar-cost
    // operators, each applied at a kick and each succeeding often enough to
    // register. The move pool is the opposite: the ordinary move mix improves
    // roughly once in 10^5 draws late in a large solve, so its EMA sits at zero
    // essentially always, and ANY operator with a non-zero recent reward
    // outbids it — however much of the clock it burns. Weighting the move pool
    // adaptively needs a scheme that can represent a rare-success operator's
    // long-run rate, which is a change to the selection schemes rather than to
    // the strategies here.
    //
    // descent: best-improvement over one nearby pair, repeated until that pair
    // is locally optimal. The neighbourhood is per kind — LIST: two
    // geometrically nearby routes, every relocation between them, both
    // directions; BASIC: a nearby entity pair, every value of each entity plus
    // the swap of the two. See strategy/descent.hpp.
    //
    // What the right weight is depends entirely on how big that neighbourhood
    // is, and for BASIC that is the size of the VALUE RANGE — one sweep costs
    // ~2 * valueCount score evaluations against an ordinary step's one. With a
    // 40-value range (an employee-rostering domain) a percent-scale weight
    // spends a sensible slice of the budget and pays for itself; with a 40x25
    // slot x room grid (school-timetabling) the same weight spends over 90% of
    // the budget on descent and starves the move mix. Weight by the value
    // range, and measure.
    double descentWeight = 0.0;
    // Give up on a neighbourhood after this many improving passes, so one
    // application cannot run away on a large pair.
    int descentMaxSweeps = 4;

    // LIST-only intensification. When enabled, descent evaluates the
    // granular neighbourhood around one nearby route pair instead of only
    // relocate moves: Exchange(1,0), Exchange(1,1), 2-opt, 2-opt* and small
    // CROSS-exchange moves. This is intentionally opt-in because a full
    // route-pair neighbourhood is much more expensive than the BASIC
    // entity/value descent used by rostering and timetabling.
    bool descentListAdvanced = false;
    int descentListMaxSegment = 2;
    int descentListCandidateRoutes = 4;

    // routeElimination: empty the smallest list into the others in one atomic
    // move, so the acceptor sees the net effect of losing its depot legs
    // rather than the uphill intermediates that relocating one element at a
    // time creates. See strategy/route_elimination.hpp.
    double routeEliminationWeight = 0.0;
    // Do not attempt lists longer than this — beyond a certain size the
    // reinsertion is both expensive and hopeless.
    int routeEliminationMaxSize = 12;

    // ejectionChain: break a hard constraint on purpose (insert an element of
    // the source list, or take the best-soft relocation that is infeasible),
    // then apply strictly hard-improving relocations and swaps until the hard
    // score is back, and let the acceptor judge the whole chain. This reaches
    // the slack shifts a tightly packed instance needs, every one of which is
    // uphill alone. See strategy/ejection_chain.hpp.
    double ejectionChainWeight = 0.0;
    // Give up (and undo) after this many repair operations.
    int ejectionChainMaxSteps = 24;
    // Lists searched per element during the repair: those of its nearest
    // neighbours, in nearby order.
    int ejectionChainCandidateLists = 4;
    // Score evaluations one chain may spend, seed and repair together; a
    // chain that runs out is undone like one that stalls. This, not the step
    // cap, is what bounds the cost of a chain on long lists (candidate
    // collection between evaluations is not counted, but it is bounded by
    // the nearby size and ejectionChainCandidateLists).
    std::int64_t ejectionChainMaxEvaluations = 50000;

    // iteratedLocalSearch (LIST only): one ILS iteration as a move — a small
    // ruin from the adaptive ruin pool, the greedy repair, then a granular
    // first-improvement descent (relocate, swap, 2-opt, 2-opt*, Or-opt, CROSS
    // around every element's nearby neighbours) until the touched lists are
    // locally optimal. The acceptor judges the local optimum. The first
    // iteration only descends, so the construction result is a local optimum
    // before anything is perturbed. Needs nearby selection and a calculator
    // that serves previews; it declines every draw otherwise. See
    // strategy/iterated_local_search.hpp.
    double iteratedLocalSearchWeight = 0.0;
    // Elements ruined per iteration, uniform in [min, max].
    int iteratedLocalSearchMinRuin = 5;
    int iteratedLocalSearchMaxRuin = 15;
    // Longest Or-opt segment the descent relocates (1 disables Or-opt).
    int iteratedLocalSearchMaxSegment = 3;
    // Evaluations one descent may spend before it stops where it is. At O(1)
    // previews five million evaluations is on the order of a quarter second;
    // where a preview costs O(route) (a decomposed assembly, a plan with
    // waiting) the descent instead consults the solver's termination every
    // thousand evaluations and stops at the time limit, so this cap is the
    // only bound just for a solve without a wall-clock limit. Lower caps
    // truncate the descents
    // that matter: with one million, CVRPLIB X-n895-k37 (37 routes of ~24
    // stops) goes from 4.3 / 5.7% to 6.1 / 6.0% gap at 60 seconds.
    std::int64_t iteratedLocalSearchMaxEvaluations = 5000000;
};

}  // namespace optimaru

#endif  // OPTIMARU_CONFIG_LOCALSEARCH_LOCAL_SEARCH_CONFIG_HPP
