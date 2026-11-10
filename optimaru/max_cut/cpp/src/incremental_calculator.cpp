#include "mc/incremental_calculator.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#include "mc/constraints/cut_weight_cost.hpp"
#include "mc/constraints/different_partition_pairs_cost.hpp"
#include "mc/constraints/fixed_partition_cost.hpp"
#include "mc/constraints/partition_imbalance_cost.hpp"
#include "mc/constraints/partition_size_bounds_cost.hpp"
#include "mc/constraints/same_partition_pairs_cost.hpp"

namespace mc {

namespace {

constexpr auto kFixedWeight = static_cast<std::int64_t>(FixedPartitionCost::kDefaultWeight);
constexpr auto kSameWeight = static_cast<std::int64_t>(SamePartitionPairsCost::kDefaultWeight);
constexpr auto kDifferentWeight =
        static_cast<std::int64_t>(DifferentPartitionPairsCost::kDefaultWeight);
constexpr auto kSizeWeight = static_cast<std::int64_t>(PartitionSizeBoundsCost::kDefaultWeight);
constexpr auto kImbalanceWeight = static_cast<std::int64_t>(PartitionImbalanceCost::kDefaultWeight);
constexpr auto kCutWeight = static_cast<std::int64_t>(CutWeightCost::kDefaultWeight);
static_assert(kFixedWeight == FixedPartitionCost::kDefaultWeight &&
                      kSameWeight == SamePartitionPairsCost::kDefaultWeight &&
                      kDifferentWeight == DifferentPartitionPairsCost::kDefaultWeight &&
                      kSizeWeight == PartitionSizeBoundsCost::kDefaultWeight &&
                      kImbalanceWeight == PartitionImbalanceCost::kDefaultWeight &&
                      kCutWeight == CutWeightCost::kDefaultWeight,
              "the monolithic calculator assumes integral default weights");

}  // namespace

void MaxCutIncrementalScoreCalculator::rebuild(MaxCutProblem& solution) {
    if (!solution.prepared) {
        throw std::logic_error(
                "MaxCutIncrementalScoreCalculator requires "
                "ScoreCalculator::prepare(problem) first");
    }
    problem_ = &solution;
    const int n = solution.vertexCount();
    side_.assign(n, kUnassigned);
    count_[0] = count_[1] = 0;
    fixed_ = sameSplit_ = diffJoined_ = cut_ = 0;
    // Start from "everything unassigned" (every total 0) and assign the
    // vertices one at a time through the same transition as a move, so every
    // pair and edge term is counted exactly once.
    for (int v = 0; v < n; ++v) {
        const int now = solution.side[v];
        if (now == kUnassigned) continue;
        apply(v, -1);
        side_[v] = now;
        apply(v, +1);
    }
}

void MaxCutIncrementalScoreCalculator::before_assign(int /*vertex*/) {}

void MaxCutIncrementalScoreCalculator::after_assign(int vertex) {
    const int now = problem_->side[vertex];
    if (now == side_[vertex]) return;
    apply(vertex, -1);
    side_[vertex] = now;
    apply(vertex, +1);
}

void MaxCutIncrementalScoreCalculator::apply(int v, int sign) {
    const MaxCutProblem& p = *problem_;
    const int s = side_[v];
    if (s >= 0) {
        count_[s] += sign;
        const int fixed = p.fixedSide[v];
        if (fixed >= 0 && fixed != s) fixed_ += sign;
        diffJoined_ += sign * p.diffSelfCount[v];
    }
    // Same-side pairs (v, w): split while v is assigned and w is not on v's
    // side.
    if (s >= 0) {
        for (int k = p.sameAsFirstStart[v]; k < p.sameAsFirstStart[v + 1]; ++k) {
            if (side_[p.sameAsFirstPartner[k]] != s) sameSplit_ += sign;
        }
    }
    // Same-side pairs (w, v): split while w is assigned and v is not on w's
    // side.
    for (int k = p.sameAsSecondStart[v]; k < p.sameAsSecondStart[v + 1]; ++k) {
        const int w = side_[p.sameAsSecondPartner[k]];
        if (w >= 0 && w != s) sameSplit_ += sign;
    }
    if (s < 0) return;
    // Different-side pairs (v, w) / (w, v): joined while both are on one side.
    for (int k = p.diffStart[v]; k < p.diffStart[v + 1]; ++k) {
        if (side_[p.diffPartner[k]] == s) diffJoined_ += sign;
    }
    // Edges (v, w): cut while both are assigned and on different sides.
    for (int k = p.adjStart[v]; k < p.adjStart[v + 1]; ++k) {
        const int w = side_[p.adjVertex[k]];
        if (w >= 0 && w != s) cut_ += sign * p.adjWeight[k];
    }
}

MaxCutIncrementalScoreCalculator::Totals MaxCutIncrementalScoreCalculator::totals() const {
    const MaxCutProblem& p = *problem_;
    Totals totals;
    totals.fixed = fixed_;
    totals.same = sameSplit_;
    totals.different = diffJoined_;
    for (int s = 0; s < kSideCount; ++s) {
        const std::int64_t n = count_[s];
        totals.size[s] = std::max<std::int64_t>(0, p.minSize() - n) +
                         std::max<std::int64_t>(0, n - p.max_size());
    }
    totals.imbalance = std::llabs(count_[0] - count_[1]);
    totals.cut = cut_;
    return totals;
}

Score MaxCutIncrementalScoreCalculator::score() {
    const Totals t = totals();
    Score score;
    score.hard = -(kFixedWeight * t.fixed + kSameWeight * t.same + kDifferentWeight * t.different +
                   kSizeWeight * (t.size[0] + t.size[1]));
    score.soft = static_cast<double>(kCutWeight * t.cut - kImbalanceWeight * t.imbalance);
    return score;
}

}  // namespace mc
