#ifndef CL_SCORE_PACKING_HPP
#define CL_SCORE_PACKING_HPP

#include <cstdint>
#include <limits>
#include <utility>
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
//     placed items. The container's top is its fillHeight (maxStackHeight,
//     at most its height), and a bottomOnly item is tried only at points on
//     the floor.
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
//   - When items set an unloadOrder, a maxLoadOnTop, a maxStackLayers or a
//     stackGroup a stacking rule names, a place is also judged by what it
//     would break. A place "blocks" when the item would stand in the way of a
//     placed item that leaves earlier (between it and the door, or above it),
//     or have a placed item that leaves later in its own way; it "overloads"
//     when its weight, passed down through the items it stands on, would put
//     more on a placed item than that item's maxLoadOnTop, when it would stand
//     on a placed item (or have one stand on it) against a stacking rule, or
//     when it would make a column taller than its own maxStackLayers or that
//     of an item under it. The item goes to the first point where some
//     orientation does neither; an item with an unloadOrder prefers, of those,
//     the first place within the depth its stop's placed items already reach
//     (their largest x + length), so that a stop fills the cross-section it
//     has started before it goes deeper. If every place breaks something, the
//     first place that only blocks, the first that overloads and leaving the
//     item unplaced are weighed by ContainerLoadingProblem::breakWeights (the
//     weights of the solve's constraints: per pair added, per unit of weight
//     over a limit, per place that breaks a stacking rule, a layer limit or a
//     segregation distance, per item left out times its priority) and the
//     cheapest is
//     taken; of equal costs blocking, then overloading, then leaving out. A
//     list in the construction's order (what leaves last first) comes out
//     with every stop nearer the door than the stops after it wherever the
//     room allows. The loads passed down are tracked as the items are placed:
//     an item placed later under an overhang does not take load from the item
//     above it, which the stacking cost on the result does count.
//   - An optional item whose weight would put the items placed so far above
//     the container's maxWeight is unplaced, wherever there is room, and so
//     is an item whose allowedContainers leave this container out.
//   - An item whose segregation class must keep a distance from a placed
//     item's treats a place closer than that as one that breaks a limit.
//
// The decode is a pure function of (problem facts, break weights, container,
// list in order): it reads the container's weight limit only for optional
// items, and nothing of the other containers. The costs are computed on its
// result.
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
    int length = 0;        // along x, after rotation
    int width = 0;         // along y
    int height = 0;        // along z
    int order = kNoOrder;  // the item's unloadOrder, kNoOrder = none
    static constexpr int kNoOrder = std::numeric_limits<int>::min();
};

// One candidate corner, with the list positions that made it and covered it.
struct Point {
    static constexpr int kOpen = std::numeric_limits<int>::max();
    int x = 0;
    int y = 0;
    int z = 0;
    int born = -1;     // position whose placement added it; -1 = the origin
    int died = kOpen;  // position whose placement covered it; kOpen = still free
    // While it is free: how far the room reaches from it along x, y and z, up
    // to the first placed item the ray from it meets or the wall. An item
    // longer than that along an axis cannot stand at it. Measured when the
    // decode first looks at the point (room[0] < 0 until then), and kept up
    // as items are placed.
    int room[3] = {-1, -1, -1};
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

    // The boxes by where they are along x: cells[c] holds, in increasing
    // order, the indices into `boxes` of those whose x-range meets
    // [c * cellLength, (c + 1) * cellLength). A box is checked only against
    // the boxes of the cells its own x-range meets.
    int cellLength = 1;
    std::vector<std::vector<int>> cells;
    // Per cell, the lowest and highest unloadOrder of its boxes that have one
    // (INT_MAX / INT_MIN when none has): a cell whose orders cannot block an
    // item is skipped without looking at its boxes.
    std::vector<int> cellLowOrder;
    std::vector<int> cellHighOrder;
    // The boxes by the height of their tops: (z + height, index into
    // `boxes`), sorted. What an item at height z can stand on is the run of
    // boxes whose tops are at z.
    std::vector<std::pair<int, int>> tops;

    // Only when some item sets maxLoadOnTop: per box, the load resting on it
    // as passed down so far (kept current on the boxes that carry a limit),
    // whether it carries a limit or stands, directly or not, on one that
    // does (bearsLimit), and what it stands on: the boxes
    // supportBox[supportStart[b] .. supportStart[b + 1]), each with the share
    // of its load (by contact area) in supportShare.
    std::vector<double> carried;
    std::vector<char> bearsLimit;
    std::vector<int> supportStart;
    std::vector<int> supportBox;
    std::vector<double> supportShare;
    // Only when some item sets maxStackLayers: per box, its layer (1 = on the
    // floor, else one above the highest box it stands on) and how many items
    // stand in the tallest column on it. The supports above are then kept
    // too (also without load limits).
    std::vector<int> level;
    std::vector<int> above;

    int unplaced() const { return static_cast<int>(members.size() - boxes.size()); }

    // The costs that are not sums along the list are computed when first
    // asked for and kept with the decode (-1 = not yet).
    mutable std::int64_t stackingExcess = -1;
    mutable std::int64_t unloadingBlocks = -1;
    mutable int balancePerMille = -1;
    mutable std::int64_t stackingViolations = -1;
    mutable std::int64_t balanceExcess = -1;
    mutable std::int64_t segregationPairs = -1;
    mutable std::int64_t axleExcess = -1;
    // Only with optional items or priorities (unplacedCost < 0 = not yet).
    mutable std::int64_t unplacedCost = -1;
    mutable std::int64_t optionalCost = 0;
    mutable std::int64_t optionalWeight = 0;
};

// Decode `members` as the loading order of container `container` into `out`.
// `keep` is how many leading members `out` already holds a decode of, for the
// same container (0 = decode from an empty container; `out` may then hold
// anything). The result is the same whatever `keep` is.
void decode(const ContainerLoadingProblem& problem, int container, const std::vector<int>& members,
            Packing& out, int keep = 0);

// How far the room reaches from `point` along x, y and z in a decode of
// container `container`, measured afresh: what Point::room holds for a free
// point once the decode has looked at it. For the tests of that cache.
void measureRoom(const ContainerLoadingProblem& problem, int container, const Packing& packing,
                 const Point& point, int room[3]);

// Whether an item that leaves at `order`, put at `point` in orientation `o`,
// would block a placed box or be blocked by one, as the decoder checks it
// (looking only at the cells that can hold such a box), and how many such
// boxes there are, counted over every box. For the tests of the first.
bool wouldBlock(const Packing& packing, int order, const Point& point, const Orientation& o);
int pairsAdded(const Packing& packing, int order, const Point& point, const Orientation& o);

// --- costs of one decoded container -----------------------------------------
// Each is the raw cost of the constraint of the same name
// (cl/constraints/<name>.hpp), written once here: the components and
// ScoreCalculator both call these.

// unplacedItemCost: over the unplaced items of the list that are not
// optional, their priorities (the count of them while every priority is 1).
std::int64_t unplacedCost(const ContainerLoadingProblem& problem, const Packing& packing);

// optionalItemCost: over the unplaced optional items, their priorities.
std::int64_t optionalCost(const ContainerLoadingProblem& problem, const Packing& packing);

// The weight of the unplaced optional items of the list, which
// containerWeightCost does not count.
std::int64_t unplacedOptionalWeight(const ContainerLoadingProblem& problem, const Packing& packing);

// segregationCost: the pairs of placed items whose segregation classes may
// not share a container, plus the pairs whose classes must keep a distance and
// stand closer than it in the floor plane (the larger of the gaps between
// their footprints along x and along y).
std::int64_t segregationPairs(const ContainerLoadingProblem& problem, const Packing& packing);

// The axle loads of a container with two axle groups, by the lever rule:
// what a placed item of weight w whose middle is at x puts on the front group
// is w (rear - x) / (rear - front), the rest on the rear; to that each group
// adds its emptyLoad or, where neither group gives one, its share of the
// tareWeight taken at the middle of the cargo space. One stage at departure,
// and one after each stop (the items with that unloadOrder taken off), stops
// in increasing order. Empty for a container without axle groups.
void axleStages(const ContainerLoadingProblem& problem, int container, const Packing& packing,
                std::vector<AxleStage>& out);

// axleLoadCost: over the stages, the largest sum over the two groups of the
// load above maxLoad, each group's rounded up to a whole weight unit (with a
// tolerance of about 1e-11 of the numbers the loads come from). 0 without axle groups
// and for a container that holds no placed item (a vehicle not on the road).
std::int64_t axleExcess(const ContainerLoadingProblem& problem, int container,
                        const Packing& packing);

// stackingLoadCost: over the placed items that set maxLoadOnTop, the weight
// resting on the item above that limit, rounded up to a whole unit. An item
// passes its own weight and what rests on it down to the items it stands on,
// shared by contact area (an item on the floor passes it to the floor).
std::int64_t stackingExcess(const ContainerLoadingProblem& problem, const Packing& packing);

// stackingRuleCost: the pairs (a, b) of placed items where a stands on b (its
// base on b's top, overlapping it in the floor plane) and a stacking rule
// forbids a's stack group on b's, plus, over the placed items that set
// maxStackLayers, the layers the tallest column through the item has beyond
// that limit. A column through an item: the item, the items under it down to
// the floor (its layer: 1 on the floor, else one above the highest item it
// stands on) and the tallest chain of items standing on it.
std::int64_t stackingViolations(const ContainerLoadingProblem& problem, const Packing& packing);

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

// balanceLimitCost: how far the placed items' centre of gravity lies beyond
// the container's maxOffsetLength along x and its maxOffsetWidth along y, in
// percent of half the length (width), each axis rounded up to a whole
// percent and the two added. 0 for a container without the limits or
// whose placed items weigh nothing.
std::int64_t balanceExcess(const ContainerLoadingProblem& problem, int container,
                           const Packing& packing);

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
    // (sizes, rotations, the support ratio, the break weights, the stack
    // heights, bottomOnly, the layer limits and stacking rules, the allowed
    // containers, the segregation classes and distances, and the weights, load
    // limits, unloading orders and balance limits the decode and the kept costs
    // read) are not the ones
    // the kept decodes were made for. The facts are compared through a
    // 64-bit hash, which is trusted: a collision (practically impossible)
    // would keep decodes made for other facts.
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
