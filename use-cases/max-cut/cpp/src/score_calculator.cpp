#include "mc/score_calculator.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "mc/constraints/cut_weight_cost.hpp"
#include "mc/constraints/different_partition_pairs_cost.hpp"
#include "mc/constraints/fixed_partition_cost.hpp"
#include "mc/constraints/partition_imbalance_cost.hpp"
#include "mc/constraints/partition_size_bounds_cost.hpp"
#include "mc/constraints/same_partition_pairs_cost.hpp"

namespace mc
{

namespace
{

constexpr auto kFixedWeight = static_cast<std::int64_t>(FixedPartitionCost::kDefaultWeight);
constexpr auto kSameWeight = static_cast<std::int64_t>(SamePartitionPairsCost::kDefaultWeight);
constexpr auto kDifferentWeight = static_cast<std::int64_t>(DifferentPartitionPairsCost::kDefaultWeight);
constexpr auto kSizeWeight = static_cast<std::int64_t>(PartitionSizeBoundsCost::kDefaultWeight);
constexpr auto kImbalanceWeight = static_cast<std::int64_t>(PartitionImbalanceCost::kDefaultWeight);
constexpr auto kCutWeight = static_cast<std::int64_t>(CutWeightCost::kDefaultWeight);
static_assert(kFixedWeight == FixedPartitionCost::kDefaultWeight
                  && kSameWeight == SamePartitionPairsCost::kDefaultWeight
                  && kDifferentWeight == DifferentPartitionPairsCost::kDefaultWeight
                  && kSizeWeight == PartitionSizeBoundsCost::kDefaultWeight
                  && kImbalanceWeight == PartitionImbalanceCost::kDefaultWeight
                  && kCutWeight == CutWeightCost::kDefaultWeight,
              "the monolithic calculator assumes integral default weights");
static_assert(FixedPartitionCost::kDefaultLevel == optimaru::Level::Hard
                  && SamePartitionPairsCost::kDefaultLevel == optimaru::Level::Hard
                  && DifferentPartitionPairsCost::kDefaultLevel == optimaru::Level::Hard
                  && PartitionSizeBoundsCost::kDefaultLevel == optimaru::Level::Hard
                  && PartitionImbalanceCost::kDefaultLevel == optimaru::Level::Soft
                  && CutWeightCost::kDefaultLevel == optimaru::Level::Soft,
              "the monolithic calculator assumes four hard constraints, then two soft ones");

[[noreturn]] void invalid(const std::string &message)
{
    throw std::invalid_argument(message);
}

void requireVertex(const MaxCutProblem &problem, int index, const std::string &where)
{
    if (index < 0 || index >= problem.vertexCount())
    {
        invalid(where + ": vertex index " + std::to_string(index) + " is out of range (0.."
                + std::to_string(problem.vertexCount() - 1) + ")");
    }
}

void requireSideVector(const MaxCutProblem &problem, const std::vector<int> &values, const char *name)
{
    if (values.size() != problem.vertexIds.size())
    {
        invalid(std::string(name) + " has " + std::to_string(values.size()) + " entries for "
                + std::to_string(problem.vertexCount()) + " vertices");
    }
    for (std::size_t v = 0; v < values.size(); ++v)
    {
        if (values[v] < kUnassigned || values[v] >= kSideCount)
        {
            invalid(std::string(name) + " of vertex '" + problem.vertexIds[v] + "' must be -1, 0 or 1, not "
                    + std::to_string(values[v]));
        }
    }
}

// CSR from per-vertex lists.
void toCsr(const std::vector<std::vector<int>> &lists, std::vector<int> &start, std::vector<int> &flat)
{
    start.assign(lists.size() + 1, 0);
    flat.clear();
    for (std::size_t v = 0; v < lists.size(); ++v)
    {
        flat.insert(flat.end(), lists[v].begin(), lists[v].end());
        start[v + 1] = static_cast<int>(flat.size());
    }
}

}  // namespace

void ScoreCalculator::prepare(MaxCutProblem &problem)
{
    const int n = problem.vertexCount();
    {
        std::unordered_set<std::string> ids;
        for (const std::string &id : problem.vertexIds)
        {
            if (!ids.insert(id).second)
                invalid("vertex '" + id + "' is listed more than once");
        }
    }
    if (problem.side.empty())
        problem.side.assign(n, kUnassigned);
    if (problem.fixedSide.empty())
        problem.fixedSide.assign(n, kUnassigned);
    requireSideVector(problem, problem.side, "side");
    requireSideVector(problem, problem.fixedSide, "fixed side");
    const int minimum = problem.minSize();
    const int maximum = problem.max_size();
    if (minimum < 0 || maximum < 0 || minimum > maximum || maximum > n)
    {
        invalid("partition size bounds must satisfy 0 <= min <= max <= vertex count (" + std::to_string(n)
                + "), not min " + std::to_string(minimum) + " / max " + std::to_string(maximum));
    }

    std::vector<std::vector<int>> adjacency(n);
    std::vector<std::vector<std::int64_t>> adjacencyWeight(n);
    for (std::size_t e = 0; e < problem.edges.size(); ++e)
    {
        const Edge &edge = problem.edges[e];
        const std::string where = "edge " + std::to_string(e);
        requireVertex(problem, edge.source, where);
        requireVertex(problem, edge.target, where);
        if (edge.weight < 0)
            invalid(where + ": weight must be >= 0, not " + std::to_string(edge.weight));
        if (edge.source == edge.target)
            continue;  // a self-loop is never cut
        adjacency[edge.source].push_back(edge.target);
        adjacencyWeight[edge.source].push_back(edge.weight);
        adjacency[edge.target].push_back(edge.source);
        adjacencyWeight[edge.target].push_back(edge.weight);
    }
    toCsr(adjacency, problem.adjStart, problem.adjVertex);
    problem.adjWeight.clear();
    for (const auto &weights : adjacencyWeight)
        problem.adjWeight.insert(problem.adjWeight.end(), weights.begin(), weights.end());

    std::vector<std::vector<int>> asFirst(n);
    std::vector<std::vector<int>> asSecond(n);
    for (std::size_t p = 0; p < problem.samePartitionPairs.size(); ++p)
    {
        const VertexPair &pair = problem.samePartitionPairs[p];
        const std::string where = "same-partition pair " + std::to_string(p);
        requireVertex(problem, pair.first, where);
        requireVertex(problem, pair.second, where);
        if (pair.first == pair.second)
            continue;  // a self-pair is never split
        asFirst[pair.first].push_back(pair.second);
        asSecond[pair.second].push_back(pair.first);
    }
    toCsr(asFirst, problem.sameAsFirstStart, problem.sameAsFirstPartner);
    toCsr(asSecond, problem.sameAsSecondStart, problem.sameAsSecondPartner);

    std::vector<std::vector<int>> different(n);
    problem.diffSelfCount.assign(n, 0);
    for (std::size_t p = 0; p < problem.differentPartitionPairs.size(); ++p)
    {
        const VertexPair &pair = problem.differentPartitionPairs[p];
        const std::string where = "different-partition pair " + std::to_string(p);
        requireVertex(problem, pair.first, where);
        requireVertex(problem, pair.second, where);
        if (pair.first == pair.second)
        {
            ++problem.diffSelfCount[pair.first];  // always joined
            continue;
        }
        different[pair.first].push_back(pair.second);
        different[pair.second].push_back(pair.first);
    }
    toCsr(different, problem.diffStart, problem.diffPartner);
    problem.prepared = true;
}

RawCosts ScoreCalculator::rawCosts(const MaxCutProblem &problem)
{
    RawCosts raw;
    const int n = problem.vertexCount();
    int count[kSideCount] = {0, 0};
    for (int v = 0; v < n; ++v)
    {
        const int s = problem.sideOf(v);
        if (s < 0)
            continue;
        ++count[s];
        const int fixed = static_cast<std::size_t>(v) < problem.fixedSide.size() ? problem.fixedSide[v] : -1;
        if (fixed >= 0 && fixed != s)
            ++raw.fixed[s];
    }
    for (const VertexPair &pair : problem.samePartitionPairs)
    {
        const int a = problem.sideOf(pair.first);
        if (a >= 0 && problem.sideOf(pair.second) != a)
            ++raw.same[a];
    }
    for (const VertexPair &pair : problem.differentPartitionPairs)
    {
        const int a = problem.sideOf(pair.first);
        if (a >= 0 && problem.sideOf(pair.second) == a)
            ++raw.different[a];
    }
    for (int s = 0; s < kSideCount; ++s)
        raw.size[s] = std::max(0, problem.minSize() - count[s]) + std::max(0, count[s] - problem.max_size());
    raw.imbalance = std::abs(count[0] - count[1]);
    raw.cut = problem.cutWeight();
    return raw;
}

Score ScoreCalculator::scoreOf(const RawCosts &raw)
{
    Score score;
    score.hard = -(kFixedWeight * raw.fixedTotal() + kSameWeight * raw.sameTotal()
                   + kDifferentWeight * raw.differentTotal() + kSizeWeight * raw.sizeTotal());
    // cutWeightCost's raw cost is -cut: -(weight * -cut) = +weight * cut.
    score.soft = static_cast<double>(kCutWeight * raw.cut - kImbalanceWeight * raw.imbalance);
    return score;
}

Score ScoreCalculator::score(MaxCutProblem &problem)
{
    prepare(problem);
    problem.score = scoreOf(rawCosts(problem));
    problem.scored = true;
    return problem.score;
}

}  // namespace mc
