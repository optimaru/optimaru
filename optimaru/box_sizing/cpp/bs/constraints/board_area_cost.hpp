#ifndef BS_CONSTRAINTS_BOARD_AREA_COST_HPP
#define BS_CONSTRAINTS_BOARD_AREA_COST_HPP

#include <cstdint>

#include "bs/constraints/common.hpp"

namespace bs {

// Soft, weight 0 (opt-in): boardAreaCost -- the board the boxes shipped are
// made of. Raw cost per box = over its parcels, quantity * 2 (lw + lh + wh),
// computed as (shipments in the box) * its board; 0 for a box without
// parcels.
//
// The material a size uses, where voidVolumeCost counts the air it ships: of
// two boxes that hold the same, the cube uses less board than the flat one.
// It shares the soft level with the void volume, so its weight says how much
// volume one unit of area is worth (in mm: a weight of 100 says 1 mm2 of
// board costs as much as 100 mm3 of air).
struct BoardAreaCost final : BsUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit BoardAreaCost(double weight = kDefaultWeight)
        : BsUnitConstraint("boardAreaCost", weight, kDefaultLevel) {}
    void on_reset(const BoxSizingProblem& problem) override { detail::requirePrepared(problem); }
    double cost(const BoxSizingProblem& problem, int unit,
                const std::vector<int>& members) const override {
        std::int64_t quantity = 0;
        for (int parcel : members) quantity += problem.parcels[parcel].quantity;
        return static_cast<double>(quantity) * static_cast<double>(problem.boxArea[unit]);
    }
};

}  // namespace bs

#endif  // BS_CONSTRAINTS_BOARD_AREA_COST_HPP
