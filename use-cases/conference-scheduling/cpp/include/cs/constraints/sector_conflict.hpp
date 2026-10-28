#ifndef CS_CONSTRAINTS_SECTOR_CONFLICT_HPP
#define CS_CONSTRAINTS_SECTOR_CONFLICT_HPP

#include "cs/constraints/common.hpp"

namespace cs
{

// Soft (default, weight 10): sectorConflict — two overlapping talks aimed at
// the same sector.
struct SectorConflict final : BreakdownConstraint
{
    explicit SectorConflict(double weight = 10.0)
        : BreakdownConstraint("sectorConflict", weight, /*hard=*/false)
    {
    }
    std::int64_t select(const Breakdown &breakdown) const override
    {
        return breakdown.sectorConflict;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_SECTOR_CONFLICT_HPP
