// Container-loading tests, engine test/ style: check() + a non-zero exit on any failure.
//   (a) the decoder by hand: the (x, z, y) order of the points, the support
//       rule, the rotations, which orientation is taken, an item that finds
//       no place
//   (b) the decoder on random lists: every decode is a valid loading (inside
//       the container, no overlap, enough support), and a decode resumed from
//       any shared prefix equals a decode from scratch
//   (c) the costs of one decoded container by hand: stacking load (passed
//       down, shared by contact area), unloading order (in front, on top),
//       used length, load balance
//   (d) PackingCache: what it returns is decode()'s result, a repeated list
//       is a hit, changed facts drop the decodes
//   (e) ScoreCalculator: prepare's refusals, the derived fields, the totals
//   (f) the construction: every item listed once, largest first, a seeded
//       list kept, later stops loaded first, deterministic
//   (g) the moves: EmptyContainerMove empties a container, RepackPairMove
//       gathers the groups two containers share, SwitchContainerMove moves a
//       load to a cheaper container or splits it over two (never over ones
//       that cost more together), all undo exactly
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

int overlap(int a, int la, int b, int lb) {
    return std::max(0, std::min(a + la, b + lb) - std::max(a, b));
}

// A loading checked without the decoder: inside the walls, no two items
// overlapping, every base supported by at least the problem's ratio.
bool validLoading(const ContainerLoadingProblem& p, int container, const Packing& packing) {
    const Container& space = p.containers[static_cast<std::size_t>(container)];
    for (std::size_t i = 0; i < packing.boxes.size(); ++i) {
        const Box& box = packing.boxes[i];
        if (box.x < 0 || box.y < 0 || box.z < 0 || box.x + box.length > space.length ||
            box.y + box.width > space.width || box.z + box.height > space.height)
            return false;
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
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) The decoder on random lists.
// -------------------------------------------------------------------------
void verifyDecoderProperties() {
    std::printf("== (b) Random decodes: valid loadings, resumable from any prefix ==\n");
    std::mt19937_64 rng(11);
    int invalid = 0;
    int resumeMismatches = 0;
    int placed = 0;
    int unplaced = 0;
    const int rounds = 300;
    for (int round = 0; round < rounds; ++round) {
        ContainerLoadingProblem p = randomProblem(rng, 1, 5 + static_cast<int>(rng() % 30));
        p.minSupportRatio = round % 3 == 0 ? 1.0 : round % 3 == 1 ? 0.8 : 0.5;
        std::vector<int> list(p.items.size());
        for (std::size_t i = 0; i < list.size(); ++i) list[i] = static_cast<int>(i);
        std::shuffle(list.begin(), list.end(), rng);
        Packing base;
        decode(p, 0, list, base);
        if (!validLoading(p, 0, base)) ++invalid;
        placed += static_cast<int>(base.boxes.size());
        unplaced += base.unplaced();

        // Another list that shares a prefix with it: decoded from scratch, and
        // resumed from every length up to the shared one.
        std::vector<int> other = list;
        const std::size_t cut = rng() % (list.size() + 1);
        std::shuffle(other.begin() + static_cast<std::ptrdiff_t>(cut), other.end(), rng);
        if (rng() % 2 == 0 && other.size() > cut) other.pop_back();
        std::size_t common = 0;
        while (common < other.size() && common < list.size() && other[common] == list[common])
            ++common;
        Packing fresh;
        decode(p, 0, other, fresh);
        for (std::size_t keep :
             {common, common / 2, static_cast<std::size_t>(rng() % (common + 1)), std::size_t{0}}) {
            Packing resumed = base;
            decode(p, 0, other, resumed, static_cast<int>(keep));
            if (!sameBoxes(resumed, fresh) || freePoints(resumed) != freePoints(fresh))
                ++resumeMismatches;
        }
    }
    std::printf("    %d items placed, %d unplaced\n", placed, unplaced);
    check(invalid == 0,
          std::to_string(rounds) +
                  " random lists decode to valid loadings (checked without the decoder)");
    check(placed > 1000 && unplaced > 100, "the lists exercise placed and unplaced items");
    check(resumeMismatches == 0,
          "a decode resumed from a shared prefix equals the decode from scratch");
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
