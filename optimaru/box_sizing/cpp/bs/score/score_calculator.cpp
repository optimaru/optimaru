#include "bs/score/score_calculator.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "bs/constraints/box_cost.hpp"
#include "bs/constraints/box_type_limit_cost.hpp"
#include "bs/constraints/box_weight_cost.hpp"
#include "bs/constraints/parcel_fit_cost.hpp"
#include "bs/constraints/void_volume_cost.hpp"

namespace bs {

namespace {

// The monolithic score is the default assembly, so it reads the same
// constants the components carry. The hard weights are integral, so the hard
// level is computed exactly, in int64.
constexpr auto kFitWeight = static_cast<std::int64_t>(ParcelFitCost::kDefaultWeight);
constexpr auto kWeightWeight = static_cast<std::int64_t>(BoxWeightCost::kDefaultWeight);
constexpr auto kLimitWeight = static_cast<std::int64_t>(BoxTypeLimitCost::kDefaultWeight);
static_assert(kFitWeight == ParcelFitCost::kDefaultWeight &&
                      kWeightWeight == BoxWeightCost::kDefaultWeight &&
                      kLimitWeight == BoxTypeLimitCost::kDefaultWeight,
              "the monolithic calculator assumes integral default hard weights");

[[noreturn]] void refuse(const std::string& message) { throw std::invalid_argument(message); }

// How many of the parcels that fit no candidate a refusal names after the first.
constexpr std::size_t kMisfitsNamed = 5;

void requireSide(const std::string& who, const char* name, std::int64_t value) {
    if (value < 1 || value > ScoreCalculator::kMaxSide) {
        refuse(who + ": " + name + " must be between 1 and " +
               std::to_string(ScoreCalculator::kMaxSide) + ", not " + std::to_string(value));
    }
}

void requireBoxesInRange(const BoxSizingProblem& problem) {
    const int boxCount = problem.boxCount();
    for (const Parcel& parcel : problem.parcels) {
        if (parcel.box < -1 || parcel.box >= boxCount) {
            refuse("parcel '" + parcel.id + "' has box " + std::to_string(parcel.box) +
                   ", outside -1.." + std::to_string(boxCount - 1));
        }
    }
}

// The rate tables: zones unique and not empty, breaks ascending, prices
// finite and >= 0.
void requireRateTables(const BoxSizingProblem& problem) {
    for (std::size_t t = 0; t < problem.rateTables.size(); ++t) {
        const RateTable& table = problem.rateTables[t];
        const std::string who = table.zone ? "the rate table of zone '" + *table.zone + "'"
                                           : std::string("the rate table without a zone");
        if (table.zone && table.zone->empty()) refuse("a rate table has an empty zone");
        if (problem.tableOf(table.zone) != static_cast<int>(t)) {
            refuse(who + " is given twice: one rate table per zone");
        }
        if (table.breaks.empty()) refuse(who + " has no breaks");
        for (std::size_t i = 0; i < table.breaks.size(); ++i) {
            const RateBreak& rateBreak = table.breaks[i];
            if (rateBreak.maxWeight < 0) refuse(who + ": a break's max_weight must be >= 0");
            if (i > 0 && rateBreak.maxWeight <= table.breaks[i - 1].maxWeight) {
                refuse(who + ": the breaks' max_weight must be strictly ascending");
            }
            if (!std::isfinite(rateBreak.price) || rateBreak.price < 0.0) {
                refuse(who + ": a break's price must be finite and >= 0");
            }
        }
    }
}

// Every parcel's billed weight in every box that takes it is within its
// table's top break: past it a rate table says nothing. Needs the rest of
// prepare.
void requireCoveredByRates(const BoxSizingProblem& problem) {
    if (problem.rateTables.empty()) return;
    for (int p = 0; p < problem.parcelCount(); ++p) {
        const RateTable& table = problem.rateTables[problem.parcelTable[p]];
        const std::int64_t top = table.breaks.back().maxWeight;
        for (int b = 0; b < problem.boxCount(); ++b) {
            if (!problem.fits(p, b) || problem.billedWeight(p, b) <= top) continue;
            refuse("parcel '" + problem.parcels[p].id + "' bills " +
                   std::to_string(problem.billedWeight(p, b)) + " in box '" + problem.boxes[b].id +
                   "', above the top break (" + std::to_string(top) + ") of " +
                   (table.zone ? "the rate table of zone '" + *table.zone + "'"
                               : std::string("the rate table without a zone")) +
                   ": add a break that covers it");
        }
    }
}

}  // namespace

void ScoreCalculator::prepare(BoxSizingProblem& problem) {
    // maxBoxTypes stays writable after construction and `prepared` does not
    // cover it, so it is checked on every call; so are the parcels' boxes.
    if (problem.maxBoxTypes && *problem.maxBoxTypes < 1) {
        refuse("max_box_types must be at least 1, not " + std::to_string(*problem.maxBoxTypes));
    }
    requireBoxesInRange(problem);
    if (problem.prepared) return;

    if (problem.boxes.empty()) refuse("boxes must hold at least one candidate box");
    std::unordered_set<std::string> ids;
    std::int64_t largestVolume = 0;
    // The heaviest billed weight a shipment can have: over every box, also
    // those a parcel does not fit, so the bound below is on the safe side.
    std::int64_t largestBilled = 0;
    bool priced = false;  // a box has a dimDivisor, or there are rate tables
    problem.boxSides.clear();
    problem.boxVolume.clear();
    problem.boxArea.clear();
    problem.boxDimWeight.clear();
    for (const Box& box : problem.boxes) {
        if (box.id.empty()) refuse("a box has an empty id");
        if (!ids.insert(box.id).second) refuse("box ids must be unique: '" + box.id + "'");
        const std::string who = "box '" + box.id + "'";
        requireSide(who, "length", box.length);
        requireSide(who, "width", box.width);
        requireSide(who, "height", box.height);
        if (box.maxWeight && (*box.maxWeight < 0 || *box.maxWeight > kMaxWeight)) {
            refuse(who + ": max_weight must be between 0 and " + std::to_string(kMaxWeight));
        }
        if (!std::isfinite(box.cost) || box.cost < 0.0) {
            refuse(who + ": cost must be finite and >= 0");
        }
        if (box.dimDivisor && *box.dimDivisor < 1) {
            refuse(who + ": dim_divisor must be at least 1, not " +
                   std::to_string(*box.dimDivisor));
        }
        if (box.dimDivisor) priced = true;
        problem.boxSides.push_back(sortedSides(box.length, box.width, box.height));
        problem.boxVolume.push_back(box.length * box.width * box.height);
        problem.boxArea.push_back(boardAreaOf(box));
        problem.boxDimWeight.push_back(dimWeightOf(box));
        largestVolume = std::max(largestVolume, problem.boxVolume.back());
        largestBilled = std::max(largestBilled, problem.boxDimWeight.back());
    }
    requireRateTables(problem);
    if (!problem.rateTables.empty()) priced = true;

    ids.clear();
    long double quantities = 0.0L;
    problem.parcelSides.clear();
    problem.parcelVolume.clear();
    problem.parcelTable.clear();
    for (const Parcel& parcel : problem.parcels) {
        if (parcel.id.empty()) refuse("a parcel has an empty id");
        if (!ids.insert(parcel.id).second) {
            refuse("parcel ids must be unique: '" + parcel.id + "'");
        }
        const std::string who = "parcel '" + parcel.id + "'";
        requireSide(who, "length", parcel.length);
        requireSide(who, "width", parcel.width);
        requireSide(who, "height", parcel.height);
        if (parcel.weight < 0 || parcel.weight > kMaxWeight) {
            refuse(who + ": weight must be between 0 and " + std::to_string(kMaxWeight));
        }
        if (parcel.quantity < 1) refuse(who + ": quantity must be at least 1");
        quantities += static_cast<long double>(parcel.quantity);
        problem.parcelSides.push_back(sortedSides(parcel.length, parcel.width, parcel.height));
        problem.parcelVolume.push_back(parcel.length * parcel.width * parcel.height);
        largestBilled = std::max(largestBilled, parcel.weight);
        if (parcel.zone && parcel.zone->empty()) refuse(who + ": zone must not be empty");
        const int table = problem.tableOf(parcel.zone);
        if (table < 0 && (parcel.zone || !problem.rateTables.empty())) {
            refuse(who + ": " +
                   (parcel.zone ? "no rate table has zone '" + *parcel.zone + "'"
                                : std::string("no zone, and no rate table without a zone")) +
                   ": add a rate table for it");
        }
        problem.parcelTable.push_back(table);
    }
    // Every void is at most quantity * the largest box volume, so below this
    // bound every sum of voids is a whole number a double holds exactly.
    if (quantities * static_cast<long double>(largestVolume) >
        static_cast<long double>(kMaxVolume)) {
        refuse("the shipped volume is too large for an exact score: the summed quantities times "
               "the largest box volume must stay below 2^53; use a coarser length unit");
    }
    // The billed weight is reported as a whole number per box: below this
    // bound every total is exact. Only problems that bill by weight are held
    // to it.
    if (priced && quantities * static_cast<long double>(largestBilled) >
                          static_cast<long double>(kMaxVolume)) {
        refuse("the billed weight is too large for an exact total: the summed quantities times "
               "the heaviest billed weight must stay below 2^53; use a coarser weight unit");
    }
    requireCoveredByRates(problem);

    problem.prepared = true;
}

void ScoreCalculator::requireSolvable(const BoxSizingProblem& problem) {
    const int boxCount = problem.boxCount();
    std::vector<int> misfits;
    for (int p = 0; p < problem.parcelCount(); ++p) {
        bool fitsOne = false;
        for (int b = 0; b < boxCount && !fitsOne; ++b) fitsOne = problem.fits(p, b);
        if (!fitsOne) misfits.push_back(p);
    }
    if (!misfits.empty()) {
        // The first one in full, and the others by id (five of them), so a
        // caller with many sees how many and where to start.
        const Parcel& parcel = problem.parcels[misfits.front()];
        std::string message =
                "parcel '" + parcel.id + "' (" + std::to_string(parcel.length) + " x " +
                std::to_string(parcel.width) + " x " + std::to_string(parcel.height) + ", weight " +
                std::to_string(parcel.weight) +
                ") fits none of the candidate boxes: add a box that takes it, or leave it out";
        if (misfits.size() > 1) {
            message += " (and " + std::to_string(misfits.size() - 1) + " more: ";
            for (std::size_t i = 1; i < misfits.size() && i <= kMisfitsNamed; ++i) {
                message += (i > 1 ? ", '" : "'") + problem.parcels[misfits[i]].id + "'";
            }
            message += misfits.size() - 1 > kMisfitsNamed ? ", ...)" : ")";
        }
        refuse(message);
    }
    if (problem.maxBoxTypes) {
        int required = 0;
        for (const Box& box : problem.boxes) required += box.required ? 1 : 0;
        if (required > *problem.maxBoxTypes) {
            refuse("max_box_types is " + std::to_string(*problem.maxBoxTypes) + ", but " +
                   std::to_string(required) + " boxes are required");
        }
    }
}

Score ScoreCalculator::score(BoxSizingProblem& problem) {
    prepare(problem);

    // Per-box aggregates of the assigned parcels.
    const std::size_t boxCount = problem.boxes.size();
    std::vector<std::int64_t> voidOfBox(boxCount, 0);
    std::vector<std::int64_t> quantityOfBox(boxCount, 0);
    std::vector<char> used(boxCount, 0);
    for (std::size_t b = 0; b < boxCount; ++b) used[b] = problem.boxes[b].required ? 1 : 0;

    std::int64_t fitExcess = 0;     // hard 1
    std::int64_t weightExcess = 0;  // hard 2
    for (int p = 0; p < problem.parcelCount(); ++p) {
        const int box = problem.parcels[p].box;
        if (box < 0) continue;  // unassigned: contributes nothing
        fitExcess += problem.fitExcess(p, box);
        weightExcess += problem.weightExcess(p, box);
        voidOfBox[box] += problem.voidOf(p, box);
        quantityOfBox[box] += problem.parcels[p].quantity;
        used[box] = 1;
    }

    // hard 3: boxTypeLimitCost.
    std::int64_t boxTypes = 0;
    for (char u : used) boxTypes += u;
    const std::int64_t overLimit = problem.maxBoxTypes && boxTypes > *problem.maxBoxTypes
                                           ? boxTypes - *problem.maxBoxTypes
                                           : 0;

    // soft 4 and 5, box by box in the order the plugin assembly adds them up
    // (voidVolumeCost, then boxCost): a cost may be fractional, and the same
    // order gives the same double.
    double soft = 0.0;
    for (std::size_t b = 0; b < boxCount; ++b) {
        soft += VoidVolumeCost::kDefaultWeight * static_cast<double>(voidOfBox[b]);
        soft += BoxCost::kDefaultWeight *
                (static_cast<double>(quantityOfBox[b]) * problem.boxes[b].cost);
    }

    Score score;
    score.hard =
            -(kFitWeight * fitExcess + kWeightWeight * weightExcess + kLimitWeight * overLimit);
    score.soft = soft == 0.0 ? 0.0 : -soft;  // a zero penalty is 0, not -0
    problem.score = score;
    return score;
}

}  // namespace bs
