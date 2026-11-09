#ifndef MC_CONSTRAINTS_CUT_WEIGHT_COST_HPP
#define MC_CONSTRAINTS_CUT_WEIGHT_COST_HPP

#include <cstdint>
#include <vector>

#include "mc/constraints/common.hpp"

namespace mc
{

// Soft, weight 1, a REWARD: cutWeightCost — the objective. Raw cost =
// -(total weight of the edges whose endpoints are on different sides), one
// total, so it is <= 0 and a larger cut scores better (soft = +cut at the
// default weight); 0 means no edge is cut, the worst value, not "no
// violation". A self-loop is never cut; parallel edges add up; an edge with
// an unassigned endpoint is not cut. It is never hard (reward,
// level_flippable = false: hard() / set_level(Hard) throw). The shaping is
// sign-preserving, so the exponent semantics are those of 0.1.x.
//
// Keeps its own copy of the sides; a join or leave of vertex e walks e's CSR
// adjacency, O(degree): an edge (e, w) is cut while both are assigned and on
// different sides.
struct CutWeightCost final : detail::MemberChangeConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit CutWeightCost(double weight = kDefaultWeight)
        : detail::MemberChangeConstraint("cutWeightCost", weight, kDefaultLevel)
    {
        reward = true;  // raw cost <= 0; level='hard' refused everywhere
        level_flippable = false;
    }

    void reset(const MaxCutProblem &problem, const std::vector<std::vector<int>> &members_per_unit) override
    {
        detail::requirePrepared(problem);
        side_.assign(problem.vertexIds.size(), kUnassigned);
        for (int s = 0; s < static_cast<int>(members_per_unit.size()); ++s)
        {
            for (int v : members_per_unit[s])
                side_[v] = s;
        }
        cut_ = 0;
        for (const Edge &edge : problem.edges)
        {
            const int a = side_[edge.source];
            const int b = side_[edge.target];
            if (a >= 0 && b >= 0 && a != b)
                cut_ += edge.weight;
        }
    }

    void on_member_changed(const MaxCutProblem &problem,
                         int unit,
                         const std::vector<int> & /*members*/,
                         int vertex,
                         bool joined) override
    {
        std::int64_t across = 0;  // weight of e's edges to the other side
        for (int k = problem.adjStart[vertex]; k < problem.adjStart[vertex + 1]; ++k)
        {
            const int other = side_[problem.adjVertex[k]];
            if (other >= 0 && other != unit)
                across += problem.adjWeight[k];
        }
        if (joined)
        {
            cut_ += across;
            side_[vertex] = unit;
        }
        else
        {
            cut_ -= across;
            side_[vertex] = kUnassigned;
        }
    }

    // -cut, with an exact 0.0 (never -0.0) for an empty cut.
    double cost() const override { return cut_ == 0 ? 0.0 : -static_cast<double>(cut_); }

private:
    std::vector<int> side_;
    std::int64_t cut_ = 0;
};

}  // namespace mc

#endif  // MC_CONSTRAINTS_CUT_WEIGHT_COST_HPP
