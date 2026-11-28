#ifndef CL_DOMAIN_HPP
#define CL_DOMAIN_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/score/score.hpp"

namespace cl {

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// Two levels. hard = items that found no place, weight above a container's
// limit, weight resting on an item above what it carries, items standing on
// each other against a stacking rule or above a layer limit, items that block
// one that leaves earlier, and a load's centre of gravity beyond its limit.
// soft = the cost of the containers in use and
// the groups spread over more than one container (plus the opt-in loading
// length and load balance). The engine's optimaru::Score(int64 hard, double
// soft).
using Score = optimaru::Score;

// Lengths and weights are whole numbers in the caller's unit (the guide's
// examples use mm and g). The bounds keep one item's or one container's
// volume inside an int64 (and so the volume of what a container holds, which
// cannot exceed its own), and a hard cost far below the engine's limit for one
// contribution. Sums over a whole list (unplaced items included) or a group
// are taken in double where the moves and the construction compare them.
constexpr int kMaxLength = 1'000'000;
constexpr std::int64_t kMaxWeight = 1'000'000'000;
// The largest axle load, tare or empty load (a weight limit's bound, kept so
// that a sum over the items stays far inside an int64).
constexpr std::int64_t kMaxLimit = 1'000'000'000'000'000;
// The largest Item::maxStackLayers (the JSON model's bound).
constexpr int kMaxLayers = 1'000'000'000;
// The largest Item::priority: an unplaced item's cost stays far below the
// engine's limit for one contribution however many there are.
constexpr int kMaxPriority = 1'000'000;

// Which of an item's orientations the decoder may use.
//   All      the six axis-parallel orientations
//   Upright  the height stays vertical: as given, or turned by 90 degrees
//   Fixed    as given
enum class Rotation { All = 0, Upright = 1, Fixed = 2 };

// One orientation of an item: its size along x (the container's length), y
// (its width) and z (up).
struct Orientation {
    int length = 0;
    int width = 0;
    int height = 0;
};

// ---------------------------------------------------------------------------
// Item  (the list element)
// ---------------------------------------------------------------------------
// One box. The Solver puts every item at a position of one container's
// loading order; where it then stands is derived from that order by the
// decoder (cl/score/packing.hpp).
struct Item {
    std::string id;
    int length = 0;  // as given: along x, y and z before any rotation
    int width = 0;
    int height = 0;
    std::int64_t weight = 0;
    Rotation rotations = Rotation::All;
    // Weight this item can carry on top of it; 0 = nothing. Unset = no limit.
    std::optional<std::int64_t> maxLoadOnTop;
    // Lower leaves first (a route's stop number). Unset = no order.
    std::optional<int> unloadOrder;
    // Items of one group belong in one container (the items of an order).
    std::optional<std::string> group;
    // Only on the floor (z = 0); other items may stand on it.
    bool bottomOnly = false;
    // At most this many items in a column through this item, itself included:
    // the items under it down to the floor and those on top of it. Unset = no
    // limit.
    std::optional<int> maxStackLayers;
    // The group the stacking rules name (ContainerLoadingProblem::stackingRules).
    std::optional<std::string> stackGroup;
    // What leaving it out costs: unplacedItemCost (optionalItemCost for an
    // optional item) counts an unplaced item `priority` times. 1 to 1'000'000.
    int priority = 1;
    // Loaded where there is room, left out freely: an unplaced optional item
    // is not an unplacedItemCost but an optionalItemCost (soft), and its
    // weight does not count against its container's maxWeight. The decoder
    // also leaves it out where its weight would put the items placed so far
    // above that limit.
    bool optional = false;
    // The containers it may be loaded in, by id or by one of their tags;
    // empty = any. In another container's list it finds no place.
    std::vector<std::string> allowedContainers;
    // The class the segregation rules name (ContainerLoadingProblem::segregationRules).
    std::optional<std::string> segregationClass;

    // --- derived by ScoreCalculator::updateDerived (calculate_score, solve) ---
    int container = -1;  // index into containers; -1 = not placed
    int sequence = -1;   // 0-based place among the container's placed items
    int x = 0;
    int y = 0;
    int z = 0;
    int placedLength = 0;  // size along x, y, z after rotation
    int placedWidth = 0;
    int placedHeight = 0;

    bool isPlaced() const { return container >= 0; }
    std::int64_t volume() const {
        return static_cast<std::int64_t>(length) * width * static_cast<std::int64_t>(height);
    }
};

// One axle group of a vehicle: where it is, measured along x from the
// container's closed end (0; ahead of it is negative: a tractor's or a cab's
// axle), what it may carry, and what it carries when the vehicle is empty.
struct AxleGroup {
    std::string id;
    int position = 0;
    std::int64_t maxLoad = 0;
    std::optional<std::int64_t> emptyLoad;
};

// The axle loads of a container at one stage of its route: at departure
// (afterStop unset) and after each stop's items are unloaded. One load per
// axle group, in the order of Container::axleGroups.
struct AxleStage {
    std::optional<int> afterStop;
    std::vector<double> loads;
};

// ---------------------------------------------------------------------------
// Container  (the list owner)
// ---------------------------------------------------------------------------
// One physical space: a pallet, a rack bay, a cargo space, one carton. x runs
// along `length` from the closed end (0) to the door (`length`), y along
// `width`, z up. `items` is the decision: the item indices in loading order.
struct Container {
    std::string id;
    int length = 0;
    int width = 0;
    int height = 0;
    std::optional<std::int64_t> maxWeight;
    double cost = 1.0;  // what using this container costs
    // How high the load may reach (a hand's height), below `height`. Unset =
    // up to `height`.
    std::optional<int> maxStackHeight;
    // How far the placed items' centre of gravity may lie from the
    // container's centre, as a share of half the length (along x) and of half
    // the width (along y). Unset = no limit.
    std::optional<double> maxOffsetLength;
    std::optional<double> maxOffsetWidth;
    // What items' allowedContainers may name besides the id (a reefer, a
    // vehicle type).
    std::vector<std::string> tags;
    // The vehicle's empty weight, taken at the middle of the cargo space
    // unless the axle groups give their emptyLoad. Unset = 0.
    std::optional<std::int64_t> tareWeight;
    // Two axle groups (front, rear) or none: the load each carries follows
    // from the lever rule (axleLoadCost).
    std::vector<AxleGroup> axleGroups;
    std::vector<int> items;  // loading order (indices into ContainerLoadingProblem::items)

    // --- derived by ScoreCalculator::updateDerived (calculate_score, solve) ---
    std::vector<int> placedItems;      // the items of the list that found a place, in order
    std::vector<int> unplacedItems;    // the items of the list that found none
    std::int64_t loadedWeight = 0;     // weight of the placed items
    std::int64_t loadedVolume = 0;     // volume of the placed items
    int usedLength = 0;                // largest x + placed length; 0 when empty
    std::vector<AxleStage> axleLoads;  // with axle groups: per stage, the load on each

    // --- derived by ScoreCalculator::prepare ---
    int fillHeight = 0;  // what the decoder fills up to: maxStackHeight, at most height

    std::int64_t volume() const {
        return static_cast<std::int64_t>(length) * width * static_cast<std::int64_t>(height);
    }
    double fillRatio() const {
        return static_cast<double>(loadedVolume) / static_cast<double>(volume());
    }
};

// How the items of one group (Item::group) are to be loaded: all of them or
// none (incompleteGroupCost), and all in one container (singleContainerCost).
struct GroupRule {
    std::string id;
    bool allOrNone = false;
    bool singleContainer = false;
};

// One segregation rule: items of classes `a` and `b` (the same class when
// a == b) may not share a container (minDistance unset), or must stand at
// least minDistance apart in the floor plane.
struct SegregationRule {
    std::string a;
    std::string b;
    std::optional<int> minDistance;
};

// One stacking rule: items whose stackGroup is `top` may not stand on items
// whose stackGroup is `bottom` (rest on them, base on top). Anything not
// forbidden is allowed.
struct StackingRule {
    std::string top;
    std::string bottom;
};

// ---------------------------------------------------------------------------
// ContainerLoadingProblem
// ---------------------------------------------------------------------------
struct ContainerLoadingProblem {
    std::vector<Container> containers;
    std::vector<Item> items;
    // The share of an item's base that must rest on the floor or on other
    // items, in (0, 1].
    double minSupportRatio = 0.8;
    std::vector<StackingRule> stackingRules;
    std::vector<GroupRule> groupRules;
    std::vector<SegregationRule> segregationRules;

    Score score;

    // --- derived by ScoreCalculator::prepare (every solve and every scoring call) ---
    // The orientations the decoder tries for item i, in the order it tries
    // them: orientations[orientationStart[i] .. orientationStart[i + 1]).
    std::vector<Orientation> orientations;
    std::vector<int> orientationStart;
    std::vector<int> smallestSide;  // per item: its shortest side
    std::vector<int> groupOf;       // per item: its group, numbered densely; -1 = none
    int groupCount = 0;
    bool anyLoadLimit = false;    // some item sets maxLoadOnTop
    bool anyUnloadOrder = false;  // some item sets unloadOrder
    bool anyLayerLimit = false;   // some item sets maxStackLayers
    // Per item: its stackGroup numbered densely among the groups the rules
    // name (-1 = none, or one no rule names), and whether items of group t may
    // stand on items of group b: forbidden[t * stackGroupCount + b].
    std::vector<int> stackGroupOf;
    int stackGroupCount = 0;
    std::vector<char> forbidden;
    bool anyStackRule = false;  // some rule names the groups of two items
    bool anyOptional = false;   // some item is optional
    bool anyPriority = false;   // some item has a priority other than 1
    // Per group (groupOf): whether its rule asks for all or none, and for one
    // container.
    std::vector<char> groupAllOrNone;
    std::vector<char> groupSingleContainer;
    bool anyAllOrNone = false;
    bool anySingleContainer = false;
    // Per item and container: whether the item may go in it,
    // allowedIn[item * containers + container]; empty when every item may go
    // anywhere.
    std::vector<char> allowedIn;
    // Per item: its segregation class numbered densely among the classes the
    // rules name (-1 = none); per pair of classes, the distance they must
    // keep: 0 = none, -1 = not in one container, else the distance:
    // apart[a * segregationClassCount + b].
    std::vector<int> segregationOf;
    int segregationClassCount = 0;
    std::vector<int> apart;
    bool anySegregation = false;      // some item has a class a rule names
    bool anyApartDistance = false;    // some rule keeps a distance
    std::vector<char> keepsDistance;  // per segregation class: some rule gives it a distance

    bool allowed(int item, int container) const {
        return allowedIn.empty() || allowedIn[static_cast<std::size_t>(item) * containers.size() +
                                              static_cast<std::size_t>(container)] != 0;
    }
    // What items a and b must keep: 0 nothing, -1 another container, d > 0 a
    // distance of d.
    int separation(int a, int b) const {
        const int ca = segregationOf[static_cast<std::size_t>(a)];
        const int cb = segregationOf[static_cast<std::size_t>(b)];
        if (ca < 0 || cb < 0) return 0;
        return apart[static_cast<std::size_t>(ca) *
                             static_cast<std::size_t>(segregationClassCount) +
                     static_cast<std::size_t>(cb)];
    }

    // Whether item `top` standing on item `bottom` breaks a stacking rule.
    bool forbids(int top, int bottom) const {
        const int t = stackGroupOf[static_cast<std::size_t>(top)];
        const int b = stackGroupOf[static_cast<std::size_t>(bottom)];
        return t >= 0 && b >= 0 &&
               forbidden[static_cast<std::size_t>(t) * static_cast<std::size_t>(stackGroupCount) +
                         static_cast<std::size_t>(b)];
    }
    // Whether the decode keeps what every box stands on.
    bool trackSupports() const { return anyLoadLimit || anyLayerLimit; }

    int containersUsed() const {
        int used = 0;
        for (const Container& container : containers) used += container.placedItems.empty() ? 0 : 1;
        return used;
    }
};

}  // namespace cl

// ---------------------------------------------------------------------------
// SolutionTraits: the containers are the list owners, the items the elements.
// An item is in at most one container's list; one in no list is unassigned,
// which happens only while the solution is being constructed or a ruin is
// being rebuilt.
// ---------------------------------------------------------------------------
template <>
struct optimaru::SolutionTraits<cl::ContainerLoadingProblem> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;
    static int list_count(const cl::ContainerLoadingProblem& p) {
        return static_cast<int>(p.containers.size());
    }
    static int element_count(const cl::ContainerLoadingProblem& p) {
        return static_cast<int>(p.items.size());
    }
    static int list_size(const cl::ContainerLoadingProblem& p, int list) {
        return static_cast<int>(p.containers[list].items.size());
    }
    static int element_at(const cl::ContainerLoadingProblem& p, int list, int position) {
        return p.containers[list].items[position];
    }
    static void insert_element(cl::ContainerLoadingProblem& p, int list, int position,
                               int element) {
        auto& items = p.containers[list].items;
        items.insert(items.begin() + position, element);
    }
    static int remove_element(cl::ContainerLoadingProblem& p, int list, int position) {
        auto& items = p.containers[list].items;
        const int element = items[position];
        items.erase(items.begin() + position);
        return element;
    }
    static void reverse_segment(cl::ContainerLoadingProblem& p, int list, int first, int last) {
        auto& items = p.containers[list].items;
        std::reverse(items.begin() + first, items.begin() + last + 1);
    }
};

#endif  // CL_DOMAIN_HPP
