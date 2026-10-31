#ifndef OPTIMARU_IMPL_CONSTRUCTIONHEURISTIC_DEFAULT_CONSTRUCTION_HEURISTIC_PHASE_HPP
#define OPTIMARU_IMPL_CONSTRUCTIONHEURISTIC_DEFAULT_CONSTRUCTION_HEURISTIC_PHASE_HPP

#include <algorithm>
#include <functional>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/config/constructionheuristic/construction_heuristic_config.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// DefaultConstructionHeuristicPhase
// ---------------------------------------------------------------------------
// Mirrors impl.constructionheuristic.DefaultConstructionHeuristicPhase:
// First Fit (Decreasing) — assign each uninitialized entity to the placement
// with the best score at that point. BASIC: best value; LIST: cheapest
// insertion (best list + position). The placement helpers are also reused by
// the ruin-and-recreate repair (upstream's RuinRecreateConstructionHeuristic-
// Phase does the same).
template <typename Solution>
struct DefaultConstructionHeuristicPhase {
    using Descriptor = SolutionDescriptor<Solution>;

    static void run(ScoreDirector<Solution>& director, const ConstructionHeuristicConfig& config) {
        Solution& solution = director.workingSolution();
        if constexpr (Descriptor::kind == VariableKind::BASIC) {
            const int entityCount = Descriptor::entityCount(solution);
            const int valueCount = Descriptor::valueCount(solution);
            if (valueCount == 0) return;
            std::vector<int> uninitialized;
            for (int e = 0; e < entityCount; ++e) {
                if (Descriptor::getVariable(solution, e) < 0 &&
                    !entityPinned<Descriptor>(solution, e)) {
                    uninitialized.push_back(e);
                }
            }
            for (int e : initializationOrder(uninitialized, config.entityDifficulty)) {
                placeBestValue(director, e, valueCount);
            }
        } else {
            const int listCount = Descriptor::listCount(solution);
            const int elementCount = Descriptor::elementCount(solution);
            if (listCount == 0) return;
            std::vector<char> assigned(elementCount, 0);
            for (int l = 0; l < listCount; ++l) {
                for (int p = 0; p < Descriptor::listSize(solution, l); ++p) {
                    assigned[Descriptor::getElement(solution, l, p)] = 1;
                }
            }
            std::vector<int> unassigned;
            for (int e = 0; e < elementCount; ++e) {
                if (!assigned[e]) unassigned.push_back(e);
            }
            for (int e : initializationOrder(unassigned, config.entityDifficulty)) {
                placeCheapestInsertionFullScan(director, e);
            }
        }
    }

    // Initialization order: decreasing difficulty if a hook is set (FFD),
    // otherwise index order (First Fit).
    static std::vector<int> initializationOrder(
            const std::vector<int>& candidates,
            const std::function<double(int)>& entityDifficulty) {
        std::vector<int> order = candidates;
        if (entityDifficulty) {
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                return entityDifficulty(a) > entityDifficulty(b);
            });
        }
        return order;
    }

    // BASIC: assign entity e to its best value.
    static void placeBestValue(ScoreDirector<Solution>& director, int e, int valueCount) {
        HardSoftScore bestScore;
        bool bestInitialized = false;
        int bestValue = 0;
        for (int v = 0; v < valueCount; ++v) {
            director.changeVariable(e, v);
            HardSoftScore candidate = director.calculateScore();
            if (!bestInitialized || candidate > bestScore) {
                bestScore = candidate;
                bestInitialized = true;
                bestValue = v;
            }
        }
        director.changeVariable(e, bestValue);
    }

    // LIST: insert element e at the best position over ALL (list, position)
    // pairs. Does not touch the local-search element index.
    static void placeCheapestInsertionFullScan(ScoreDirector<Solution>& director, int e) {
        Solution& solution = director.workingSolution();
        const int listCount = Descriptor::listCount(solution);
        HardSoftScore bestScore;
        bool bestInitialized = false;
        int bestList = 0, bestPos = 0;
        for (int l = 0; l < listCount; ++l) {
            const int size = Descriptor::listSize(solution, l);
            for (int p = 0; p <= size; ++p) {
                director.insertListElement(l, p, e);
                HardSoftScore candidate = director.calculateScore();
                director.removeListElement(l, p);
                if (!bestInitialized || candidate > bestScore) {
                    bestScore = candidate;
                    bestInitialized = true;
                    bestList = l;
                    bestPos = p;
                }
            }
        }
        director.insertListElement(bestList, bestPos, e);
    }
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_CONSTRUCTIONHEURISTIC_DEFAULT_CONSTRUCTION_HEURISTIC_PHASE_HPP
