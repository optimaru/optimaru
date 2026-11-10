#ifndef CS_CONSTRAINTS_SECTOR_CONFLICT_COST_HPP
#define CS_CONSTRAINTS_SECTOR_CONFLICT_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft, weight 10: sectorConflictCost — two overlapping talks aimed at the
// same sector.
// Raw cost (one global total) = the sum of shared sectors x overlapping
// minutes for each pair of overlapping talks; 0 = no violation (no overlapping
// talks target the same sector).
struct SectorConflictCost final : BreakdownConstraint
{
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit SectorConflictCost(double weight = kDefaultWeight)
        : BreakdownConstraint("sectorConflictCost", weight, kDefaultLevel)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.sectorConflictCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SECTOR_CONFLICT_COST_HPP
