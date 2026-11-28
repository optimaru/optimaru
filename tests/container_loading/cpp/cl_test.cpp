// Container-loading tests, engine test/ style: check() + a non-zero exit on any failure.
//   (a) the decoder by hand: the (x, z, y) order of the points, the support
//       rule, the rotations, which orientation is taken, an item that finds
//       no place, bottomOnly, maxStackHeight, stacking rules and layer
//       limits, an optional item above the weight limit, allowed containers,
//       a segregation distance
//   (b) the decoder on random lists, short and long, a third of them with
//       stacking rules, layer limits, bottomOnly items and a stack height:
//       every decode is a valid loading (inside the container and below its
//       stack height, no overlap, enough support, bottomOnly on the floor), a decode
//       resumed from any shared prefix equals a decode from scratch, and the
//       room every free point keeps equals the room measured afresh
//   (c) the costs of one decoded container by hand: stacking load (passed
//       down, shared by contact area, exact at loads of 5.6e10), unloading
//       order (in front, on top),
//       used length, load balance, stacking rules and layer limits, the
//       balance limit, priorities and optional items, all-or-none and
//       single-container groups, segregation
//   (d) PackingCache: what it returns is decode()'s result, a repeated list
//       is a hit, changed facts drop the decodes
//   (e) ScoreCalculator: prepare's refusals, the derived fields, the totals
//   (f) the construction: every item listed once, largest first, a seeded
//       list kept, later stops loaded first, deterministic
//   (g) the moves: EmptyContainerMove empties a container, RepackPairMove
//       gathers the groups two containers share, SwitchContainerMove moves a
//       load to a cheaper container or splits it over two (never over ones
//       that cost more together), StopOrderMove puts a load back in stop
//       blocks, all undo exactly
//   (h) the solver: the demo under ScoreChecks::Full, a zero budget returns the
//       construction, parallel starts return the best single start
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cl/constraints.hpp"
#include "cl/construct/construction.hpp"
#include "cl/model/demo_data.hpp"
#include "cl/model/domain.hpp"
#include "cl/score/packing.hpp"
#include "cl/score/score_calculator.hpp"
#include "cl/search/moves/empty_container.hpp"
#include "cl/search/moves/repack_pair.hpp"
#include "cl/search/moves/stop_order.hpp"
#include "cl/search/moves/switch_container.hpp"
#include "cl/solver.hpp"
#include "optimaru/solution/editor.hpp"
#include "optimaru/solution/neighborhoods.hpp"
#include "optimaru/testing/check.hpp"

using namespace cl;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

int addContainer(ContainerLoadingProblem& p, int length, int width, int height) {
    Container container;
    container.id = "c" + std::to_string(p.containers.size());
    container.length = length;
    container.width = width;
    container.height = height;
    p.containers.push_back(container);
    return static_cast<int>(p.containers.size()) - 1;
}

int addItem(ContainerLoadingProblem& p, int length, int width, int height,
            Rotation rotations = Rotation::Fixed, std::int64_t weight = 0) {
    Item item;
    item.id = "i" + std::to_string(p.items.size());
    item.length = length;
    item.width = width;
    item.height = height;
    item.weight = weight;
    item.rotations = rotations;
    p.items.push_back(item);
    return static_cast<int>(p.items.size()) - 1;
}

// Every item of the problem, in index order, as container 0's list.
void loadAll(ContainerLoadingProblem& p) {
    p.containers[0].items.clear();
    for (std::size_t i = 0; i < p.items.size(); ++i)
        p.containers[0].items.push_back(static_cast<int>(i));
}

bool at(const Item& item, int x, int y, int z) {
    return item.isPlaced() && item.x == x && item.y == y && item.z == z;
}

bool sized(const Item& item, int length, int width, int height) {
    return item.placedLength == length && item.placedWidth == width && item.placedHeight == height;
}

bool sameBoxes(const Packing& a, const Packing& b) {
    if (a.boxes.size() != b.boxes.size() || a.boxOf != b.boxOf) return false;
    for (std::size_t i = 0; i < a.boxes.size(); ++i) {
        const Box& p = a.boxes[i];
        const Box& q = b.boxes[i];
        if (p.item != q.item || p.position != q.position || p.x != q.x || p.y != q.y ||
            p.z != q.z || p.length != q.length || p.width != q.width || p.height != q.height)
            return false;
    }
    return a.loadedVolume == b.loadedVolume && a.loadedWeight == b.loadedWeight &&
           a.usedLength == b.usedLength;
}

// The free points of a decode, which are what the next item would see.
std::vector<std::vector<int>> freePoints(const Packing& packing) {
    std::vector<std::vector<int>> points;
    for (const Point& point : packing.points) {
        if (point.died == Point::kOpen) points.push_back({point.x, point.y, point.z});
    }
    return points;
}

// The free points whose kept room (Point::room, once measured) differs from
// the room measured afresh: 0 when the decoder's cache is current.
int staleRooms(const ContainerLoadingProblem& p, int container, const Packing& packing) {
    int stale = 0;
    for (const Point& point : packing.points) {
        if (point.died != Point::kOpen || point.room[0] < 0) continue;
        int room[3];
        measureRoom(p, container, packing, point, room);
        if (room[0] != point.room[0] || room[1] != point.room[1] || room[2] != point.room[2])
            ++stale;
    }
    return stale;
}

int overlap(int a, int la, int b, int lb) {
    return std::max(0, std::min(a + la, b + lb) - std::max(a, b));
}

// A loading checked without the decoder: inside the walls and below the
// stack height, no two items overlapping, every base supported by at least
// the problem's ratio, every bottomOnly item on the floor.
bool validLoading(const ContainerLoadingProblem& p, int container, const Packing& packing) {
    const Container& space = p.containers[static_cast<std::size_t>(container)];
    const int top =
            space.maxStackHeight ? std::min(space.height, *space.maxStackHeight) : space.height;
    for (std::size_t i = 0; i < packing.boxes.size(); ++i) {
        const Box& box = packing.boxes[i];
        if (box.x < 0 || box.y < 0 || box.z < 0 || box.x + box.length > space.length ||
            box.y + box.width > space.width || box.z + box.height > top)
            return false;
        if (p.items[static_cast<std::size_t>(box.item)].bottomOnly && box.z > 0) return false;
        double supported = 0.0;
        for (std::size_t j = 0; j < packing.boxes.size(); ++j) {
            if (i == j) continue;
            const Box& other = packing.boxes[j];
            const int dx = overlap(box.x, box.length, other.x, other.length);
            const int dy = overlap(box.y, box.width, other.y, other.width);
            const int dz = overlap(box.z, box.height, other.z, other.height);
            if (dx > 0 && dy > 0 && dz > 0) return false;
            if (other.z + other.height == box.z) supported += static_cast<double>(dx) * dy;
        }
        const double base = static_cast<double>(box.length) * box.width;
        if (box.z > 0 && supported < p.minSupportRatio * base - 1e-6) return false;
    }
    return true;
}

bool throwsInvalid(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

// A random problem: `containers` spaces of 12 x 8 x 10 and `items` boxes with
// sides of 1 to 6, mixed rotations.
ContainerLoadingProblem randomProblem(std::mt19937_64& rng, int containers, int items) {
    ContainerLoadingProblem p;
    for (int c = 0; c < containers; ++c) addContainer(p, 12, 8, 10);
    for (int i = 0; i < items; ++i) {
        const auto side = [&rng] { return 1 + static_cast<int>(rng() % 6); };
        const Rotation rotation = static_cast<Rotation>(rng() % 3);
        const int index = addItem(p, side(), side(), side(), rotation,
                                  static_cast<std::int64_t>(1 + rng() % 9));
        if (rng() % 4 == 0)
            p.items[static_cast<std::size_t>(index)].maxLoadOnTop =
                    static_cast<std::int64_t>(rng() % 12);
        if (rng() % 3 == 0)
            p.items[static_cast<std::size_t>(index)].unloadOrder = static_cast<int>(rng() % 4);
        if (rng() % 2 == 0)
            p.items[static_cast<std::size_t>(index)].group = "g" + std::to_string(rng() % 5);
    }
    ScoreCalculator::prepare(p);
    return p;
}

// Stacking facts on a random problem, drawn from their own generator:
// bottomOnly items, layer limits, stack groups with two rules among them, and
// now and then a stack height.
void addStacking(ContainerLoadingProblem& p, std::mt19937_64& rng) {
    for (Item& item : p.items) {
        item.bottomOnly = rng() % 8 == 0;
        if (rng() % 4 == 0) item.maxStackLayers = 1 + static_cast<int>(rng() % 3);
        if (rng() % 2 == 0) item.stackGroup = "s" + std::to_string(rng() % 3);
    }
    p.stackingRules = {{"s0", "s1"}, {"s2", "s2"}};
    for (Container& container : p.containers) {
        if (rng() % 3 == 0) container.maxStackHeight = container.height - 2;
    }
    ScoreCalculator::prepare(p);
}

// -------------------------------------------------------------------------
// (a) The decoder by hand.
// -------------------------------------------------------------------------
void verifyDecoderByHand() {
    std::printf("== (a) The decoder by hand ==\n");
    {
        // Eight cubes of 5 fill a 10 x 10 x 10 space; the ninth finds no
        // place. Points go by (x, z, y): across first, then up, then towards
        // the door.
        ContainerLoadingProblem p;
        addContainer(p, 10, 10, 10);
        for (int i = 0; i < 9; ++i) addItem(p, 5, 5, 5);
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[0], 0, 0, 0) && at(p.items[1], 0, 5, 0) && at(p.items[2], 0, 0, 5) &&
                      at(p.items[3], 0, 5, 5),
              "the first four cubes build the wall at the closed end: across, then up");
        check(at(p.items[4], 5, 0, 0) && at(p.items[5], 5, 5, 0) && at(p.items[6], 5, 0, 5) &&
                      at(p.items[7], 5, 5, 5),
              "the next four build the wall in front of it");
        check(!p.items[8].isPlaced() && p.containers[0].unplacedItems == std::vector<int>{8} &&
                      p.containers[0].placedItems.size() == 8,
              "the ninth cube finds no place and stays in the list");
        check(p.items[5].sequence == 5 && p.items[5].container == 0 &&
                      p.containers[0].loadedVolume == 1000 && p.containers[0].usedLength == 10 &&
                      p.containers[0].fillRatio() == 1.0,
              "sequence, container, loaded volume, used length and fill ratio are derived");
    }
    {
        // A base of 4 x 4 carries 16 of the 36 of a 6 x 6 item: not enough at
        // the default 0.8, enough at 0.4.
        ContainerLoadingProblem p;
        addContainer(p, 6, 6, 10);
        addItem(p, 4, 4, 4);
        addItem(p, 6, 6, 2);
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[0], 0, 0, 0) && !p.items[1].isPlaced(),
              "an item with 44% of its base supported is not placed at ratio 0.8");
        p.minSupportRatio = 0.4;
        ScoreCalculator::score(p);
        check(at(p.items[1], 0, 0, 4), "the same item is placed on top at ratio 0.4");
    }
    {
        // 4 x 4 x 10 into a space of 10 x 4 x 4: only lying down.
        ContainerLoadingProblem p;
        addContainer(p, 10, 4, 4);
        addItem(p, 4, 4, 10, Rotation::All);
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[0], 0, 0, 0) && sized(p.items[0], 10, 4, 4),
              "rotations all: the item is laid along the length");
        p.items[0].rotations = Rotation::Upright;
        ScoreCalculator::score(p);
        check(!p.items[0].isPlaced(), "rotations upright: the height stays up, no place");
        p.items[0].rotations = Rotation::Fixed;
        ScoreCalculator::score(p);
        check(!p.items[0].isPlaced(), "rotations fixed: no place");
    }
    {
        // Upright turns the base: 6 x 3 x 2 into a space 3 long and 6 wide.
        ContainerLoadingProblem p;
        addContainer(p, 3, 6, 2);
        addItem(p, 6, 3, 2, Rotation::Upright);
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[0], 0, 0, 0) && sized(p.items[0], 3, 6, 2),
              "rotations upright: the base is turned by 90 degrees");
    }
    {
        // The order of the orientations: the lowest first, and of two equally
        // low ones the one that reaches less far towards the door.
        ContainerLoadingProblem p;
        addContainer(p, 10, 10, 10);
        addItem(p, 2, 3, 5, Rotation::All);
        ScoreCalculator::prepare(p);
        const Orientation& first = p.orientations[0];
        const Orientation& second = p.orientations[1];
        check(p.orientationStart[1] == 6 && first.height == 2 && first.length == 3 &&
                      first.width == 5 && second.height == 2 && second.length == 5,
              "six orientations, the lowest first, the shorter one along x before the longer");
        ContainerLoadingProblem cube;
        addContainer(cube, 10, 10, 10);
        addItem(cube, 4, 4, 4, Rotation::All);
        addItem(cube, 2, 2, 7, Rotation::Upright);
        ScoreCalculator::prepare(cube);
        check(cube.orientationStart[1] == 1 && cube.orientationStart[2] == 2,
              "equal orientations are kept once");
    }
    {
        // Of the feasible orientations, the one that divides the free room
        // best: eight boxes of 2 x 3 x 3 fill a space of 4 x 6 x 6 only laid
        // 2 long, which is not the lowest orientation.
        ContainerLoadingProblem p;
        addContainer(p, 4, 6, 6);
        for (int i = 0; i < 8; ++i) addItem(p, 3, 3, 2, Rotation::All);
        loadAll(p);
        ScoreCalculator::score(p);
        check(sized(p.items[0], 2, 3, 3) && p.containers[0].unplacedItems.empty() &&
                      p.containers[0].fillRatio() == 1.0,
              "the orientation that divides the free room is taken: eight boxes fill the space");
        // In a space nothing divides, the lowest orientation is taken.
        ContainerLoadingProblem q;
        addContainer(q, 7, 7, 7);
        addItem(q, 3, 3, 2, Rotation::All);
        loadAll(q);
        ScoreCalculator::score(q);
        check(sized(q.items[0], 3, 3, 2), "without a better fit, the lowest orientation is taken");
    }
    {
        // A stop-1 box finds the hole at the closed end beside the stop-2
        // box, but that hole is behind the stop-2 box in front: it is
        // skipped for the floor in front of everything.
        ContainerLoadingProblem p;
        addContainer(p, 12, 4, 4);
        const int back = addItem(p, 2, 2, 4);
        const int front = addItem(p, 2, 4, 4);
        const int early = addItem(p, 2, 2, 4);
        p.items[static_cast<std::size_t>(back)].unloadOrder = 3;
        p.items[static_cast<std::size_t>(front)].unloadOrder = 2;
        p.items[static_cast<std::size_t>(early)].unloadOrder = 1;
        p.containers[0].items = {back, front, early};
        ScoreCalculator::score(p);
        check(at(p.items[1], 2, 0, 0) && at(p.items[2], 4, 0, 0) &&
                      ScoreCalculator::calculateTotals(p).unloadingOrderCost == 0,
              "unloading orders: an earlier stop skips the hole behind a later one for the room "
              "in front");
        p.items[static_cast<std::size_t>(front)].unloadOrder.reset();
        ScoreCalculator::score(p);
        check(at(p.items[2], 0, 2, 0),
              "the hole is taken when what stands in front of it has no order");
        // An earlier stop with no clean place left still finds one.
        ContainerLoadingProblem q;
        addContainer(q, 4, 4, 4);
        const int later = addItem(q, 2, 4, 4);
        const int earlier = addItem(q, 2, 4, 4);
        q.items[static_cast<std::size_t>(later)].unloadOrder = 1;
        q.items[static_cast<std::size_t>(earlier)].unloadOrder = 2;
        q.containers[0].items = {later, earlier};
        ScoreCalculator::score(q);
        check(at(q.items[1], 2, 0, 0) &&
                      ScoreCalculator::calculateTotals(q).unloadingOrderCost == 1,
              "with no place that keeps the order, the first feasible one is taken");
    }
    {
        // Load limits: a box that carries nothing on the floor, then a heavy
        // box that fits on top of it or beside it. It goes beside it; once
        // the floor is full, on top (an item is not left out for a limit).
        ContainerLoadingProblem p;
        addContainer(p, 4, 2, 4);
        const int fragile = addItem(p, 2, 2, 2, Rotation::Fixed, 1);
        addItem(p, 2, 2, 2, Rotation::Fixed, 5);
        addItem(p, 2, 2, 2, Rotation::Fixed, 5);
        p.items[static_cast<std::size_t>(fragile)].maxLoadOnTop = 0;
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[1], 2, 0, 0) && at(p.items[2], 2, 0, 2),
              "load limits: a heavy box goes beside the fragile one and on the other, not on it");
        addItem(p, 2, 2, 2, Rotation::Fixed, 5);
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[3], 0, 0, 2) && ScoreCalculator::calculateTotals(p).stackingLoadCost == 5,
              "with no other place left it goes on the fragile box, and the cost counts it");
        // A limit of 6 on the box under the heavy pair: the third box's
        // weight passed down through the second would exceed it.
        ContainerLoadingProblem q;
        addContainer(q, 4, 2, 6);
        const int base = addItem(q, 2, 2, 2, Rotation::Fixed, 1);
        addItem(q, 2, 2, 2, Rotation::Fixed, 5);
        addItem(q, 2, 2, 2, Rotation::Fixed, 5);
        q.items[static_cast<std::size_t>(base)].maxLoadOnTop = 6;
        q.containers[0].items = {base, 1, 2};
        // Point order: the second goes on the base (5 <= 6), so the third,
        // which would make it 10, goes to the floor beside them.
        ScoreCalculator::score(q);
        check(at(q.items[1], 0, 0, 2) && at(q.items[2], 2, 0, 0) &&
                      ScoreCalculator::calculateTotals(q).stackingLoadCost == 0,
              "a load passed down through another box counts against the limit below");
    }
    {
        // bottomOnly: the second box would go on the first; bottomOnly sends
        // it to the floor beside it, and leaves it out when there is none.
        ContainerLoadingProblem p;
        addContainer(p, 8, 4, 8);
        addItem(p, 4, 4, 4);
        const int floor = addItem(p, 4, 4, 4);
        p.items[static_cast<std::size_t>(floor)].bottomOnly = true;
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[1], 4, 0, 0), "bottomOnly: on the floor beside, not on top");
        p.containers[0].length = 4;
        ScoreCalculator::score(p);
        check(!p.items[1].isPlaced(), "bottomOnly: no floor left, no place");
        // maxStackHeight: two cubes of 4 in a space 10 high, up to 6.
        ContainerLoadingProblem q;
        addContainer(q, 4, 4, 10);
        addItem(q, 4, 4, 4);
        addItem(q, 4, 4, 4);
        loadAll(q);
        ScoreCalculator::score(q);
        check(at(q.items[1], 0, 0, 4), "without a stack height the second cube goes on top");
        q.containers[0].maxStackHeight = 6;
        ScoreCalculator::score(q);
        check(!q.items[1].isPlaced() && q.containers[0].fillHeight == 6,
              "maxStackHeight 6: no room for a second layer of 4");
    }
    {
        // Stacking rules: "chem" may not stand on "food". The chem box goes
        // beside the food box; with no room beside it, on it, and the cost
        // counts the pair.
        ContainerLoadingProblem p;
        addContainer(p, 8, 4, 8);
        const int food = addItem(p, 4, 4, 4);
        const int chem = addItem(p, 4, 4, 4);
        p.items[static_cast<std::size_t>(food)].stackGroup = "food";
        p.items[static_cast<std::size_t>(chem)].stackGroup = "chem";
        p.stackingRules = {{"chem", "food"}};
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[1], 4, 0, 0) && ScoreCalculator::calculateTotals(p).stackingRuleCost == 0,
              "stacking rules: the forbidden box goes beside, not on top");
        p.stackingRules = {{"food", "chem"}};
        ScoreCalculator::score(p);
        check(at(p.items[1], 0, 0, 4), "a rule the other way round does not hold it back");
        p.stackingRules = {{"chem", "food"}};
        p.containers[0].length = 4;
        ScoreCalculator::score(p);
        check(at(p.items[1], 0, 0, 4) && ScoreCalculator::calculateTotals(p).stackingRuleCost == 1,
              "no other place: on top all the same, and one pair counted");
        // Layer limits: the bottom box allows 2 layers, so the third box goes
        // to the floor beside the column; in a space without a floor left,
        // on top, one layer too many.
        ContainerLoadingProblem q;
        addContainer(q, 8, 4, 12);
        const int base = addItem(q, 4, 4, 4);
        addItem(q, 4, 4, 4);
        addItem(q, 4, 4, 4);
        q.items[static_cast<std::size_t>(base)].maxStackLayers = 2;
        loadAll(q);
        ScoreCalculator::score(q);
        check(at(q.items[1], 0, 0, 4) && at(q.items[2], 4, 0, 0) &&
                      ScoreCalculator::calculateTotals(q).stackingRuleCost == 0,
              "layer limits: the third box starts a column of its own");
        q.containers[0].length = 4;
        ScoreCalculator::score(q);
        check(at(q.items[2], 0, 0, 8) && ScoreCalculator::calculateTotals(q).stackingRuleCost == 1,
              "no other place: a third layer, one too many");
        // The new box's own limit: it allows 1 layer, so not on top of another.
        q.containers[0].length = 8;
        q.items[static_cast<std::size_t>(base)].maxStackLayers.reset();
        q.items[1].maxStackLayers = 1;
        ScoreCalculator::score(q);
        check(at(q.items[1], 4, 0, 0) && at(q.items[2], 0, 0, 4),
              "a box that allows one layer stays on the floor, and nothing on it");
    }
    {
        // Optional items and the weight limit: 6 + 6 against 10. A second
        // required item is placed (and overweight); a second optional one is
        // left out although there is room.
        ContainerLoadingProblem p;
        addContainer(p, 8, 4, 4);
        p.containers[0].maxWeight = 10;
        addItem(p, 4, 4, 4, Rotation::Fixed, 6);
        addItem(p, 4, 4, 4, Rotation::Fixed, 6);
        loadAll(p);
        ScoreCalculator::score(p);
        check(p.items[1].isPlaced() && ScoreCalculator::calculateTotals(p).containerWeightCost == 2,
              "a required item is placed whatever it weighs");
        p.items[1].optional = true;
        ScoreCalculator::score(p);
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(p);
        check(!p.items[1].isPlaced() && totals.containerWeightCost == 0 &&
                      totals.optionalItemCost == 1 && totals.unplacedItemCost == 0,
              "an optional item that would go over the limit is left out, and its weight with it");
        p.containers[0].maxWeight = 12;
        ScoreCalculator::score(p);
        check(p.items[1].isPlaced(), "within the limit it is loaded");
    }
    {
        // Allowed containers: an item for "reefer" containers only finds no
        // place in a dry one, and one in a container tagged "reefer".
        ContainerLoadingProblem p;
        addContainer(p, 8, 4, 4);
        addContainer(p, 8, 4, 4);
        p.containers[1].tags = {"reefer"};
        addItem(p, 4, 4, 4);
        p.items[0].allowedContainers = {"reefer"};
        p.containers[0].items = {0};
        ScoreCalculator::score(p);
        check(!p.items[0].isPlaced(), "allowed containers: no place outside them");
        p.containers[0].items = {};
        p.containers[1].items = {0};
        ScoreCalculator::score(p);
        check(p.items[0].isPlaced(), "allowed containers: placed in one tagged so");
        p.items[0].allowedContainers = {"c0"};
        p.containers[0].items = {0};
        p.containers[1].items = {};
        ScoreCalculator::score(p);
        check(p.items[0].isPlaced(), "allowed containers: an id names a container too");
    }
    {
        // A segregation distance of 4: chemicals, a box without a class, then
        // food. Without the rule the food box takes the corner beside the
        // chemicals; with it, the corner past the other box, 4 away.
        ContainerLoadingProblem p;
        addContainer(p, 12, 8, 4);
        addItem(p, 4, 4, 4);
        addItem(p, 4, 8, 4);  // across the width, beside the chemicals
        addItem(p, 4, 4, 4);
        p.items[0].segregationClass = "chem";
        p.items[2].segregationClass = "food";
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[2], 0, 4, 0), "without a rule the food box goes beside the chemicals");
        p.segregationRules = {{"chem", "food", 4}};
        ScoreCalculator::score(p);
        check(at(p.items[2], 8, 0, 0) && ScoreCalculator::calculateTotals(p).segregationCost == 0,
              "segregation: it keeps its distance where a corner allows it");
        p.containers[0].length = 8;
        ScoreCalculator::score(p);
        check(at(p.items[1], 4, 0, 0) && at(p.items[2], 0, 4, 0) &&
                      ScoreCalculator::calculateTotals(p).segregationCost == 1,
              "no corner far enough: beside it all the same, one pair counted");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) The decoder on random lists.
// -------------------------------------------------------------------------
void verifyDecoderProperties() {
    std::printf("== (b) Random decodes: valid loadings, resumable from any prefix ==\n");
    std::mt19937_64 rng(11);
    std::mt19937_64 stackRng(12);
    int invalid = 0;
    int resumeMismatches = 0;
    int staleRoomCount = 0;
    int placed = 0;
    int unplaced = 0;
    // 300 short lists, then 40 long ones in a longer space (100 to 300 items,
    // so the decode files the boxes in 6 to 18 cells and boxes span several).
    const int rounds = 340;
    for (int round = 0; round < rounds; ++round) {
        const bool longList = round >= 300;
        ContainerLoadingProblem p = randomProblem(
                rng, 1,
                longList ? 100 + static_cast<int>(rng() % 201) : 5 + static_cast<int>(rng() % 30));
        if (longList) {
            p.containers[0].length = 60;
            p.containers[0].width = 10;
            p.containers[0].height = 12;
        }
        p.minSupportRatio = round % 3 == 0 ? 1.0 : round % 3 == 1 ? 0.8 : 0.5;
        if (round % 3 == 2) addStacking(p, stackRng);
        std::vector<int> list(p.items.size());
        for (std::size_t i = 0; i < list.size(); ++i) list[i] = static_cast<int>(i);
        std::shuffle(list.begin(), list.end(), rng);
        Packing base;
        decode(p, 0, list, base);
        if (!validLoading(p, 0, base)) ++invalid;
        staleRoomCount += staleRooms(p, 0, base);
        placed += static_cast<int>(base.boxes.size());
        unplaced += base.unplaced();

        // Another list that shares a prefix with it: decoded from scratch, and
        // resumed from every length up to the shared one.
        std::vector<int> other = list;
        const std::size_t cut = rng() % (list.size() + 1);
        std::shuffle(other.begin() + static_cast<std::ptrdiff_t>(cut), other.end(), rng);
        if (rng() % 2 == 0 && other.size() > cut) other.pop_back();
        // Now and then one item fewer than a multiple of 16, so the cells are
        // filed again between the two decodes.
        if (longList && round % 4 == 0 && other.size() % 16 == 0 && other.size() > cut)
            other.pop_back();
        std::size_t common = 0;
        while (common < other.size() && common < list.size() && other[common] == list[common])
            ++common;
        Packing fresh;
        decode(p, 0, other, fresh);
        for (std::size_t keep :
             {common, common / 2, static_cast<std::size_t>(rng() % (common + 1)), std::size_t{0}}) {
            Packing resumed = base;
            decode(p, 0, other, resumed, static_cast<int>(keep));
            if (!sameBoxes(resumed, fresh) || freePoints(resumed) != freePoints(fresh) ||
                resumed.level != fresh.level || resumed.above != fresh.above)
                ++resumeMismatches;
            staleRoomCount += staleRooms(p, 0, resumed);
        }
    }
    std::printf("    %d items placed, %d unplaced\n", placed, unplaced);
    check(invalid == 0,
          std::to_string(rounds) +
                  " random lists decode to valid loadings (checked without the decoder)");
    check(placed > 1000 && unplaced > 100, "the lists exercise placed and unplaced items");
    check(resumeMismatches == 0,
          "a decode resumed from a shared prefix equals the decode from scratch");
    check(staleRoomCount == 0,
          "every free point's kept room equals the room measured afresh, resumed or not");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) The costs of one decoded container by hand.
// -------------------------------------------------------------------------
void verifyCostsByHand() {
    std::printf("== (c) Costs of a decoded container by hand ==\n");
    {
        // A column: a (10, carries 5) under b (8) under c (2). The load on a
        // is 10, on b 2.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 12);
        const int a = addItem(p, 4, 4, 4, Rotation::Fixed, 10);
        const int b = addItem(p, 4, 4, 4, Rotation::Fixed, 8);
        addItem(p, 4, 4, 4, Rotation::Fixed, 2);
        p.items[static_cast<std::size_t>(a)].maxLoadOnTop = 5;
        p.items[static_cast<std::size_t>(b)].maxLoadOnTop = 2;
        loadAll(p);
        check(ScoreCalculator::calculateTotals(p).stackingLoadCost == 5,
              "stacking: a column passes its weight down (10 on a limit of 5, 2 on a limit of 2)");
        p.items[static_cast<std::size_t>(a)].maxLoadOnTop = 10;
        check(ScoreCalculator::calculateTotals(p).stackingLoadCost == 0,
              "stacking: 0 when every limit holds");
        p.items[static_cast<std::size_t>(b)].maxLoadOnTop = 0;
        check(ScoreCalculator::calculateTotals(p).stackingLoadCost == 2,
              "stacking: a limit of 0 means nothing on top");
    }
    {
        // Two bases side by side carry one item of 9 across both: 4.5 each.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 6);
        const int left = addItem(p, 4, 2, 4, Rotation::Fixed, 1);
        const int right = addItem(p, 4, 2, 4, Rotation::Fixed, 1);
        addItem(p, 4, 4, 2, Rotation::Fixed, 9);
        p.items[static_cast<std::size_t>(left)].maxLoadOnTop = 4;
        p.items[static_cast<std::size_t>(right)].maxLoadOnTop = 5;
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[1], 0, 2, 0) && at(p.items[2], 0, 0, 4),
              "two bases and one item across them");
        check(ScoreCalculator::calculateTotals(p).stackingLoadCost == 1,
              "stacking: the load is shared by contact area, an excess of 0.5 counts 1");
    }
    {
        // Heavy loads: 56 slabs of 997,041,038 on five supports of widths
        // 6, 9, 8, 9 and 5 over a base that carries exactly their weight. The
        // shares are not whole numbers, and in double they add up to about
        // 8e-6 more than the total: the tolerance has to grow with the load.
        constexpr int kSlabs = 56;
        constexpr std::int64_t kSlabWeight = 997'041'038;
        const int widths[] = {6, 9, 8, 9, 5};
        ContainerLoadingProblem p;
        addContainer(p, 1, 37, 2 + kSlabs);
        const int base = addItem(p, 1, 37, 1, Rotation::Fixed, 1);
        for (int width : widths) addItem(p, 1, width, 1, Rotation::Fixed, 0);
        for (int slab = 0; slab < kSlabs; ++slab)
            addItem(p, 1, 37, 1, Rotation::Fixed, kSlabWeight);
        p.items[static_cast<std::size_t>(base)].maxLoadOnTop = kSlabs * kSlabWeight;
        loadAll(p);
        ScoreCalculator::score(p);
        check(p.containers[0].unplacedItems.empty() && at(p.items[2], 0, 6, 1) &&
                      at(p.items[6], 0, 0, 2),
              "five supports side by side on the base, the slabs on them");
        check(ScoreCalculator::calculateTotals(p).stackingLoadCost == 0,
              "stacking: a base loaded to exactly its limit by 5.6e10 costs nothing");
        p.items[static_cast<std::size_t>(base)].maxLoadOnTop = kSlabs * kSlabWeight - 1;
        check(ScoreCalculator::calculateTotals(p).stackingLoadCost == 1,
              "stacking: one unit over it still counts 1");
    }
    {
        // a leaves at stop 1, b at stop 2.
        ContainerLoadingProblem p;
        addContainer(p, 10, 4, 4);
        const int a = addItem(p, 4, 4, 4);
        const int b = addItem(p, 4, 4, 4);
        p.items[static_cast<std::size_t>(a)].unloadOrder = 1;
        p.items[static_cast<std::size_t>(b)].unloadOrder = 2;
        p.containers[0].items = {a, b};
        check(ScoreCalculator::calculateTotals(p).unloadingOrderCost == 1,
              "unloading: the later item between the earlier one and the door is one block");
        p.containers[0].items = {b, a};
        check(ScoreCalculator::calculateTotals(p).unloadingOrderCost == 0,
              "unloading: loaded the other way round, nothing is in the way");
        p.items[static_cast<std::size_t>(b)].unloadOrder.reset();
        p.containers[0].items = {a, b};
        check(ScoreCalculator::calculateTotals(p).unloadingOrderCost == 0,
              "unloading: an item without an order blocks nobody");
    }
    {
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 10);
        const int a = addItem(p, 4, 4, 4);
        const int b = addItem(p, 4, 4, 4);
        p.items[static_cast<std::size_t>(a)].unloadOrder = 1;
        p.items[static_cast<std::size_t>(b)].unloadOrder = 2;
        p.containers[0].items = {a, b};
        check(ScoreCalculator::calculateTotals(p).unloadingOrderCost == 1,
              "unloading: the later item on top of the earlier one is one block");
        p.items[static_cast<std::size_t>(b)].unloadOrder = 1;
        check(ScoreCalculator::calculateTotals(p).unloadingOrderCost == 0,
              "unloading: the same stop blocks nobody");
    }
    {
        // One item of 4 x 4 x 4 in the corner of an 8 x 8 space: its centre
        // (2, 2) is half the half-diagonal away from the centre (4, 4).
        ContainerLoadingProblem p;
        addContainer(p, 8, 8, 8);
        addItem(p, 4, 4, 4, Rotation::Fixed, 7);
        loadAll(p);
        ConstraintTotals totals = ScoreCalculator::calculateTotals(p);
        check(totals.loadBalanceCost == 500 && totals.usedLengthCost == 4,
              "balance: a corner load is 500 per mille off centre; used length 4");
        p.items[0].length = p.items[0].width = 8;
        totals = ScoreCalculator::calculateTotals(p);
        check(totals.loadBalanceCost == 0 && totals.usedLengthCost == 8,
              "balance: a load over the whole floor is centred");
        p.items[0].weight = 0;
        p.items[0].length = 4;
        check(ScoreCalculator::calculateTotals(p).loadBalanceCost == 0,
              "balance: a load that weighs nothing costs 0");
    }
    {
        // A column food, chem, chem: "chem" may not stand on "food" (one
        // pair); with chem on chem forbidden too, two.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 12);
        addItem(p, 4, 4, 4);
        addItem(p, 4, 4, 4);
        addItem(p, 4, 4, 4);
        p.items[0].stackGroup = "food";
        p.items[1].stackGroup = p.items[2].stackGroup = "chem";
        p.stackingRules = {{"chem", "food"}};
        loadAll(p);
        check(ScoreCalculator::calculateTotals(p).stackingRuleCost == 1,
              "stacking rules: one forbidden pair in the column (chem above chem is allowed)");
        p.stackingRules.push_back({"chem", "chem"});
        check(ScoreCalculator::calculateTotals(p).stackingRuleCost == 2,
              "stacking rules: a group may be forbidden on itself");
        p.stackingRules = {{"food", "chem"}};
        check(ScoreCalculator::calculateTotals(p).stackingRuleCost == 0,
              "stacking rules: only the direction named counts");
        // Layers: the column is 3 high. A limit of 2 on the bottom box and
        // on the middle one: one layer too many each.
        p.stackingRules.clear();
        p.items[0].maxStackLayers = 2;
        p.items[1].maxStackLayers = 2;
        check(ScoreCalculator::calculateTotals(p).stackingRuleCost == 2,
              "layers: a column of 3 through two boxes that allow 2");
        p.items[0].maxStackLayers = 1;
        p.items[1].maxStackLayers.reset();
        p.items[2].maxStackLayers = 3;
        check(ScoreCalculator::calculateTotals(p).stackingRuleCost == 2,
              "layers: a box that allows 1 under two more: two too many; 3 at the top holds");
    }
    {
        // Two bases side by side under one box across both: each base is in
        // a column of 2, and a limit of 1 on both counts 1 each.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 6);
        addItem(p, 4, 2, 4);
        addItem(p, 4, 2, 4);
        addItem(p, 4, 4, 2);
        p.items[0].maxStackLayers = p.items[1].maxStackLayers = 1;
        loadAll(p);
        ScoreCalculator::score(p);
        check(at(p.items[2], 0, 0, 4) && ScoreCalculator::calculateTotals(p).stackingRuleCost == 2,
              "layers: a box across two bases tops both columns");
    }
    {
        // Priorities: three cubes for room for two; the third, of priority 5,
        // costs 5. Optional, it costs 5 in optionalItemCost and nothing in
        // unplacedItemCost, and its weight leaves the list's.
        ContainerLoadingProblem p;
        addContainer(p, 8, 4, 4);
        addItem(p, 4, 4, 4, Rotation::Fixed, 1);
        addItem(p, 4, 4, 4, Rotation::Fixed, 1);
        const int third = addItem(p, 4, 4, 4, Rotation::Fixed, 9);
        p.items[static_cast<std::size_t>(third)].priority = 5;
        p.containers[0].maxWeight = 2;
        loadAll(p);
        ConstraintTotals totals = ScoreCalculator::calculateTotals(p);
        check(totals.unplacedItemCost == 5 && totals.containerWeightCost == 9,
              "priority: an unplaced item counts its priority");
        p.items[static_cast<std::size_t>(third)].optional = true;
        totals = ScoreCalculator::calculateTotals(p);
        check(totals.unplacedItemCost == 0 && totals.optionalItemCost == 5 &&
                      totals.containerWeightCost == 0,
              "optional: counted soft, its weight out of the load");
    }
    {
        // Groups: "order" of three cubes in containers with room for one
        // each. All or none: 2 placed and 1 left out cost 1; one container:
        // two hold it, so 1.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 4);
        addContainer(p, 4, 4, 4);
        for (int i = 0; i < 3; ++i) addItem(p, 4, 4, 4);
        p.items[0].group = p.items[1].group = p.items[2].group = "order";
        p.containers[0].items = {0, 2};
        p.containers[1].items = {1};
        p.groupRules = {{"order", true, true}};
        ConstraintTotals totals = ScoreCalculator::calculateTotals(p);
        check(totals.incompleteGroupCost == 1 && totals.singleContainerCost == 1,
              "groups: two of three placed costs 1; placed in two containers costs 1");
        p.containers[0].items = {0, 1, 2};
        p.containers[1].items = {};
        totals = ScoreCalculator::calculateTotals(p);
        check(totals.incompleteGroupCost == 1 && totals.singleContainerCost == 0,
              "groups: one placed and two left out costs 1; one container holds the placed one");
        p.groupRules = {{"order", false, false}};
        totals = ScoreCalculator::calculateTotals(p);
        check(totals.incompleteGroupCost == 0 && totals.singleContainerCost == 0,
              "groups: a rule that asks for nothing costs nothing");
    }
    {
        // Segregation: two hazmat cubes and a food cube in one container that
        // may not share it: two pairs. With a distance of 4 instead, the food
        // cube next to one hazmat cube and 4 from the other: one pair.
        ContainerLoadingProblem p;
        addContainer(p, 12, 4, 4);
        for (int i = 0; i < 3; ++i) addItem(p, 4, 4, 4);
        p.items[0].segregationClass = p.items[2].segregationClass = "hazmat";
        p.items[1].segregationClass = "food";
        p.segregationRules = {{"hazmat", "food", std::nullopt}};
        p.containers[0].items = {0, 1, 2};
        check(ScoreCalculator::calculateTotals(p).segregationCost == 2,
              "segregation: every pair across the two classes in one container");
        p.segregationRules = {{"food", "hazmat", 4}};
        check(ScoreCalculator::calculateTotals(p).segregationCost == 2,
              "segregation: a distance of 4, the food cube touching both hazmat cubes");
        p.containers[0].items = {0, 2, 1};
        check(ScoreCalculator::calculateTotals(p).segregationCost == 1,
              "segregation: 0 apart from one, 4 from the other: one pair");
        p.segregationRules = {{"hazmat", "hazmat", std::nullopt}};
        check(ScoreCalculator::calculateTotals(p).segregationCost == 1,
              "segregation: a class kept from itself");
        // Two rules for one pair of classes: "not in one container" holds,
        // whichever comes first; a class kept from itself counts each pair once.
        p.containers[0].items = {0, 1, 2};
        for (const auto& rules : {std::vector<SegregationRule>{{"hazmat", "food", 100},
                                                               {"food", "hazmat", std::nullopt}},
                                  std::vector<SegregationRule>{{"food", "hazmat", std::nullopt},
                                                               {"hazmat", "food", 100}}}) {
            p.segregationRules = rules;
            check(ScoreCalculator::calculateTotals(p).segregationCost == 2,
                  "segregation: the rule without a distance wins in either order");
        }
        p.segregationRules = {{"hazmat", "hazmat", 1}, {"hazmat", "hazmat", 100}};
        check(ScoreCalculator::calculateTotals(p).segregationCost == 1,
              "segregation: of two distances the larger, each pair once");
        p.segregationRules.clear();
        check(ScoreCalculator::calculateTotals(p).segregationCost == 0,
              "segregation: no rule, no cost");
    }
    {
        // The balance limit. One item of 4 x 4 in the corner of an 8 x 8
        // floor: its centre (2, 2) is half of the half size off along both
        // axes. Limits 0.2 along and 0.5 across: 30 percent over.
        ContainerLoadingProblem p;
        addContainer(p, 8, 8, 8);
        addItem(p, 4, 4, 4, Rotation::Fixed, 7);
        loadAll(p);
        check(ScoreCalculator::calculateTotals(p).balanceLimitCost == 0,
              "balance limit: none set, none counted");
        p.containers[0].maxOffsetLength = 0.2;
        p.containers[0].maxOffsetWidth = 0.5;
        check(ScoreCalculator::calculateTotals(p).balanceLimitCost == 30,
              "balance limit: 0.5 off along x against 0.2 is 30; 0.5 across at 0.5 holds");
        p.containers[0].maxOffsetWidth = 0.25;
        check(ScoreCalculator::calculateTotals(p).balanceLimitCost == 55,
              "balance limit: both axes add up (30 + 25)");
        p.containers[0].maxOffsetLength = 0.4999;
        p.containers[0].maxOffsetWidth.reset();
        check(ScoreCalculator::calculateTotals(p).balanceLimitCost == 1,
              "balance limit: a hundredth of a percent over counts 1");
        p.items[0].weight = 0;
        check(ScoreCalculator::calculateTotals(p).balanceLimitCost == 0,
              "balance limit: a load that weighs nothing costs 0");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) PackingCache.
// -------------------------------------------------------------------------
void verifyCache() {
    std::printf("== (d) PackingCache ==\n");
    std::mt19937_64 rng(5);
    ContainerLoadingProblem p = randomProblem(rng, 2, 30);
    PackingCache cache;
    cache.reset(p);
    std::vector<int> list(p.items.size());
    for (std::size_t i = 0; i < list.size(); ++i) list[i] = static_cast<int>(i);
    int mismatches = 0;
    const int rounds = 2000;
    for (int round = 0; round < rounds; ++round) {
        // A walk of small changes, as a search makes them, with repeats.
        const int kind = static_cast<int>(rng() % 4);
        if (kind == 0) {
            std::swap(list[rng() % list.size()], list[rng() % list.size()]);
        } else if (kind == 1 && list.size() > 3) {
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(rng() % list.size()));
        } else if (kind == 2) {
            std::vector<char> in(p.items.size(), 0);
            for (int item : list) in[static_cast<std::size_t>(item)] = 1;
            for (std::size_t i = 0; i < in.size(); ++i) {
                if (!in[i]) {
                    list.insert(
                            list.begin() + static_cast<std::ptrdiff_t>(rng() % (list.size() + 1)),
                            static_cast<int>(i));
                    break;
                }
            }
        }
        const int container = static_cast<int>(rng() % 2);
        if (rng() % 5 == 0) cache.pin(container, list);
        const Packing& cached = cache.packing(p, container, list);
        Packing fresh;
        decode(p, container, list, fresh);
        if (!sameBoxes(cached, fresh) || stackingExcess(p, cached) != stackingExcess(p, fresh) ||
            unloadingBlocks(p, cached) != unloadingBlocks(p, fresh) ||
            balancePerMille(p, container, cached) != balancePerMille(p, container, fresh))
            ++mismatches;
    }
    check(mismatches == 0,
          std::to_string(rounds) + " cached decodes equal a decode from scratch, costs included");
    check(cache.hits() > 100 && cache.decodes() > 100, "the walk had hits and misses");
    const std::int64_t before = cache.decodes();
    cache.packing(p, 0, list);
    cache.packing(p, 0, list);
    check(cache.decodes() <= before + 1, "a repeated list is decoded once");
    cache.reset(p);
    check(cache.decodes() >= before, "a reset with the same facts keeps the decodes");
    {
        // pin() of a list no slot holds keeps THAT list once it is decoded,
        // not whatever is decoded next: pin A, decode other lists, then A,
        // then more lists than there are slots; A is still a hit.
        PackingCache pinning;
        pinning.reset(p);
        std::vector<int> a = list;
        std::vector<std::vector<int>> others;
        for (int k = 0; k < 6; ++k) {
            others.push_back(list);
            std::rotate(others.back().begin(), others.back().begin() + k + 1, others.back().end());
        }
        pinning.pin(0, a);
        pinning.packing(p, 0, others[0]);
        pinning.packing(p, 0, a);
        for (const std::vector<int>& other : others) pinning.packing(p, 0, other);
        const std::int64_t hits = pinning.hits();
        const std::int64_t decodes = pinning.decodes();
        pinning.packing(p, 0, a);
        check(pinning.hits() == hits + 1 && pinning.decodes() == decodes,
              "a pinned list stays in its slot, also when another list was decoded first");
    }
    for (const auto& change : std::vector<std::function<void(ContainerLoadingProblem&)>>{
                 [](ContainerLoadingProblem& q) { q.containers[0].maxStackHeight = 7; },
                 [](ContainerLoadingProblem& q) { q.items[1].bottomOnly = true; },
                 [](ContainerLoadingProblem& q) { q.items[1].maxStackLayers = 2; },
                 [](ContainerLoadingProblem& q) {
                     q.items[1].stackGroup = "a";
                     q.stackingRules = {{"a", "a"}};
                 },
                 [](ContainerLoadingProblem& q) { q.containers[1].maxOffsetWidth = 0.1; }}) {
        PackingCache kept;
        kept.reset(p);
        kept.packing(p, 0, list);
        ContainerLoadingProblem changed = p;
        change(changed);
        ScoreCalculator::prepare(changed);
        kept.reset(changed);
        check(kept.decodes() == 0, "a changed stacking fact drops the decodes");
    }
    p.items[0].length += 1;
    ScoreCalculator::prepare(p);
    cache.reset(p);
    check(cache.decodes() == 0 && cache.hits() == 0, "changed facts drop every decode");
    Packing fresh;
    decode(p, 0, list, fresh);
    check(sameBoxes(cache.packing(p, 0, list), fresh), "and the next decode reads the new facts");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) ScoreCalculator.
// -------------------------------------------------------------------------
void verifyScoreCalculator() {
    std::printf("== (e) ScoreCalculator ==\n");
    const auto broken = [](const std::function<void(ContainerLoadingProblem&)>& damage) {
        ContainerLoadingProblem p = generateDemoData(6, 2);
        damage(p);
        return throwsInvalid([&p] { ScoreCalculator::prepare(p); });
    };
    check(!broken([](ContainerLoadingProblem&) {}), "the demo is a valid problem");
    check(broken([](ContainerLoadingProblem& p) { p.minSupportRatio = 0.0; }) &&
                  broken([](ContainerLoadingProblem& p) { p.minSupportRatio = 1.5; }),
          "min_support_ratio outside (0, 1] is refused");
    check(broken([](ContainerLoadingProblem& p) {
              p.items[1].id = p.items[0].id;
          }) && broken([](ContainerLoadingProblem& p) { p.containers[1].id = p.containers[0].id; }),
          "repeated ids are refused");
    check(broken([](ContainerLoadingProblem& p) {
              p.items[0].height = 0;
          }) && broken([](ContainerLoadingProblem& p) { p.containers[0].width = kMaxLength + 1; }),
          "a size outside 1..1'000'000 is refused");
    check(broken([](ContainerLoadingProblem& p) { p.items[0].weight = -1; }) &&
                  broken([](ContainerLoadingProblem& p) { p.items[0].maxLoadOnTop = -1; }) &&
                  broken([](ContainerLoadingProblem& p) { p.containers[0].maxWeight = -1; }) &&
                  broken([](ContainerLoadingProblem& p) { p.containers[0].cost = -1.0; }),
          "a negative weight, limit or cost is refused");
    check(broken([](ContainerLoadingProblem& p) {
              p.containers[0].items = {0, 0};
          }) && broken([](ContainerLoadingProblem& p) {
              p.containers[0].items = {1};
              p.containers[1].items = {1};
          }) && broken([](ContainerLoadingProblem& p) { p.containers[0].items = {99}; }),
          "an item listed twice or out of range is refused");
    check(broken([](ContainerLoadingProblem& p) { p.containers.clear(); }),
          "items without a container are refused");
    check(broken([](ContainerLoadingProblem& p) {
              p.items[0].maxStackLayers = 0;
          }) && broken([](ContainerLoadingProblem& p) {
              p.containers[0].maxStackHeight = 0;
          }) && broken([](ContainerLoadingProblem& p) {
              p.containers[0].maxStackHeight = kMaxLength + 1;
          }) && broken([](ContainerLoadingProblem& p) {
              p.containers[0].maxOffsetLength = 1.5;
          }) && broken([](ContainerLoadingProblem& p) {
              p.containers[0].maxOffsetWidth = -0.1;
          }) && broken([](ContainerLoadingProblem& p) {
              p.containers[0].maxOffsetWidth = std::nan("");
          }) && broken([](ContainerLoadingProblem& p) { p.stackingRules = {{"", "food"}}; }),
          "a layer limit below 1, a stack height outside 1..1'000'000, an offset limit outside "
          "0..1 and a rule without a group are refused");
    check(broken([](ContainerLoadingProblem& p) {
              p.items[0].priority = 0;
          }) && broken([](ContainerLoadingProblem& p) {
              p.items[0].priority = kMaxPriority + 1;
          }) && broken([](ContainerLoadingProblem& p) {
              p.groupRules = {{"nobody", true, false}};
          }) && broken([](ContainerLoadingProblem& p) {
              p.groupRules = {{*p.items[0].group, true, false}, {*p.items[0].group, false, true}};
          }),
          "a priority outside 1..1'000'000, a group rule for no item's group and a group ruled "
          "twice are refused");
    check(broken([](ContainerLoadingProblem& p) {
              p.items[0].allowedContainers = {"nowhere"};
          }) && broken([](ContainerLoadingProblem& p) {
              p.segregationRules = {{"a", "", std::nullopt}};
          }) && broken([](ContainerLoadingProblem& p) { p.segregationRules = {{"a", "b", 0}}; }),
          "allowed containers naming no container's id or tag, a segregation rule without a class "
          "and a distance below 1 are refused");
    check(broken([](ContainerLoadingProblem& p) { p.containers[0].tags = {p.containers[1].id}; }),
          "a tag that is another container's id is refused");

    // Two pallets, hand-built: weight above the limit, a group over both, a
    // container cost.
    ContainerLoadingProblem p;
    addContainer(p, 10, 10, 10);
    addContainer(p, 10, 10, 10);
    p.containers[0].maxWeight = 10;
    p.containers[1].cost = 2.5;
    const int a = addItem(p, 5, 5, 5, Rotation::Fixed, 8);
    const int b = addItem(p, 5, 5, 5, Rotation::Fixed, 7);
    const int c = addItem(p, 5, 5, 5, Rotation::Fixed, 1);
    addItem(p, 5, 5, 5, Rotation::Fixed, 1);  // in no list
    p.items[static_cast<std::size_t>(a)].group = "order";
    p.items[static_cast<std::size_t>(c)].group = "order";
    p.containers[0].items = {a, b};
    p.containers[1].items = {c};
    const ConstraintTotals totals = ScoreCalculator::calculateTotals(p);
    check(totals.containerWeightCost == 5 && totals.unplacedItemCost == 0 &&
                  totals.usedContainerCost == 3.5 && totals.groupSplitCost == 1,
          "totals: 5 over the weight limit, containers 1 + 2.5, one group in two containers");
    const Score score = ScoreCalculator::score(p);
    check(score.hard == -5 && std::fabs(score.soft + 4.5) < 1e-12, "score: -5hard/-4.5soft");
    check(!p.items[3].isPlaced() && p.containersUsed() == 2 && p.containers[0].loadedWeight == 15,
          "an item in no list is not placed and costs nothing; derived fields are written");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) The construction.
// -------------------------------------------------------------------------
void verifyConstruction() {
    std::printf("== (f) Construction ==\n");
    {
        ContainerLoadingProblem p = generateDemoData();
        ScoreCalculator::prepare(p);
        constructLoading(p);
        std::vector<int> count(p.items.size(), 0);
        for (const Container& container : p.containers) {
            for (int item : container.items) ++count[static_cast<std::size_t>(item)];
        }
        check(std::all_of(count.begin(), count.end(), [](int c) { return c == 1; }),
              "every item is in exactly one list");
        const ConstraintTotals totals = ScoreCalculator::calculateTotals(p);
        check(totals.unplacedItemCost == 0 && totals.containerWeightCost == 0,
              "the demo's construction places every item within the weight limits");
        ContainerLoadingProblem again = generateDemoData();
        ScoreCalculator::prepare(again);
        constructLoading(again);
        bool same = true;
        for (std::size_t c = 0; c < p.containers.size(); ++c)
            same = same && p.containers[c].items == again.containers[c].items;
        check(same, "the construction is deterministic");
    }
    {
        // The largest item goes first, and a list the problem comes with is kept.
        ContainerLoadingProblem p;
        addContainer(p, 10, 10, 10);
        addContainer(p, 10, 10, 10);
        const int small = addItem(p, 2, 2, 2);
        const int large = addItem(p, 6, 6, 6);
        const int medium = addItem(p, 4, 4, 4);
        const int seeded = addItem(p, 3, 3, 3);
        p.containers[1].items = {seeded};
        ScoreCalculator::prepare(p);
        constructLoading(p);
        check(p.containers[1].items == std::vector<int>({seeded, large, medium, small}),
              "a seeded list is kept and filled first, largest item first");
        check(p.containers[0].items.empty(),
              "no second container is opened while the first takes it");
    }
    {
        // Later stops are loaded first.
        ContainerLoadingProblem p;
        addContainer(p, 12, 4, 4);
        for (int stop : {1, 3, 2}) {
            const int item = addItem(p, 4, 4, 4);
            p.items[static_cast<std::size_t>(item)].unloadOrder = stop;
        }
        ScoreCalculator::prepare(p);
        constructLoading(p);
        check(p.containers[0].items == std::vector<int>({1, 2, 0}),
              "with unloading orders, what leaves last is loaded first");
        check(ScoreCalculator::calculateTotals(p).unloadingOrderCost == 0,
              "and nothing is in the way of an earlier stop");
        std::vector<int> list = {1, 0};
        check(insertPosition(p, list, 2) == 1 && insertPosition(p, list, 0) == 2,
              "insertPosition keeps a list sorted by unloading order");
    }
    {
        // Items of a group go where the group already is.
        ContainerLoadingProblem p;
        addContainer(p, 10, 10, 10);
        addContainer(p, 10, 10, 10);
        const int first = addItem(p, 5, 5, 5);
        const int second = addItem(p, 5, 5, 5);
        p.items[static_cast<std::size_t>(first)].group = "order";
        p.items[static_cast<std::size_t>(second)].group = "order";
        const int other = addItem(p, 6, 6, 6);
        p.containers[1].items = {first};
        ScoreCalculator::prepare(p);
        constructLoading(p);
        check(std::find(p.containers[1].items.begin(), p.containers[1].items.end(), second) !=
                      p.containers[1].items.end(),
              "an item joins the container that holds its group");
        (void)other;
    }
    {
        // An item no container takes still gets a list, and counts as unplaced.
        ContainerLoadingProblem p;
        addContainer(p, 4, 4, 4);
        addItem(p, 5, 5, 5);
        addItem(p, 4, 4, 4);
        ScoreCalculator::prepare(p);
        constructLoading(p);
        check(p.containers[0].items.size() == 2 &&
                      ScoreCalculator::calculateTotals(p).unplacedItemCost == 1,
              "an item that fits nowhere is listed and counted as unplaced");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) The moves.
// -------------------------------------------------------------------------
struct MoveHarness {
    explicit MoveHarness(ContainerLoadingProblem& problem)
        : editor(problem, calculator, optimaru::ScoreChecks::Full) {}

    // Apply `move` once; returns whether it moved.
    bool apply(optimaru::MoveOperator<ContainerLoadingProblem>& move, std::mt19937_64& rng) {
        ContainerLoadingProblem& p = editor.solution();
        elementList.assign(p.items.size(), -1);
        elementPosition.assign(p.items.size(), -1);
        for (std::size_t c = 0; c < p.containers.size(); ++c) {
            for (std::size_t position = 0; position < p.containers[c].items.size(); ++position) {
                const auto item = static_cast<std::size_t>(p.containers[c].items[position]);
                elementList[item] = static_cast<int>(c);
                elementPosition[item] = static_cast<int>(position);
            }
        }
        optimaru::OperatorContext<ContainerLoadingProblem> ctx{editor, nearby, elementList,
                                                               elementPosition};
        entities.clear();
        return move.apply(ctx, rng, entities);
    }
    void undo(optimaru::MoveOperator<ContainerLoadingProblem>& move) {
        optimaru::OperatorContext<ContainerLoadingProblem> ctx{editor, nearby, elementList,
                                                               elementPosition};
        move.undo_last(ctx);
    }

    DecomposedCalculator calculator = [] {
        DecomposedCalculator built;
        addDefaultConstraints(built);
        return built;
    }();
    optimaru::Editor<ContainerLoadingProblem> editor;
    optimaru::Neighborhoods nearby;
    std::vector<int> elementList;
    std::vector<int> elementPosition;
    std::vector<int> entities;
};

std::vector<std::vector<int>> listsOf(const ContainerLoadingProblem& p) {
    std::vector<std::vector<int>> lists;
    for (const Container& container : p.containers) lists.push_back(container.items);
    return lists;
}

void verifyMoves() {
    std::printf("== (g) The moves ==\n");
    {
        // One container with three stops, loaded in a mixed order, and one
        // item without an order. The move puts the stops back in blocks,
        // latest first, leaves the unordered item where it is, changes
        // something inside one block, and undoes exactly.
        ContainerLoadingProblem p;
        addContainer(p, 20, 4, 4);
        std::vector<int> mixed;
        for (int stop : {1, 3, 2, 1, 3, 2, 2}) {
            const int item = addItem(p, 2, 2, 2 + static_cast<int>(p.items.size()) % 2);
            p.items[static_cast<std::size_t>(item)].unloadOrder = stop;
            mixed.push_back(item);
        }
        const int free = addItem(p, 1, 1, 1);
        mixed.insert(mixed.begin() + 2, free);
        p.containers[0].items = mixed;
        ScoreCalculator::prepare(p);
        check(StopOrderMove::applies(p), "StopOrderMove applies when an item has an order");
        MoveHarness harness(p);
        const Score before = harness.editor.score();
        const auto lists = listsOf(p);
        StopOrderMove move;
        std::mt19937_64 rng(5);
        bool blocks = true;
        bool kept = true;
        bool undone = true;
        for (int round = 0; round < 30; ++round) {
            if (!harness.apply(move, rng)) {
                blocks = false;
                break;
            }
            const std::vector<int>& list = p.containers[0].items;
            kept = kept && list[2] == free;
            int previous = 1 << 30;
            for (int item : list) {
                const auto& order = p.items[static_cast<std::size_t>(item)].unloadOrder;
                if (!order) continue;
                blocks = blocks && *order <= previous;
                previous = *order;
            }
            harness.undo(move);
            undone = undone && listsOf(p) == lists && harness.editor.score() == before;
        }
        check(blocks, "StopOrderMove puts the ordered items in stop blocks, the latest first");
        check(kept, "and leaves an item without an order where it was");
        check(undone, "undo_last puts the list and the score back");
        // Already in blocks: it changes the order inside one block.
        std::vector<int> sorted = mixed;
        std::stable_sort(sorted.begin(), sorted.end(), [&p](int a, int b) {
            return p.items[static_cast<std::size_t>(a)].unloadOrder.value_or(0) >
                   p.items[static_cast<std::size_t>(b)].unloadOrder.value_or(0);
        });
        harness.editor.solution().containers[0].items = sorted;
        MoveHarness again(p);
        bool changed = true;
        for (int round = 0; round < 30; ++round) {
            again.apply(move, rng);
            changed = changed && p.containers[0].items != sorted;
            again.undo(move);
        }
        check(changed, "on a list in blocks it always changes something");
    }
    {
        // Three containers, each holding a few small boxes that fit in one.
        ContainerLoadingProblem p;
        for (int c = 0; c < 3; ++c) addContainer(p, 10, 10, 10);
        for (int i = 0; i < 9; ++i)
            p.containers[static_cast<std::size_t>(i % 3)].items.push_back(addItem(p, 3, 3, 3));
        ScoreCalculator::prepare(p);
        MoveHarness harness(p);
        const Score before = harness.editor.score();
        const auto lists = listsOf(p);
        EmptyContainerMove move(p, 3);
        std::mt19937_64 rng(1);
        const bool moved = harness.apply(move, rng);
        const Score after = harness.editor.score();
        int used = 0;
        for (const Container& container : p.containers) used += container.items.empty() ? 0 : 1;
        check(moved && used == 2 && before.soft == -3.0 && after.soft == -2.0 && after.hard == 0 &&
                      harness.entities.size() == 3,
              "EmptyContainerMove empties one of three containers into the others: -3 to -2");
        harness.undo(move);
        check(listsOf(p) == lists && harness.editor.score() == before,
              "undo_last puts the lists and the score back");

        // Emptied down to one container, then nothing more to empty: it still moves.
        for (int step = 0; step < 20; ++step) harness.apply(move, rng);
        used = 0;
        for (const Container& container : p.containers) used += container.items.empty() ? 0 : 1;
        check(used == 1 && harness.editor.score().soft == -1.0,
              "repeated, it packs everything into one container");
        check(!harness.apply(move, rng), "with one container in use there is no move");
    }
    {
        // Nothing fits anywhere else: the move relocates one item instead, and undoes it.
        ContainerLoadingProblem p;
        for (int c = 0; c < 2; ++c) addContainer(p, 4, 4, 4);
        for (int c = 0; c < 2; ++c)
            p.containers[static_cast<std::size_t>(c)].items = {addItem(p, 4, 4, 4)};
        ScoreCalculator::prepare(p);
        MoveHarness harness(p);
        const Score before = harness.editor.score();
        const auto lists = listsOf(p);
        EmptyContainerMove move(p, 3);
        std::mt19937_64 rng(3);
        check(harness.apply(move, rng) && listsOf(p) != lists && harness.editor.score().hard == -1,
              "when no item can move without losing its place, one item is relocated anyway");
        harness.undo(move);
        check(listsOf(p) == lists && harness.editor.score() == before,
              "and that is undone exactly");
    }
    {
        // Two groups of four, each half in one container and half in the other.
        ContainerLoadingProblem p;
        for (int c = 0; c < 2; ++c) addContainer(p, 10, 10, 10);
        for (int i = 0; i < 8; ++i) {
            const int item = addItem(p, 5, 5, 5);
            p.items[static_cast<std::size_t>(item)].group = i < 4 ? "red" : "blue";
            p.containers[static_cast<std::size_t>(i % 2)].items.push_back(item);
        }
        ScoreCalculator::prepare(p);
        MoveHarness harness(p);
        const Score before = harness.editor.score();
        const auto lists = listsOf(p);
        RepackPairMove move(p);
        std::mt19937_64 rng(6);
        const bool moved = harness.apply(move, rng);
        // Eight cubes of 5 fill one space of 10. A group goes whole to the
        // first container with room for all of it, so either both groups
        // share the first (-1) or each has its own (-2); neither is split.
        const Score after = harness.editor.score();
        check(moved && before == Score{0, -4.0} && after.hard == 0 && after.soft >= -2.0 &&
                      ScoreCalculator::calculateTotals(p).groupSplitCost == 0,
              "RepackPairMove gathers two groups that two containers shared: -4 to -2 or better");
        harness.undo(move);
        check(listsOf(p) == lists && harness.editor.score() == before,
              "undo_last puts the lists and the score back");
    }
    {
        // A group for the reefer only, half in the dry container (unplaced
        // there): repacked, it goes whole to the reefer.
        ContainerLoadingProblem p;
        for (int c = 0; c < 2; ++c) addContainer(p, 10, 10, 10);
        p.containers[1].tags = {"reefer"};
        for (int i = 0; i < 4; ++i) {
            const int item = addItem(p, 5, 5, 5);
            p.items[static_cast<std::size_t>(item)].group = "chilled";
            p.items[static_cast<std::size_t>(item)].allowedContainers = {"reefer"};
            p.containers[static_cast<std::size_t>(i % 2)].items.push_back(item);
        }
        ScoreCalculator::prepare(p);
        MoveHarness harness(p);
        const Score before = harness.editor.score();
        RepackPairMove move(p);
        std::mt19937_64 rng(6);
        check(harness.apply(move, rng) && before.hard == -2 && harness.editor.score().hard == 0 &&
                      p.containers[0].items.empty(),
              "RepackPairMove deals a group only to a container it may go in");
    }
    {
        // What does not fit the two containers again is left as it is: two
        // spaces each full with one cube, loaded with a second that found no
        // place. The move changes one item's place at most (the fallback) and
        // undoes it.
        ContainerLoadingProblem p;
        for (int c = 0; c < 2; ++c) addContainer(p, 4, 4, 4);
        for (int i = 0; i < 4; ++i)
            p.containers[static_cast<std::size_t>(i % 2)].items.push_back(addItem(p, 4, 4, 4));
        ScoreCalculator::prepare(p);
        MoveHarness harness(p);
        const Score before = harness.editor.score();
        const auto lists = listsOf(p);
        RepackPairMove move(p);
        std::mt19937_64 rng(8);
        const bool moved = harness.apply(move, rng);
        check(moved && harness.editor.score() == before && before.hard == -2,
              "a pair that cannot be loaded clean is not repacked (one item is relocated)");
        harness.undo(move);
        check(listsOf(p) == lists && harness.editor.score() == before,
              "and that is undone exactly");
    }
    {
        // loadsBefore: later stops first, a limited load last, else largest first.
        ContainerLoadingProblem p;
        addContainer(p, 10, 10, 10);
        const int small = addItem(p, 2, 2, 2);
        const int large = addItem(p, 4, 4, 4);
        const int fragile = addItem(p, 5, 5, 5);
        const int early = addItem(p, 6, 6, 6);
        p.items[static_cast<std::size_t>(fragile)].maxLoadOnTop = 3;
        p.items[static_cast<std::size_t>(early)].unloadOrder = 1;
        ScoreCalculator::prepare(p);
        check(loadsBefore(p, large, small) && !loadsBefore(p, small, large) &&
                      loadsBefore(p, small, fragile) && loadsBefore(p, small, early) &&
                      loadsBefore(p, fragile, early) && !loadsBefore(p, large, large),
              "loadsBefore: largest first, a limited load after the others, an earlier stop last");
        constructLoading(p);
        check(p.containers[0].items == std::vector<int>({large, small, fragile, early}),
              "the construction loads in that order");
    }
    {
        // A small load in a large, dear carton; a small, cheap carton stands empty.
        ContainerLoadingProblem p;
        addContainer(p, 10, 10, 10);
        addContainer(p, 4, 4, 4);
        p.containers[0].cost = 5.0;
        p.containers[1].cost = 2.0;
        p.containers[0].items = {addItem(p, 2, 2, 2), addItem(p, 2, 2, 2)};
        ScoreCalculator::prepare(p);
        check(SwitchContainerMove::applies(p),
              "SwitchContainerMove applies when the containers differ");
        MoveHarness harness(p);
        const Score before = harness.editor.score();
        const auto lists = listsOf(p);
        SwitchContainerMove move(p);
        std::mt19937_64 rng(2);
        check(harness.apply(move, rng) && p.containers[0].items.empty() &&
                      p.containers[1].items == lists[0] && before.soft == -5.0 &&
                      harness.editor.score() == Score{0, -2.0},
              "it moves the whole load into the cheaper carton: -5 to -2");
        harness.undo(move);
        check(listsOf(p) == lists && harness.editor.score() == before,
              "undo_last puts the lists and the score back");
        p.containers[1].cost = 5.0;
        p.containers[1].length = p.containers[1].width = p.containers[1].height = 10;
        check(!SwitchContainerMove::applies(p), "and does not apply when all containers are alike");
    }
    {
        // Two boxes in a large carton (cost 3.5) that weigh more than a small
        // one carries: two small cartons (cost 1 each) are cheaper.
        ContainerLoadingProblem p;
        addContainer(p, 4, 2, 2);
        addContainer(p, 4, 2, 2);
        addContainer(p, 6, 4, 4);
        p.containers[0].maxWeight = p.containers[1].maxWeight = 5;
        p.containers[2].cost = 3.5;
        p.containers[2].items = {addItem(p, 2, 2, 2, Rotation::Fixed, 4),
                                 addItem(p, 2, 2, 2, Rotation::Fixed, 4)};
        ScoreCalculator::prepare(p);
        MoveHarness harness(p);
        const auto lists = listsOf(p);
        SwitchContainerMove move(p);
        std::mt19937_64 rng(4);
        check(harness.apply(move, rng) && p.containers[2].items.empty() &&
                      p.containers[0].items.size() == 1 && p.containers[1].items.size() == 1 &&
                      harness.editor.score() == Score{0, -2.0},
              "a load too heavy for one small carton is split over two: -3.5 to -2");
        harness.undo(move);
        check(listsOf(p) == lists, "and the split is undone exactly");
    }
    {
        // The same load, but the small cartons cost 2 each: the split would
        // open 4 for a carton of 3.5 and stops there, and the whole load does
        // not fit one small carton's weight limit. Nothing is in use besides
        // the large carton, so the fallback has nowhere to go either.
        for (const double smallCost : {2.0, 1.75}) {
            ContainerLoadingProblem p;
            addContainer(p, 4, 2, 2);
            addContainer(p, 4, 2, 2);
            addContainer(p, 6, 4, 4);
            p.containers[0].maxWeight = p.containers[1].maxWeight = 5;
            p.containers[0].cost = p.containers[1].cost = smallCost;
            p.containers[2].cost = 3.5;
            p.containers[2].items = {addItem(p, 2, 2, 2, Rotation::Fixed, 4),
                                     addItem(p, 2, 2, 2, Rotation::Fixed, 4)};
            ScoreCalculator::prepare(p);
            MoveHarness harness(p);
            const auto lists = listsOf(p);
            SwitchContainerMove move(p);
            bool anyApplied = false;
            bool everSplit = false;
            for (std::uint64_t seed = 0; seed < 16; ++seed) {
                std::mt19937_64 rng(seed);
                if (!harness.apply(move, rng)) continue;
                anyApplied = true;
                everSplit = everSplit || (p.containers[2].items.empty() &&
                                          harness.editor.score() == Score{0, -2 * smallCost});
                harness.undo(move);
            }
            if (smallCost > 1.75) {
                check(!anyApplied && listsOf(p) == lists,
                      "a split dearer than the carton it leaves is not made (2 + 2 > 3.5)");
            } else {
                check(everSplit && listsOf(p) == lists,
                      "and one that costs exactly as much is (1.75 + 1.75 = 3.5)");
            }
        }
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The solver.
// -------------------------------------------------------------------------
void verifySolver() {
    std::printf("== (h) The solver ==\n");
    {
        ContainerLoadingProblem p = generateDemoData();
        Solver::Config config;
        config.time_limit = std::chrono::seconds(60);
        config.step_limit = 3000;
        config.seed = 3;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 300;
        config.ruin_recreate_polish_steps = 100;
        bool corrupted = false;
        try {
            Solver(config).solve(p);
        } catch (const optimaru::ScoreMismatchError& error) {
            corrupted = true;
            std::printf("  ScoreMismatchError: %s\n", error.what());
        }
        check(!corrupted,
              "3000 steps of the demo under ScoreChecks::Full without a score mismatch");
        check(!corrupted && p.score.feasible() && p.containersUsed() <= 3,
              "the demo is feasible on at most three pallets");
        std::size_t listed = 0;
        for (const Container& container : p.containers) listed += container.items.size();
        check(listed == p.items.size(), "every item is still in a list");
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.rebuild(p);
        check(calculator.score() == p.score, "the reported score == the default assembly's");
    }
    {
        ContainerLoadingProblem p = generateDemoData();
        ContainerLoadingProblem constructed = generateDemoData();
        ScoreCalculator::prepare(constructed);
        constructLoading(constructed);
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(0);
        Solver(config).solve(p);
        check(listsOf(p) == listsOf(constructed), "a zero budget returns the construction");
        const auto lists = listsOf(p);
        Solver(config).solve(p);
        check(listsOf(p) == lists, "and a problem that comes with its lists keeps them");
    }
    {
        const auto solved = [](std::uint64_t seed, int starts) {
            ContainerLoadingProblem p = generateDemoData();
            Solver::Config config;
            config.time_limit = std::chrono::seconds(60);
            config.step_limit = 1500;
            config.seed = seed;
            config.parallel_solver_count = starts;
            Solver(config).solve(p);
            return p.score;
        };
        const Score first = solved(4, 1);
        const Score best = std::max({first, solved(5, 1), solved(6, 1)});
        check(solved(4, 1) == first, "a step-limited start is reproducible");
        check(solved(4, 3) == best,
              "three parallel starts return the best of the three single starts");
    }
    {
        ContainerLoadingProblem empty;
        Solver().solve(empty);
        check(empty.score == Score{0, 0.0}, "an empty problem solves to 0hard/0soft");
        ContainerLoadingProblem bad = generateDemoData(4, 1);
        bad.items[0].length = 0;
        check(throwsInvalid([&bad] { Solver().solve(bad); }),
              "a bad problem is refused before the search");
    }
    std::printf("\n");
}

}  // namespace

int main() {
    verifyDecoderByHand();
    verifyDecoderProperties();
    verifyCostsByHand();
    verifyCache();
    verifyScoreCalculator();
    verifyConstruction();
    verifyMoves();
    verifySolver();

    if (failures == 0) {
        std::printf("All container-loading checks PASSED.\n");
        return 0;
    }
    std::printf("%d container-loading check(s) FAILED.\n", failures);
    return 1;
}
