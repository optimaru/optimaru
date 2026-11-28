#ifndef CL_DOMAIN_HPP
#define CL_DOMAIN_HPP

#include <algorithm>
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
// limit, weight resting on an item above what it carries, and items that
// block one that leaves earlier. soft = the cost of the containers in use and
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
    double cost = 1.0;       // what using this container costs
    std::vector<int> items;  // loading order (indices into ContainerLoadingProblem::items)

    // --- derived by ScoreCalculator::updateDerived (calculate_score, solve) ---
    std::vector<int> placedItems;    // the items of the list that found a place, in order
    std::vector<int> unplacedItems;  // the items of the list that found none
    std::int64_t loadedWeight = 0;   // weight of the placed items
    std::int64_t loadedVolume = 0;   // volume of the placed items
    int usedLength = 0;              // largest x + placed length; 0 when empty

    std::int64_t volume() const {
        return static_cast<std::int64_t>(length) * width * static_cast<std::int64_t>(height);
    }
    double fillRatio() const {
        return static_cast<double>(loadedVolume) / static_cast<double>(volume());
    }
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
