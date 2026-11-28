#ifndef BS_DOMAIN_HPP
#define BS_DOMAIN_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"

namespace bs {

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The engine's hard/soft Score, no medium level. At the default weights:
//   hard = -(length by which parcels exceed their box, over the sorted sides
//            + parcel weight above the box's maxWeight
//            + distinct boxes in use above maxBoxTypes)
//   soft = -(quantity * (box volume - parcel volume), over the parcels
//            + quantity * the box's cost, over the parcels)
// The weights are the components' kDefaultWeight (bs/constraints/*.hpp); the
// problem carries none. The volumes are whole numbers and the engine keeps
// soft in a double, which is why ScoreCalculator::prepare refuses a problem
// whose shipped volume could pass 2^53.
using Score = optimaru::Score;

// The three sides of a size, ascending: a parcel may be turned any way, so
// only the sorted sides matter.
using Sides = std::array<std::int64_t, 3>;

inline Sides sortedSides(std::int64_t length, std::int64_t width, std::int64_t height) {
    Sides sides{length, width, height};
    std::sort(sides.begin(), sides.end());
    return sides;
}

// ---------------------------------------------------------------------------
// Box  (problem fact, the value)
// ---------------------------------------------------------------------------
// One candidate box size. Lengths and weights are whole numbers in the
// caller's unit (the guide's examples use mm and g).
struct Box {
    std::string id;
    std::int64_t length = 0;  // inner size, > 0
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::optional<std::int64_t> maxWeight;  // no limit when empty
    double cost = 0.0;                      // of one box of this size (material, freight), >= 0
    // Already stocked and staying: it counts as in use even when no parcel
    // takes it.
    bool required = false;
};

// ---------------------------------------------------------------------------
// Parcel  (entity)
// ---------------------------------------------------------------------------
// The packed content of one shipment, by its bounding size. `quantity` is how
// many shipments had this size: they all go in the same box size. `box` is the
// decision variable: the index into BoxSizingProblem::boxes, -1 == unassigned.
struct Parcel {
    std::string id;
    std::int64_t length = 0;  // > 0
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::int64_t weight = 0;    // >= 0, of one shipment
    std::int64_t quantity = 1;  // >= 1

    int box = -1;  // decision variable; -1 == unassigned

    bool isAssigned() const { return box >= 0; }
};

// ---------------------------------------------------------------------------
// BoxSizingProblem  (solution)
// ---------------------------------------------------------------------------
struct BoxSizingProblem {
    std::vector<Box> boxes;
    std::vector<Parcel> parcels;
    // How many distinct boxes may be in use (the required ones included); no
    // limit when empty.
    std::optional<int> maxBoxTypes;

    // true = the next solve starts from the parcels' boxes (a seed), completed
    // where a parcel is still -1; false = a solve ignores them. A solve clears
    // it.
    bool warmStart = false;

    Score score;

    // Set by ScoreCalculator::prepare, which also validates the facts; cleared
    // by whoever replaces boxes or parcels.
    bool prepared = false;
    // Filled by ScoreCalculator::prepare.
    std::vector<Sides> boxSides;  // ascending
    std::vector<Sides> parcelSides;
    std::vector<std::int64_t> boxVolume;
    std::vector<std::int64_t> parcelVolume;

    int boxCount() const { return static_cast<int>(boxes.size()); }
    int parcelCount() const { return static_cast<int>(parcels.size()); }

    // --- the terms of the score, per (parcel, box); they need prepare() ---

    // The length by which the parcel exceeds the box, summed over the three
    // sorted sides; 0 = it fits in some rotation.
    std::int64_t fitExcess(int parcel, int box) const {
        const Sides& p = parcelSides[parcel];
        const Sides& b = boxSides[box];
        return std::max<std::int64_t>(0, p[0] - b[0]) + std::max<std::int64_t>(0, p[1] - b[1]) +
               std::max<std::int64_t>(0, p[2] - b[2]);
    }
    // The parcel's weight above the box's maxWeight; 0 without a limit.
    std::int64_t weightExcess(int parcel, int box) const {
        const std::optional<std::int64_t>& limit = boxes[box].maxWeight;
        if (!limit) return 0;
        return std::max<std::int64_t>(0, parcels[parcel].weight - *limit);
    }
    // The box takes the parcel: by size and by weight.
    bool fits(int parcel, int box) const {
        return fitExcess(parcel, box) == 0 && weightExcess(parcel, box) == 0;
    }
    // quantity * (box volume - parcel volume): the air shipped. A parcel
    // larger than the box (which does not fit) counts 0, not a negative
    // volume, so the cost is never a bonus.
    std::int64_t voidOf(int parcel, int box) const {
        const std::int64_t air = boxVolume[box] - parcelVolume[parcel];
        return air > 0 ? parcels[parcel].quantity * air : 0;
    }

    // --- summary getters (read the facts directly: no prepare() needed) ---

    // Shipments per box: the quantities of its parcels, summed.
    std::vector<std::int64_t> parcelsPerBox() const {
        std::vector<std::int64_t> counts(boxes.size(), 0);
        for (const Parcel& parcel : parcels) {
            if (parcel.box >= 0 && parcel.box < boxCount()) counts[parcel.box] += parcel.quantity;
        }
        return counts;
    }

    // Distinct boxes in use: those a parcel ships in and the required ones.
    int boxTypesUsed() const {
        std::vector<char> used(boxes.size(), 0);
        for (std::size_t b = 0; b < boxes.size(); ++b) used[b] = boxes[b].required ? 1 : 0;
        for (const Parcel& parcel : parcels) {
            if (parcel.box >= 0 && parcel.box < boxCount()) used[parcel.box] = 1;
        }
        int count = 0;
        for (char u : used) count += u;
        return count;
    }

    // Sum over the assigned parcels of quantity * (box volume - parcel
    // volume), a parcel larger than its box counting 0.
    std::int64_t voidVolume() const {
        std::int64_t total = 0;
        for (const Parcel& parcel : parcels) {
            if (parcel.box < 0 || parcel.box >= boxCount()) continue;
            const Box& box = boxes[parcel.box];
            const std::int64_t air = box.length * box.width * box.height -
                                     parcel.length * parcel.width * parcel.height;
            if (air > 0) total += parcel.quantity * air;
        }
        return total;
    }

    // Shipped parcel volume / shipped box volume over the assigned parcels;
    // 0 when nothing is assigned.
    double fillRatio() const {
        long double parcelVolumeSum = 0.0L;
        long double boxVolumeSum = 0.0L;
        for (const Parcel& parcel : parcels) {
            if (parcel.box < 0 || parcel.box >= boxCount()) continue;
            const Box& box = boxes[parcel.box];
            const auto quantity = static_cast<long double>(parcel.quantity);
            parcelVolumeSum += quantity * static_cast<long double>(parcel.length) *
                               static_cast<long double>(parcel.width) *
                               static_cast<long double>(parcel.height);
            boxVolumeSum += quantity * static_cast<long double>(box.length) *
                            static_cast<long double>(box.width) *
                            static_cast<long double>(box.height);
        }
        return boxVolumeSum > 0.0L ? static_cast<double>(parcelVolumeSum / boxVolumeSum) : 0.0;
    }

    std::vector<int> assignment() const {
        std::vector<int> out;
        out.reserve(parcels.size());
        for (const Parcel& parcel : parcels) out.push_back(parcel.box);
        return out;
    }
};

}  // namespace bs

// ---------------------------------------------------------------------------
// SolutionTraits specialization: entities are parcels, the decision variable
// is Parcel::box, the value range is the candidate list
// (BoxSizingProblem::boxes).
// ---------------------------------------------------------------------------
template <>
struct optimaru::SolutionTraits<bs::BoxSizingProblem> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Assignment;
    static int entity_count(const bs::BoxSizingProblem& problem) { return problem.parcelCount(); }
    static int value_count(const bs::BoxSizingProblem& problem) { return problem.boxCount(); }
    static int value_of(const bs::BoxSizingProblem& problem, int parcel) {
        return problem.parcels[parcel].box;
    }
    static void assign(bs::BoxSizingProblem& problem, int parcel, int box) {
        problem.parcels[parcel].box = box;
    }
};

#endif  // BS_DOMAIN_HPP
