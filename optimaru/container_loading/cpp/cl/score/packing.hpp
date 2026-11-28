#ifndef CL_SCORE_PACKING_HPP
#define CL_SCORE_PACKING_HPP

#include <cstdint>
#include <limits>
#include <vector>

#include "cl/model/domain.hpp"

namespace cl {

// ---------------------------------------------------------------------------
// The placement decoder
// ---------------------------------------------------------------------------
// The decision is an order: each container's list of items is its loading
// order. Where every item then stands is derived from that order, one item
// after the other, by the extreme-point rule:
//
//   - The decoder keeps the corners an item may be put at ("points"). An
//     empty container has one, its origin: the bottom corner at the closed
//     end.
//   - The next item of the list goes to the first point, in (x, z, y) order
//     (from the closed end towards the door, then up, then across), at which
//     one of its allowed orientations is feasible. Feasible = inside the
//     container, overlapping no placed item, and with at least
//     minSupportRatio of its base resting on the floor or on the tops of
//     placed items.
//   - Of the orientations feasible at that point, it takes the one that
//     divides the free room best: the one with the most axes along which the
//     room from the point to the next item or wall is a whole multiple of the
//     item's size, so that equal items line up into full rows and layers. Of
//     equals it takes the first in the order ScoreCalculator::prepare gives
//     them (the lowest, then the one that reaches less far towards the door).
//   - A placed item kills the points it covers and adds up to six new ones:
//     its three far corners, each slid back along the two other axes until it
//     meets a placed item or a wall (Crainic, Perboli and Tadei's extreme
//     points).
//   - An item for which no point works is unplaced. It stays in the list,
//     changes nothing for the items after it, and unplacedItemCost counts it.
//
// The decode is a pure function of (problem facts, container, list in order):
// it reads no weight limit, no unloading order and nothing of the other
// containers; those are costs computed on its result.
//
// It is also a function of the list's prefix: item k's place depends on items
// 0 .. k - 1 only. A Packing keeps what a decode needs to go on from any
// position (every point with the positions that made and covered it), so a
// list that shares its first k items with a decoded one costs only the items
// from k on. A move changes one or two lists somewhere in the middle, which
// makes this the difference between decoding a list and decoding half of it.

// One placed item.
struct Box {
    int item = -1;      // index into ContainerLoadingProblem::items
    int position = -1;  // its place in the list (unplaced items count)
    int x = 0;
    int y = 0;
    int z = 0;
    int length = 0;  // along x, after rotation
    int width = 0;   // along y
    int height = 0;  // along z
};

// One candidate corner, with the list positions that made it and covered it.
struct Point {
    static constexpr int kOpen = std::numeric_limits<int>::max();
    int x = 0;
    int y = 0;
    int z = 0;
    int born = -1;     // position whose placement added it; -1 = the origin
    int died = kOpen;  // position whose placement covered it; kOpen = still free
};

// One container's list, decoded.
struct Packing {
    std::vector<int> members;       // the list
    std::vector<int> boxOf;         // per list position: its index in `boxes`, -1 = unplaced
    std::vector<Box> boxes;         // the placed items, in loading order
    std::vector<Point> points;      // sorted by (x, z, y); covered ones stay, marked
    std::int64_t loadedVolume = 0;  // volume of the placed items
    std::int64_t loadedWeight = 0;  // weight of the placed items
    int usedLength = 0;             // largest x + length of a placed item

    int unplaced() const { return static_cast<int>(members.size() - boxes.size()); }

    // The costs that are not sums along the list are computed when first
    // asked for and kept with the decode (-1 = not yet).
    mutable std::int64_t stackingExcess = -1;
    mutable std::int64_t unloadingBlocks = -1;
    mutable int balancePerMille = -1;
};

// Decode `members` as the loading order of container `container` into `out`.
// `keep` is how many leading members `out` already holds a decode of, for the
// same container (0 = decode from an empty container; `out` may then hold
// anything). The result is the same whatever `keep` is.
void decode(const ContainerLoadingProblem& problem, int container, const std::vector<int>& members,
            Packing& out, int keep = 0);

// --- costs of one decoded container -----------------------------------------
// Each is the raw cost of the constraint of the same name
// (cl/constraints/<name>.hpp), written once here: the components and
// ScoreCalculator both call these.

// stackingLoadCost: over the placed items that set maxLoadOnTop, the weight
// resting on the item above that limit, rounded up to a whole unit. An item
// passes its own weight and what rests on it down to the items it stands on,
// shared by contact area (an item on the floor passes it to the floor).
std::int64_t stackingExcess(const ContainerLoadingProblem& problem, const Packing& packing);

// unloadingOrderCost: the pairs (a, b) of placed items with unloadOrder(a) <
// unloadOrder(b) where b is in a's way out: between a and the door (at or
// beyond a's far end along x, overlapping a in y and z) or above a (at or
// above a's top, overlapping a in x and y).
std::int64_t unloadingBlocks(const ContainerLoadingProblem& problem, const Packing& packing);

// loadBalanceCost: how far the placed items' centre of gravity is from the
// container's centre in the x-y plane, in thousandths of the half-diagonal
// (0 = centred, 1000 = in a corner), rounded to the nearest whole number. 0
// for a container whose placed items weigh nothing.
int balancePerMille(const ContainerLoadingProblem& problem, int container, const Packing& packing);

// ---------------------------------------------------------------------------
// PackingCache
// ---------------------------------------------------------------------------
// The decodes of each container the search last asked for. Several
// constraints cost the same list of the same container (a move's candidate
// first, then, if the move is taken, the list it becomes), so a decode is
// looked up by its list and made once.
//
// Each container has a few slots. A list that is in none of them is decoded
// into the least recently used one, starting from the slot that shares the
// longest prefix with it. pin() keeps one list's slot from being reused: the
// search pins the list a container holds now, which is what its next
// candidates differ from in one place.
//
// What it returns is decode()'s result exactly, so the cache changes how long
// a cost takes and never what it is. It keeps scratch of its own: one cache
// serves one calculator (or one move) on one thread.
class PackingCache {
public:
    static constexpr int kSlots = 3;

    // Before the first packing() of a problem, and again whenever its facts
    // may have changed: sizes the slots and drops every decode if the facts
    // (sizes, rotations, the support ratio, and the weights, load limits and
    // unloading orders the kept costs read) are not the ones the kept decodes
    // were made for. The facts are compared through a 64-bit hash,
    // which is trusted: a collision (practically impossible) would keep
    // decodes made for other facts.
    void reset(const ContainerLoadingProblem& problem);

    const Packing& packing(const ContainerLoadingProblem& problem, int container,
                           const std::vector<int>& members);

    // Keep the decode of `members` for this container until the next pin().
    // If no slot holds it yet, the next packing() of that list is the one
    // kept; until then the slot pinned before is free to be reused.
    void pin(int container, const std::vector<int>& members);

    // How many lists were decoded (in full or from a prefix) and how many
    // were found in a slot, since the last reset() that dropped the decodes.
    std::int64_t decodes() const { return decodes_; }
    std::int64_t hits() const { return hits_; }

private:
    struct Slots {
        Packing slot[kSlots];
        bool valid[kSlots] = {};
        std::uint64_t used[kSlots] = {};
        int pinned = -1;
        bool pinNext = false;      // pin `pending` once it is decoded
        std::vector<int> pending;  // the list pin() asked for that no slot held yet
    };

    std::vector<Slots> containers_;
    std::uint64_t fingerprint_ = 0;
    std::uint64_t clock_ = 0;
    std::int64_t decodes_ = 0;
    std::int64_t hits_ = 0;
};

}  // namespace cl

#endif  // CL_SCORE_PACKING_HPP
