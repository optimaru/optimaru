#ifndef MC_DOMAIN_HPP
#define MC_DOMAIN_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace mc
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The engine's HardSoftScore, so the mapping is the identity and there is no
// medium level (no optimaru::MediumFoldOf specialization). At the default
// weights (the components' kDefaultWeight, mc/constraints/*.hpp):
//   hard = -(fixed vertices off their side + split same-side pairs
//            + joined different-side pairs + side-size shortfalls/excesses)
//   soft = +(cut weight)          // cutWeightCost is a reward
//          (partitionImbalanceCost is opt-in: default weight 0)
// The problem carries no weights. Every level is integral; soft is stored in
// a double, exact below 2^53.
using Score = optimaru::HardSoftScore;

// The planning variable's values: side 0 or side 1. -1 exists only
// transiently (the engine's ruin-and-recreate) or in a problem a C++ caller
// builds; see "Unassigned vertices" below.
constexpr int kUnassigned = -1;
constexpr int kSideCount = 2;

// ---------------------------------------------------------------------------
// Edge / VertexPair  (problem facts)
// ---------------------------------------------------------------------------
// An undirected weighted edge between two vertex indices. A self-loop is
// allowed and never cut; parallel edges add up. weight >= 0.
struct Edge
{
    int source = -1;
    int target = -1;
    std::int64_t weight = 1;
};

// Two vertex indices that must end up on the same side
// (samePartitionPairs) or on different sides (differentPartitionPairs). A
// pair may repeat (each entry counts) and may name one vertex twice (a same
// self-pair is never split, a different self-pair is always joined).
struct VertexPair
{
    int first = -1;
    int second = -1;
};

// ---------------------------------------------------------------------------
// MaxCutProblem  (@PlanningSolution; the vertices are the entities)
// ---------------------------------------------------------------------------
// Unassigned vertices (side == -1) follow one rule everywhere — the
// monolithic calculators, the plugin components and Python's raw _core
// functions (the package-level calculate_score / constraint_costs refuse a
// partial partition instead):
//   * it is in no side: it adds nothing to the cut, the sizes, the imbalance,
//     fixedPartitionCost or differentPartitionPairsCost;
//   * a same-side pair is charged while its FIRST endpoint is assigned and
//     its second endpoint is not on the same side (so an unassigned second
//     endpoint splits the pair, an unassigned first endpoint charges
//     nothing).
// On a complete partition every rule is the 0.1.x one.
struct MaxCutProblem
{
    // --- problem facts -----------------------------------------------------
    std::vector<std::string> vertexIds;  // index = entity
    std::vector<Edge> edges;
    // Per vertex: -1 = free, 0 / 1 = pinned to that side (the engine never
    // moves a pinned vertex). Empty = nothing pinned (prepare() sizes it).
    std::vector<int> fixedSide;
    std::vector<VertexPair> samePartitionPairs;
    std::vector<VertexPair> differentPartitionPairs;
    // Inclusive bounds on the size of EACH side; nullopt = 0 / vertexCount().
    // Both set and equal ("exact size") switches the search to swaps only.
    std::optional<int> minPartitionSize;
    std::optional<int> maxPartitionSize;

    // --- planning variable -------------------------------------------------
    // Per vertex: 0 / 1, or -1 = unassigned. Empty = every vertex unassigned
    // (prepare() sizes it).
    std::vector<int> side;
    // true: the next solve starts from `side` (only the -1 entries take the
    // default start). false: the solve ignores `side`. Every solve resets it
    // to false, so a seed is used once.
    bool warmStart = false;

    // No constraint weights here: the defaults are the components'
    // kDefaultWeight, and a caller re-weights through a constraint assembly.

    // --- result ------------------------------------------------------------
    Score score;
    bool scored = false;  // set by ScoreCalculator::calculateScore and solve

    // --- derived data, rebuilt by ScoreCalculator::prepare ----------------
    // Every entry point (solve, calculateScore, the Python constraint_costs)
    // re-runs prepare, so a problem edited after a solve is never scored
    // against stale indexes. The incremental calculator and the components
    // read these.
    bool prepared = false;
    // CSR adjacency without self-loops (each parallel edge once per
    // endpoint): neighbours of v are adjVertex[adjStart[v] .. adjStart[v+1]).
    std::vector<int> adjStart;
    std::vector<int> adjVertex;
    std::vector<std::int64_t> adjWeight;
    // Same-side pairs without self-pairs: the partners of v where v is the
    // pair's first endpoint, and where v is its second endpoint.
    std::vector<int> sameAsFirstStart;
    std::vector<int> sameAsFirstPartner;
    std::vector<int> sameAsSecondStart;
    std::vector<int> sameAsSecondPartner;
    // Different-side pairs without self-pairs: the partner of v in every pair
    // naming v (both directions), and the number of (v, v) self-pairs.
    std::vector<int> diffStart;
    std::vector<int> diffPartner;
    std::vector<int> diffSelfCount;

    int vertexCount() const { return static_cast<int>(vertexIds.size()); }
    int minSize() const { return minPartitionSize.value_or(0); }
    int maxSize() const { return maxPartitionSize.value_or(vertexCount()); }
    // The 0.1.x rule: both bounds given and equal.
    bool exactSize() const
    {
        return minPartitionSize.has_value() && maxPartitionSize.has_value()
               && *minPartitionSize == *maxPartitionSize;
    }
    bool isPinned(int vertex) const
    {
        return static_cast<std::size_t>(vertex) < fixedSide.size() && fixedSide[vertex] >= 0;
    }
    int movableCount() const
    {
        int movable = 0;
        for (int v = 0; v < vertexCount(); ++v)
            movable += isPinned(v) ? 0 : 1;
        return movable;
    }
    int sideSize(int s) const
    {
        int count = 0;
        for (int value : side)
            count += value == s ? 1 : 0;
        return count;
    }
    // The weight of the edges with both endpoints assigned and on different
    // sides (straight from the facts; no prepare needed).
    std::int64_t cutWeight() const
    {
        std::int64_t cut = 0;
        for (const Edge &edge : edges)
        {
            const int a = sideOf(edge.source);
            const int b = sideOf(edge.target);
            if (a >= 0 && b >= 0 && a != b)
                cut += edge.weight;
        }
        return cut;
    }
    std::int64_t totalEdgeWeight() const
    {
        std::int64_t total = 0;
        for (const Edge &edge : edges)
            total += edge.weight;
        return total;
    }
    bool isComplete() const
    {
        if (side.size() != vertexIds.size())
            return vertexIds.empty();
        for (int value : side)
        {
            if (value < 0)
                return false;
        }
        return true;
    }

    int sideOf(int vertex) const
    {
        return static_cast<std::size_t>(vertex) < side.size() ? side[vertex] : kUnassigned;
    }
};

}  // namespace mc

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization: entities are the vertices, the planning
// variable is MaxCutProblem::side, the value range is {0, 1} (unit = value =
// side). With two values the engine's change move is a flip and its swap
// move exchanges a side-0 and a side-1 vertex (the side sizes stay). A
// vertex with fixedSide >= 0 is pinned: never moved, swapped or ruined.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<mc::MaxCutProblem>
{
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::BASIC;
    static int entityCount(const mc::MaxCutProblem &problem) { return problem.vertexCount(); }
    static int valueCount(const mc::MaxCutProblem & /*problem*/) { return mc::kSideCount; }
    static int getVariable(const mc::MaxCutProblem &problem, int vertex) { return problem.side[vertex]; }
    static void setVariable(mc::MaxCutProblem &problem, int vertex, int value) { problem.side[vertex] = value; }
    static bool isPinned(const mc::MaxCutProblem &problem, int vertex) { return problem.isPinned(vertex); }
};

#endif  // MC_DOMAIN_HPP
