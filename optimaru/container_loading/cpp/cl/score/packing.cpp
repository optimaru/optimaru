#include "cl/score/packing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace cl {

namespace {

// (x, z, y): from the closed end towards the door, then up, then across.
bool before(const Point& a, const Point& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.z != b.z) return a.z < b.z;
    return a.y < b.y;
}

// The length two ranges [a, a + la) and [b, b + lb) share.
int shared(int a, int la, int b, int lb) {
    const int low = std::max(a, b);
    const int high = std::min(a + la, b + lb);
    return high > low ? high - low : 0;
}

// Forget everything the decode did from list position `keep` on.
void truncate(Packing& packing, int keep) {
    while (!packing.boxes.empty() && packing.boxes.back().position >= keep)
        packing.boxes.pop_back();
    auto& points = packing.points;
    points.erase(std::remove_if(points.begin(), points.end(),
                                [keep](const Point& point) { return point.born >= keep; }),
                 points.end());
    for (Point& point : points) {
        if (point.died != Point::kOpen && point.died >= keep) point.died = Point::kOpen;
    }
}

bool feasible(const ContainerLoadingProblem& problem, const Container& container,
              const std::vector<Box>& boxes, const Point& point, const Orientation& o) {
    if (point.x + o.length > container.length || point.y + o.width > container.width ||
        point.z + o.height > container.height)
        return false;
    for (const Box& box : boxes) {
        if (point.x < box.x + box.length && box.x < point.x + o.length &&
            point.y < box.y + box.width && box.y < point.y + o.width &&
            point.z < box.z + box.height && box.z < point.z + o.height)
            return false;
    }
    if (point.z == 0) return true;
    // The base rests on the tops that end exactly where it starts. Placed
    // items do not overlap, so the areas add up without counting any twice.
    const double base = static_cast<double>(o.length) * static_cast<double>(o.width);
    const double needed = problem.minSupportRatio * base;
    // The tolerance absorbs the rounding of ratio * base; capped at half an
    // area unit, since `supported` is a sum of whole areas, so the decoder
    // never accepts a base a whole unit short (the evaluators check at 1e-6).
    const double enough = needed - std::min(0.5, 1e-9 * std::max(1.0, needed));
    double supported = 0.0;
    for (const Box& box : boxes) {
        if (box.z + box.height != point.z) continue;
        const int dx = shared(point.x, o.length, box.x, box.length);
        if (dx == 0) continue;
        const int dy = shared(point.y, o.width, box.y, box.width);
        if (dy == 0) continue;
        supported += static_cast<double>(dx) * static_cast<double>(dy);
        if (supported >= enough) return true;
    }
    return false;
}

// Slide the point (x, y, z) back along one axis until it meets the far face
// of a placed item or the wall at 0; returns its coordinate on that axis.
int slide(const std::vector<Box>& boxes, int x, int y, int z, int axis) {
    int best = 0;
    for (const Box& box : boxes) {
        const bool inX = box.x <= x && x < box.x + box.length;
        const bool inY = box.y <= y && y < box.y + box.width;
        const bool inZ = box.z <= z && z < box.z + box.height;
        if (axis == 0) {
            if (inY && inZ && box.x + box.length <= x) best = std::max(best, box.x + box.length);
        } else if (axis == 1) {
            if (inX && inZ && box.y + box.width <= y) best = std::max(best, box.y + box.width);
        } else {
            if (inX && inY && box.z + box.height <= z) best = std::max(best, box.z + box.height);
        }
    }
    return best;
}

// How far the room reaches from a point along x, y and z: up to the next
// placed item the ray from the point meets, or the wall.
void freeRoom(const Container& container, const std::vector<Box>& boxes, const Point& point,
              int room[3]) {
    int endX = container.length;
    int endY = container.width;
    int endZ = container.height;
    for (const Box& box : boxes) {
        const bool inX = box.x <= point.x && point.x < box.x + box.length;
        const bool inY = box.y <= point.y && point.y < box.y + box.width;
        const bool inZ = box.z <= point.z && point.z < box.z + box.height;
        if (inY && inZ && box.x >= point.x) endX = std::min(endX, box.x);
        if (inX && inZ && box.y >= point.y) endY = std::min(endY, box.y);
        if (inX && inY && box.z >= point.z) endZ = std::min(endZ, box.z);
    }
    room[0] = endX - point.x;
    room[1] = endY - point.y;
    room[2] = endZ - point.z;
}

void addPoint(Packing& packing, const Container& container, int x, int y, int z, int position) {
    if (x >= container.length || y >= container.width || z >= container.height) return;
    for (const Box& box : packing.boxes) {
        if (box.x <= x && x < box.x + box.length && box.y <= y && y < box.y + box.width &&
            box.z <= z && z < box.z + box.height)
            return;  // inside a placed item
    }
    Point point;
    point.x = x;
    point.y = y;
    point.z = z;
    point.born = position;
    auto& points = packing.points;
    auto at = std::lower_bound(points.begin(), points.end(), point, before);
    for (auto same = at; same != points.end() && !before(point, *same); ++same) {
        if (same->died == Point::kOpen) return;  // already a free point
    }
    points.insert(at, point);
}

void place(Packing& packing, const Container& container, const Point& at, const Orientation& o,
           int item, int position) {
    Box box;
    box.item = item;
    box.position = position;
    box.x = at.x;
    box.y = at.y;
    box.z = at.z;
    box.length = o.length;
    box.width = o.width;
    box.height = o.height;
    for (Point& point : packing.points) {
        if (point.died == Point::kOpen && box.x <= point.x && point.x < box.x + box.length &&
            box.y <= point.y && point.y < box.y + box.width && box.z <= point.z &&
            point.z < box.z + box.height)
            point.died = position;
    }
    packing.boxes.push_back(box);
    const std::vector<Box>& boxes = packing.boxes;
    const int farX = box.x + box.length;
    const int farY = box.y + box.width;
    const int farZ = box.z + box.height;
    addPoint(packing, container, farX, slide(boxes, farX, box.y, box.z, 1), box.z, position);
    addPoint(packing, container, farX, box.y, slide(boxes, farX, box.y, box.z, 2), position);
    addPoint(packing, container, slide(boxes, box.x, farY, box.z, 0), farY, box.z, position);
    addPoint(packing, container, box.x, farY, slide(boxes, box.x, farY, box.z, 2), position);
    addPoint(packing, container, slide(boxes, box.x, box.y, farZ, 0), box.y, farZ, position);
    addPoint(packing, container, box.x, slide(boxes, box.x, box.y, farZ, 1), farZ, position);
}

}  // namespace

void decode(const ContainerLoadingProblem& problem, int container, const std::vector<int>& members,
            Packing& out, int keep) {
    const Container& space = problem.containers[container];
    const int count = static_cast<int>(members.size());
    if (keep <= 0) {
        keep = 0;
        out.boxes.clear();
        out.points.clear();
        out.points.push_back(Point());
    } else {
        truncate(out, keep);
    }
    out.members.assign(members.begin(), members.end());
    out.boxOf.resize(static_cast<std::size_t>(count));

    for (int position = keep; position < count; ++position) {
        const int item = members[static_cast<std::size_t>(position)];
        const int smallest = problem.smallestSide[static_cast<std::size_t>(item)];
        const int first = problem.orientationStart[static_cast<std::size_t>(item)];
        const int last = problem.orientationStart[static_cast<std::size_t>(item) + 1];
        int chosenPoint = -1;
        int chosenOrientation = -1;
        int chosenFit = -1;
        const int pointCount = static_cast<int>(out.points.size());
        for (int k = 0; k < pointCount && chosenPoint < 0; ++k) {
            const Point& point = out.points[static_cast<std::size_t>(k)];
            if (point.died != Point::kOpen) continue;
            if (point.x + smallest > space.length || point.y + smallest > space.width ||
                point.z + smallest > space.height)
                continue;
            // The first point that takes the item decides where it goes; of
            // the orientations feasible there, the one that divides the free
            // room best is taken (the earliest of equals).
            int room[3] = {-1, -1, -1};
            for (int o = first; o < last; ++o) {
                const Orientation& orientation = problem.orientations[static_cast<std::size_t>(o)];
                if (!feasible(problem, space, out.boxes, point, orientation)) continue;
                if (last - first == 1) {
                    chosenPoint = k;
                    chosenOrientation = o;
                    break;
                }
                if (room[0] < 0) freeRoom(space, out.boxes, point, room);
                const int fit = (room[0] % orientation.length == 0 ? 1 : 0) +
                                (room[1] % orientation.width == 0 ? 1 : 0) +
                                (room[2] % orientation.height == 0 ? 1 : 0);
                if (fit > chosenFit) {
                    chosenPoint = k;
                    chosenOrientation = o;
                    chosenFit = fit;
                }
            }
        }
        if (chosenPoint < 0) {
            out.boxOf[static_cast<std::size_t>(position)] = -1;
            continue;
        }
        out.boxOf[static_cast<std::size_t>(position)] = static_cast<int>(out.boxes.size());
        const Point at = out.points[static_cast<std::size_t>(chosenPoint)];
        place(out, space, at, problem.orientations[static_cast<std::size_t>(chosenOrientation)],
              item, position);
    }

    out.loadedVolume = 0;
    out.loadedWeight = 0;
    out.usedLength = 0;
    for (const Box& box : out.boxes) {
        out.loadedVolume += static_cast<std::int64_t>(box.length) * box.width *
                            static_cast<std::int64_t>(box.height);
        out.loadedWeight += problem.items[static_cast<std::size_t>(box.item)].weight;
        out.usedLength = std::max(out.usedLength, box.x + box.length);
    }
    out.stackingExcess = -1;
    out.unloadingBlocks = -1;
    out.balancePerMille = -1;
}

std::int64_t stackingExcess(const ContainerLoadingProblem& problem, const Packing& packing) {
    if (packing.stackingExcess >= 0) return packing.stackingExcess;
    packing.stackingExcess = 0;
    if (!problem.anyLoadLimit) return 0;
    const std::vector<Box>& boxes = packing.boxes;
    const std::size_t count = boxes.size();
    bool limited = false;
    for (const Box& box : boxes) {
        limited = limited || problem.items[static_cast<std::size_t>(box.item)].maxLoadOnTop;
    }
    if (!limited) return 0;

    // Top down: every item above a given one has a higher base, so by the
    // time an item is reached everything that rests on it has been passed
    // down to it.
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&boxes](std::size_t a, std::size_t b) { return boxes[a].z > boxes[b].z; });
    std::vector<double> load(count, 0.0);
    std::vector<std::pair<std::size_t, double>> below;
    for (std::size_t index : order) {
        const Box& box = boxes[index];
        if (box.z == 0) continue;  // the floor carries it
        const double passed =
                static_cast<double>(problem.items[static_cast<std::size_t>(box.item)].weight) +
                load[index];
        if (passed == 0.0) continue;
        below.clear();
        double contact = 0.0;
        for (std::size_t other = 0; other < count; ++other) {
            const Box& under = boxes[other];
            if (under.z + under.height != box.z) continue;
            const int dx = shared(box.x, box.length, under.x, under.length);
            if (dx == 0) continue;
            const int dy = shared(box.y, box.width, under.y, under.width);
            if (dy == 0) continue;
            const double area = static_cast<double>(dx) * static_cast<double>(dy);
            below.emplace_back(other, area);
            contact += area;
        }
        // The decoder never places an item above the floor without support;
        // the guard keeps this function from relying on that.
        if (contact <= 0.0) continue;
        for (const auto& [other, area] : below) load[other] += passed * area / contact;
    }
    std::int64_t excess = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& limit = problem.items[static_cast<std::size_t>(boxes[index].item)].maxLoadOnTop;
        if (!limit) continue;
        const double carried = static_cast<double>(*limit);
        const double over = load[index] - carried;
        // A whole unit per started unit. The tolerance absorbs the rounding
        // of the shares, which add up to a whole weight again; double's
        // rounding grows with the load (about 1e-5 at 1e11), so the tolerance
        // is relative: 1e-12 of the load, at least 1e-6. Below 0.1 of a unit
        // up to loads of 1e11, so a real excess of a whole unit always counts.
        const double tolerance = std::max(1e-6, 1e-12 * std::max(load[index], carried));
        if (over > tolerance) excess += static_cast<std::int64_t>(std::ceil(over - tolerance));
    }
    packing.stackingExcess = excess;
    return excess;
}

std::int64_t unloadingBlocks(const ContainerLoadingProblem& problem, const Packing& packing) {
    if (packing.unloadingBlocks >= 0) return packing.unloadingBlocks;
    packing.unloadingBlocks = 0;
    if (!problem.anyUnloadOrder) return 0;
    const std::vector<Box>& boxes = packing.boxes;
    std::int64_t blocks = 0;
    for (const Box& a : boxes) {
        const auto& first = problem.items[static_cast<std::size_t>(a.item)].unloadOrder;
        if (!first) continue;
        for (const Box& b : boxes) {
            const auto& second = problem.items[static_cast<std::size_t>(b.item)].unloadOrder;
            if (!second || !(*first < *second)) continue;
            const bool overX = shared(a.x, a.length, b.x, b.length) > 0;
            const bool overY = shared(a.y, a.width, b.y, b.width) > 0;
            const bool overZ = shared(a.z, a.height, b.z, b.height) > 0;
            const bool inFront = b.x >= a.x + a.length && overY && overZ;
            const bool onTop = b.z >= a.z + a.height && overX && overY;
            if (inFront || onTop) ++blocks;
        }
    }
    packing.unloadingBlocks = blocks;
    return blocks;
}

int balancePerMille(const ContainerLoadingProblem& problem, int container, const Packing& packing) {
    if (packing.balancePerMille >= 0) return packing.balancePerMille;
    const Container& space = problem.containers[static_cast<std::size_t>(container)];
    double weight = 0.0;
    double momentX = 0.0;
    double momentY = 0.0;
    for (const Box& box : packing.boxes) {
        const double w =
                static_cast<double>(problem.items[static_cast<std::size_t>(box.item)].weight);
        weight += w;
        momentX += w * (static_cast<double>(box.x) + 0.5 * static_cast<double>(box.length));
        momentY += w * (static_cast<double>(box.y) + 0.5 * static_cast<double>(box.width));
    }
    int result = 0;
    if (weight > 0.0) {
        const double halfLength = 0.5 * static_cast<double>(space.length);
        const double halfWidth = 0.5 * static_cast<double>(space.width);
        const double dx = momentX / weight - halfLength;
        const double dy = momentY / weight - halfWidth;
        const double share = std::sqrt(dx * dx + dy * dy) /
                             std::sqrt(halfLength * halfLength + halfWidth * halfWidth);
        result = static_cast<int>(std::floor(1000.0 * share + 0.5));
    }
    packing.balancePerMille = result;
    return result;
}

// ---------------------------------------------------------------------------
// PackingCache
// ---------------------------------------------------------------------------

namespace {

// A 64-bit hash taken a word at a time: each word is folded in and the state
// mixed with splitmix64's finalizer (a bijection), so one changed word always
// changes the state. A reset runs it over every item, so it is kept cheap.
struct Hash {
    std::uint64_t value = 0x9e3779b97f4a7c15ull;
    void add(std::uint64_t word) {
        std::uint64_t x = value ^ word;
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        x ^= x >> 31;
        value = x + 0x9e3779b97f4a7c15ull;
    }
    void add(double number) {
        std::uint64_t word = 0;
        std::memcpy(&word, &number, sizeof(word));
        add(word);
    }
    void add(std::int64_t number) { add(static_cast<std::uint64_t>(number)); }
    void add(int number) { add(static_cast<std::uint64_t>(static_cast<std::int64_t>(number))); }
};

// Everything a decode or one of the kept costs reads.
std::uint64_t factsOf(const ContainerLoadingProblem& problem) {
    Hash hash;
    hash.add(problem.minSupportRatio);
    hash.add(static_cast<std::int64_t>(problem.containers.size()));
    for (const Container& container : problem.containers) {
        hash.add(container.length);
        hash.add(container.width);
        hash.add(container.height);
    }
    hash.add(static_cast<std::int64_t>(problem.items.size()));
    for (const Item& item : problem.items) {
        hash.add(item.length);
        hash.add(item.width);
        hash.add(item.height);
        hash.add(static_cast<int>(item.rotations));
        hash.add(item.weight);
        hash.add(item.maxLoadOnTop ? *item.maxLoadOnTop : std::int64_t{-1});
        hash.add(item.unloadOrder ? 1 : 0);
        hash.add(item.unloadOrder ? *item.unloadOrder : 0);
    }
    return hash.value;
}

int commonPrefix(const std::vector<int>& a, const std::vector<int>& b) {
    const std::size_t limit = std::min(a.size(), b.size());
    std::size_t same = 0;
    while (same < limit && a[same] == b[same]) ++same;
    return static_cast<int>(same);
}

}  // namespace

void PackingCache::reset(const ContainerLoadingProblem& problem) {
    const std::uint64_t facts = factsOf(problem);
    if (facts == fingerprint_ && containers_.size() == problem.containers.size()) return;
    fingerprint_ = facts;
    containers_.assign(problem.containers.size(), Slots());
    clock_ = 0;
    decodes_ = 0;
    hits_ = 0;
}

const Packing& PackingCache::packing(const ContainerLoadingProblem& problem, int container,
                                     const std::vector<int>& members) {
    Slots& slots = containers_[static_cast<std::size_t>(container)];
    ++clock_;
    for (int i = 0; i < kSlots; ++i) {
        if (!slots.valid[i] || slots.slot[i].members != members) continue;
        slots.used[i] = clock_;
        ++hits_;
        if (slots.pinNext && members == slots.pending) {
            slots.pinned = i;
            slots.pinNext = false;
        }
        return slots.slot[i];
    }

    int source = -1;
    int sourcePrefix = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (!slots.valid[i]) continue;
        const int prefix = commonPrefix(slots.slot[i].members, members);
        if (prefix > sourcePrefix) {
            source = i;
            sourcePrefix = prefix;
        }
    }
    int target = -1;
    for (int i = 0; i < kSlots; ++i) {
        if (i == slots.pinned && !slots.pinNext) continue;
        if (target < 0 || !slots.valid[i] ||
            (slots.valid[target] && slots.used[i] < slots.used[target]))
            target = i;
        if (!slots.valid[target]) break;
    }
    if (source >= 0 && source != target) slots.slot[target] = slots.slot[source];
    decode(problem, container, members, slots.slot[target], source >= 0 ? sourcePrefix : 0);
    slots.valid[target] = true;
    slots.used[target] = clock_;
    ++decodes_;
    if (slots.pinNext && members == slots.pending) {
        slots.pinned = target;
        slots.pinNext = false;
    }
    return slots.slot[target];
}

void PackingCache::pin(int container, const std::vector<int>& members) {
    Slots& slots = containers_[static_cast<std::size_t>(container)];
    for (int i = 0; i < kSlots; ++i) {
        if (slots.valid[i] && slots.slot[i].members == members) {
            slots.pinned = i;
            slots.pinNext = false;
            return;
        }
    }
    slots.pending = members;
    slots.pinNext = true;
}

}  // namespace cl
