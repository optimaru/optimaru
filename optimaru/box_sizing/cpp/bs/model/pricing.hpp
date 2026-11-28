#ifndef BS_PRICING_HPP
#define BS_PRICING_HPP

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "bs/model/domain.hpp"

namespace bs {

// ---------------------------------------------------------------------------
// What a box costs a parcel, for the construction and the moves
// ---------------------------------------------------------------------------
// The score decides whether a step is taken; these prices only decide which
// step is proposed. They are the soft weights of the solve: the defaults, or
// the picked ones (see pricingOf in solver.cpp).
struct Pricing {
    double voidWeight = 1.0;  // voidVolumeCost
    double costWeight = 1.0;  // boxCost
    double typeWeight = 0.0;  // boxTypeCost: the fixed cost of one more size
};

// Among the boxes that take a parcel, its price is
//   quantity * (voidWeight * (box volume - parcel volume) + costWeight * cost),
// so the cheapest one is the same for every parcel: the box with the lowest
//   unit price = voidWeight * volume + costWeight * cost.
// The ranking is the boxes in that order, and a parcel's cheapest box in a
// set is the first box of the set, in this order, that takes it.
//
// Parcels that the same boxes take have the same cheapest box in every set, so
// a move places them together: `groups` holds them, and a move asks the
// fitting question once per group instead of once per parcel.
struct BoxRanking {
    std::vector<double> unitPrice;  // per box
    std::vector<int> order;         // box indices, cheapest first (ties: lower index)
    std::vector<int> rank;          // per box: its position in `order`
    double typeWeight = 0.0;
    std::vector<std::vector<int>> groups;     // parcels by the set of boxes that take them
    std::vector<std::int64_t> groupQuantity;  // per group: its shipments

    static BoxRanking of(const BoxSizingProblem& problem, const Pricing& pricing) {
        BoxRanking ranking;
        const int boxCount = problem.boxCount();
        ranking.typeWeight = pricing.typeWeight;
        ranking.unitPrice.resize(static_cast<std::size_t>(boxCount));
        for (int b = 0; b < boxCount; ++b) {
            ranking.unitPrice[b] = pricing.voidWeight * static_cast<double>(problem.boxVolume[b]) +
                                   pricing.costWeight * problem.boxes[b].cost;
        }
        ranking.order.resize(static_cast<std::size_t>(boxCount));
        std::iota(ranking.order.begin(), ranking.order.end(), 0);
        std::stable_sort(ranking.order.begin(), ranking.order.end(), [&ranking](int a, int b) {
            return ranking.unitPrice[a] < ranking.unitPrice[b];
        });
        ranking.rank.resize(static_cast<std::size_t>(boxCount));
        for (int position = 0; position < boxCount; ++position) {
            ranking.rank[ranking.order[position]] = position;
        }

        std::unordered_map<std::string, int> groupOf;
        std::string takes(static_cast<std::size_t>(boxCount), '0');
        for (int p = 0; p < problem.parcelCount(); ++p) {
            for (int b = 0; b < boxCount; ++b) takes[b] = problem.fits(p, b) ? '1' : '0';
            const auto found = groupOf.emplace(takes, static_cast<int>(ranking.groups.size()));
            if (found.second) {
                ranking.groups.emplace_back();
                ranking.groupQuantity.push_back(0);
            }
            ranking.groups[found.first->second].push_back(p);
            ranking.groupQuantity[found.first->second] += problem.parcels[p].quantity;
        }
        return ranking;
    }

    // The cheapest box that takes the parcel, any box allowed but `except`;
    // -1 when none does.
    int cheapestFor(const BoxSizingProblem& problem, int parcel, int except = -1) const {
        for (int box : order) {
            if (box != except && problem.fits(parcel, box)) return box;
        }
        return -1;
    }
};

}  // namespace bs

#endif  // BS_PRICING_HPP
