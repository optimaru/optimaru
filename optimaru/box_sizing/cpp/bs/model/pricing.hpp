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
    double voidWeight = 1.0;      // voidVolumeCost
    double costWeight = 1.0;      // boxCost
    double typeWeight = 0.0;      // boxTypeCost: the fixed cost of one more size
    double boardWeight = 0.0;     // boardAreaCost
    double shippingWeight = 0.0;  // shippingCost
};

// Among the boxes that take a parcel, its price is
//   quantity * (voidWeight * (box volume - parcel volume) + costWeight * cost
//               + boardWeight * board + shippingWeight * shipping price).
// Without shipping prices (no rate tables, or shippingWeight 0) the cheapest
// one is the same for every parcel: the box with the lowest
//   unit price = voidWeight * volume + costWeight * cost + boardWeight * board.
// The ranking is the boxes in that order, and a parcel's cheapest box in a
// set is the first box of the set, in this order, that takes it.
//
// Parcels that the same boxes take have the same cheapest box in every set, so
// a move places them together: `groups` holds them, and a move asks the
// fitting question once per group instead of once per parcel.
//
// With shipping prices a box's price depends on the parcel too (its weight
// and zone, against the box's dimensional weight). Parcels that the same
// boxes take AND that every one of those boxes prices at the same break of
// the same rate table still pay the same, so the groups are those, and each
// group has its own order (`perGroup`): the boxes that take it, by
// unit price + shippingWeight * its shipping price, ties to the smaller
// volume, then the lower index. The accessors orderOf, priceOf and rankOf
// answer for a group in both cases.
struct BoxRanking {
    std::vector<double> unitPrice;  // per box
    std::vector<int> order;         // box indices, cheapest first (ties: lower index)
    std::vector<int> rank;          // per box: its position in `order`
    double typeWeight = 0.0;
    std::vector<std::vector<int>> groups;     // parcels by the set of boxes that take them
    std::vector<std::int64_t> groupQuantity;  // per group: its shipments
    std::vector<int> groupOf;                 // per parcel: its group

    // Shipping prices: per group, its order (only the boxes that take it),
    // its price and its rank per box. Empty unless perGroup.
    bool perGroup = false;
    std::vector<std::vector<int>> groupOrder;
    std::vector<std::vector<double>> groupPrice;
    std::vector<std::vector<int>> groupRank;

    static BoxRanking of(const BoxSizingProblem& problem, const Pricing& pricing) {
        BoxRanking ranking;
        const int boxCount = problem.boxCount();
        ranking.typeWeight = pricing.typeWeight;
        ranking.unitPrice.resize(static_cast<std::size_t>(boxCount));
        for (int b = 0; b < boxCount; ++b) {
            ranking.unitPrice[b] = pricing.voidWeight * static_cast<double>(problem.boxVolume[b]) +
                                   pricing.costWeight * problem.boxes[b].cost;
            if (pricing.boardWeight != 0.0) {
                ranking.unitPrice[b] +=
                        pricing.boardWeight * static_cast<double>(problem.boxArea[b]);
            }
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

        ranking.perGroup = pricing.shippingWeight != 0.0 && !problem.rateTables.empty();
        std::unordered_map<std::string, int> groupOf;
        std::string takes(static_cast<std::size_t>(boxCount), '0');
        ranking.groupOf.resize(static_cast<std::size_t>(problem.parcelCount()));
        for (int p = 0; p < problem.parcelCount(); ++p) {
            for (int b = 0; b < boxCount; ++b) takes[b] = problem.fits(p, b) ? '1' : '0';
            std::string key = takes;
            if (ranking.perGroup) {
                // The table, and per box that takes the parcel, the break.
                const int table = problem.parcelTable[p];
                key += ':' + std::to_string(table);
                for (int b = 0; b < boxCount; ++b) {
                    if (takes[b] == '0') continue;
                    key += ',' + std::to_string(problem.rateTables[table].breakOf(
                                         problem.billedWeight(p, b)));
                }
            }
            const auto found = groupOf.emplace(key, static_cast<int>(ranking.groups.size()));
            if (found.second) {
                ranking.groups.emplace_back();
                ranking.groupQuantity.push_back(0);
            }
            ranking.groups[found.first->second].push_back(p);
            ranking.groupQuantity[found.first->second] += problem.parcels[p].quantity;
            ranking.groupOf[p] = found.first->second;
        }

        if (ranking.perGroup) {
            const std::size_t groupCount = ranking.groups.size();
            ranking.groupOrder.resize(groupCount);
            ranking.groupPrice.assign(groupCount, ranking.unitPrice);
            ranking.groupRank.assign(groupCount, std::vector<int>(boxCount, boxCount));
            for (std::size_t g = 0; g < groupCount; ++g) {
                const int p = ranking.groups[g].front();
                std::vector<double>& price = ranking.groupPrice[g];
                std::vector<int>& groupOrder = ranking.groupOrder[g];
                for (int b = 0; b < boxCount; ++b) {
                    if (!problem.fits(p, b)) continue;
                    price[b] += pricing.shippingWeight * problem.shippingPrice(p, b);
                    groupOrder.push_back(b);
                }
                std::stable_sort(groupOrder.begin(), groupOrder.end(),
                                 [&price, &problem](int a, int b) {
                                     if (price[a] != price[b]) return price[a] < price[b];
                                     return problem.boxVolume[a] < problem.boxVolume[b];
                                 });
                for (std::size_t position = 0; position < groupOrder.size(); ++position) {
                    ranking.groupRank[g][groupOrder[position]] = static_cast<int>(position);
                }
            }
        }
        return ranking;
    }

    // A group's boxes, cheapest first: without shipping prices every box (the
    // caller asks which take the group), with them only the boxes that do.
    const std::vector<int>& orderOf(int group) const {
        return perGroup ? groupOrder[group] : order;
    }
    // What one shipment of the group pays in the box, up to a constant of
    // the group.
    double priceOf(int group, int box) const {
        return perGroup ? groupPrice[group][box] : unitPrice[box];
    }
    // The box's position in orderOf(group); with shipping prices, a box that
    // does not take the group is after all of them.
    int rankOf(int group, int box) const { return perGroup ? groupRank[group][box] : rank[box]; }

    // The cheapest box that takes the parcel, any box allowed but `except`;
    // -1 when none does.
    int cheapestFor(const BoxSizingProblem& problem, int parcel, int except = -1) const {
        for (int box : orderOf(groupOf[parcel])) {
            if (box != except && problem.fits(parcel, box)) return box;
        }
        return -1;
    }
};

}  // namespace bs

#endif  // BS_PRICING_HPP
