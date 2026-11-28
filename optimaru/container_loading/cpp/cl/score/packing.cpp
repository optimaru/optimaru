#include "cl/score/packing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

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

// How far a load passed down may exceed what an item carries before it
// counts: it absorbs the rounding of the shares (see stackingExcess, the one
// other user; the decoder and the cost must agree).
double loadTolerance(double load, double carried) {
    return std::max(1e-6, 1e-12 * std::max(load, carried));
}

// Whether `load` is more than an item that carries `carried` takes.
bool over(double load, double carried) { return load - carried > loadTolerance(load, carried); }

// The loads resting on the boxes that carry a limit, passed down again from
// the boxes as they stand: every box's supports were placed before it, so the
// boxes from last to first are an order in which everything above a box comes
// before it. Only the boxes whose weight reaches a limit (bearsLimit) pass
// anything on; the others' loads stay 0 and are never read.
void recomputeCarried(const ContainerLoadingProblem& problem, Packing& packing) {
    const std::size_t count = packing.boxes.size();
    packing.carried.assign(count, 0.0);
    for (std::size_t b = count; b-- > 0;) {
        if (!packing.bearsLimit[b]) continue;
        const double passed =
                static_cast<double>(
                        problem.items[static_cast<std::size_t>(packing.boxes[b].item)].weight) +
                packing.carried[b];
        if (passed == 0.0) continue;
        for (int s = packing.supportStart[b]; s < packing.supportStart[b + 1]; ++s) {
            packing.carried[static_cast<std::size_t>(
                    packing.supportBox[static_cast<std::size_t>(s)])] +=
                    passed * packing.supportShare[static_cast<std::size_t>(s)];
        }
    }
}

// How many items stand in the tallest column on each box (Packing::above),
// again from what the boxes stand on: a box's supports come before it, so the
// boxes from last to first reach every box after everything on it.
void recomputeAbove(Packing& packing) {
    const std::size_t count = packing.boxes.size();
    packing.above.assign(count, 0);
    for (std::size_t b = count; b-- > 0;) {
        for (int s = packing.supportStart[b]; s < packing.supportStart[b + 1]; ++s) {
            int& below = packing.above[static_cast<std::size_t>(
                    packing.supportBox[static_cast<std::size_t>(s)])];
            below = std::max(below, packing.above[b] + 1);
        }
    }
}

// Forget everything the decode did from list position `keep` on.
void truncate(const ContainerLoadingProblem& problem, Packing& packing, int keep) {
    std::size_t kept = packing.boxes.size();
    while (kept > 0 && packing.boxes[kept - 1].position >= keep) --kept;
    for (std::vector<int>& cell : packing.cells) {
        while (!cell.empty() && cell.back() >= static_cast<int>(kept)) cell.pop_back();
    }
    packing.tops.erase(std::remove_if(packing.tops.begin(), packing.tops.end(),
                                      [kept](const std::pair<int, int>& top) {
                                          return top.second >= static_cast<int>(kept);
                                      }),
                       packing.tops.end());
    auto& points = packing.points;
    points.erase(std::remove_if(points.begin(), points.end(),
                                [keep](const Point& point) { return point.born >= keep; }),
                 points.end());
    // The rooms of the free points, as the boxes kept leave them: a point
    // covered again, or one whose ray a removed box stopped, is measured anew.
    for (Point& point : points) {
        bool stale = false;
        if (point.died != Point::kOpen) {
            if (point.died < keep) continue;
            point.died = Point::kOpen;
            stale = true;
        }
        if (point.room[0] < 0) continue;  // not measured yet
        // The boxes taken away (still in `boxes` past `kept`) may have stopped
        // the rays of points that stay.
        for (std::size_t r = kept; r < packing.boxes.size() && !stale; ++r) {
            const Box& box = packing.boxes[r];
            const bool inX = box.x <= point.x && point.x < box.x + box.length;
            const bool inY = box.y <= point.y && point.y < box.y + box.width;
            const bool inZ = box.z <= point.z && point.z < box.z + box.height;
            stale = (inY && inZ && box.x >= point.x && point.room[0] == box.x - point.x) ||
                    (inX && inZ && box.y >= point.y && point.room[1] == box.y - point.y) ||
                    (inX && inY && box.z >= point.z && point.room[2] == box.z - point.z);
        }
        if (stale) point.room[0] = -1;
    }
    packing.boxes.resize(kept);
    if (problem.trackSupports()) {
        packing.supportStart.resize(packing.boxes.size() + 1);
        packing.supportBox.resize(static_cast<std::size_t>(packing.supportStart.back()));
        packing.supportShare.resize(static_cast<std::size_t>(packing.supportStart.back()));
    }
    if (problem.anyLoadLimit) {
        packing.bearsLimit.resize(kept);
        recomputeCarried(problem, packing);
    }
    if (problem.anyLayerLimit) {
        packing.level.resize(kept);
        recomputeAbove(packing);
    }
}

// How many cells a container's length is cut into for Packing::cells.
constexpr int kCells = 32;

int cellOf(const Packing& packing, int x) {
    return std::min(static_cast<int>(packing.cells.size()) - 1, x / packing.cellLength);
}

// Calls visit(box) for every box whose x-range may meet [low, high) (high >
// low): once per cell, so a box that spans several cells may be visited more
// than once. visit returns true to stop; the function then returns true.
template <typename Visit>
bool anyInRange(const Packing& packing, int low, int high, Visit&& visit) {
    const int first = cellOf(packing, std::max(0, low));
    const int last = cellOf(packing, std::max(0, high - 1));
    for (int c = first; c <= last; ++c) {
        for (int index : packing.cells[static_cast<std::size_t>(c)]) {
            if (visit(packing.boxes[static_cast<std::size_t>(index)])) return true;
        }
    }
    return false;
}

// Calls visit(index, box) for every box whose top is at height z, in index
// order; visit returns true to stop.
template <typename Visit>
void eachTopAt(const Packing& packing, int z, Visit&& visit) {
    auto at = std::lower_bound(packing.tops.begin(), packing.tops.end(), std::make_pair(z, -1));
    for (; at != packing.tops.end() && at->first == z; ++at) {
        if (visit(at->second, packing.boxes[static_cast<std::size_t>(at->second)])) return;
    }
}

bool feasible(const ContainerLoadingProblem& problem, const Container& container,
              const Packing& packing, const Point& point, const Orientation& o) {
    if (point.x + o.length > container.length || point.y + o.width > container.width ||
        point.z + o.height > container.fillHeight)
        return false;
    if (point.z > 0) {
        // The base rests on the tops that end exactly where it starts. Placed
        // items do not overlap, so the areas add up without counting any
        // twice (and, being whole numbers, to the same sum in any order).
        // Checked first: high up, most points fail here.
        const double base = static_cast<double>(o.length) * static_cast<double>(o.width);
        const double needed = problem.minSupportRatio * base;
        // The tolerance absorbs the rounding of ratio * base; capped at half an
        // area unit, since `supported` is a sum of whole areas, so the decoder
        // never accepts a base a whole unit short (the evaluators check at 1e-6).
        const double enough = needed - std::min(0.5, 1e-9 * std::max(1.0, needed));
        double supported = 0.0;
        bool done = false;
        eachTopAt(packing, point.z, [&](int, const Box& box) {
            const int dx = shared(point.x, o.length, box.x, box.length);
            if (dx == 0) return false;
            const int dy = shared(point.y, o.width, box.y, box.width);
            if (dy == 0) return false;
            supported += static_cast<double>(dx) * static_cast<double>(dy);
            done = supported >= enough;
            return done;
        });
        if (!done) return false;
    }
    return !anyInRange(packing, point.x, point.x + o.length, [&](const Box& box) {
        return point.x < box.x + box.length && box.x < point.x + o.length &&
               point.y < box.y + box.width && box.y < point.y + o.width &&
               point.z < box.z + box.height && box.z < point.z + o.height;
    });
}

// Slide the point (x, y, z) back along one axis until it meets the far face
// of a placed item or the wall at 0; returns its coordinate on that axis.
int slide(const Packing& packing, int x, int y, int z, int axis) {
    int best = 0;
    if (axis == 0) {
        for (const Box& box : packing.boxes) {
            const bool inY = box.y <= y && y < box.y + box.width;
            const bool inZ = box.z <= z && z < box.z + box.height;
            if (inY && inZ && box.x + box.length <= x) best = std::max(best, box.x + box.length);
        }
        return best;
    }
    // Along y or z only the boxes whose x-range holds x can stop it.
    anyInRange(packing, x, x + 1, [&](const Box& box) {
        const bool inX = box.x <= x && x < box.x + box.length;
        const bool inY = box.y <= y && y < box.y + box.width;
        const bool inZ = box.z <= z && z < box.z + box.height;
        if (axis == 1) {
            if (inX && inZ && box.y + box.width <= y) best = std::max(best, box.y + box.width);
        } else {
            if (inX && inY && box.z + box.height <= z) best = std::max(best, box.z + box.height);
        }
        return false;
    });
    return best;
}

// How far the room reaches from a point along x, y and z: up to the next
// placed item the ray from the point meets, or the wall.
void freeRoom(const Container& container, const Packing& packing, const Point& point, int room[3]) {
    int endX = container.length;
    int endY = container.width;
    int endZ = container.fillHeight;
    // Along y and z only the boxes whose x-range holds the point's x can stop
    // the ray: those of its cell.
    anyInRange(packing, point.x, point.x + 1, [&](const Box& box) {
        if (!(box.x <= point.x && point.x < box.x + box.length)) return false;
        const bool inY = box.y <= point.y && point.y < box.y + box.width;
        const bool inZ = box.z <= point.z && point.z < box.z + box.height;
        if (inZ && box.y >= point.y) endY = std::min(endY, box.y);
        if (inY && box.z >= point.z) endZ = std::min(endZ, box.z);
        return false;
    });
    // Along x: the cells from the point's on, until a cell starts beyond the
    // nearest box found.
    const int cellCount = static_cast<int>(packing.cells.size());
    for (int c = cellOf(packing, point.x); c < cellCount && c * packing.cellLength < endX; ++c) {
        for (int index : packing.cells[static_cast<std::size_t>(c)]) {
            const Box& box = packing.boxes[static_cast<std::size_t>(index)];
            if (box.x < point.x || box.x >= endX) continue;
            if (box.y <= point.y && point.y < box.y + box.width && box.z <= point.z &&
                point.z < box.z + box.height)
                endX = box.x;
        }
    }
    room[0] = endX - point.x;
    room[1] = endY - point.y;
    room[2] = endZ - point.z;
}

// Whether an item that leaves at `order`, put at `point` in orientation `o`,
// would block a placed item or be blocked by one (the pairs
// unloadingBlocks counts).
bool blocks(const ContainerLoadingProblem& problem, const std::vector<Box>& boxes, int order,
            const Point& point, const Orientation& o) {
    for (const Box& box : boxes) {
        const auto& other = problem.items[static_cast<std::size_t>(box.item)].unloadOrder;
        if (!other || *other == order) continue;
        const bool overX = shared(point.x, o.length, box.x, box.length) > 0;
        const bool overY = shared(point.y, o.width, box.y, box.width) > 0;
        const bool overZ = shared(point.z, o.height, box.z, box.height) > 0;
        if (*other > order) {
            // The placed item leaves later: it must not be in the new one's way.
            if (box.x >= point.x + o.length && overY && overZ) return true;
            if (box.z >= point.z + o.height && overX && overY) return true;
        } else {
            // The placed item leaves earlier: the new one must not be in its way.
            if (point.x >= box.x + box.length && overY && overZ) return true;
            if (point.z >= box.z + box.height && overX && overY) return true;
        }
    }
    return false;
}

// What a box at `point` in orientation `o` would stand on: the placed boxes
// whose tops are at its base, each with its share of the contact area.
void supportsOf(const Packing& packing, const Point& point, const Orientation& o,
                std::vector<std::pair<int, double>>& out) {
    out.clear();
    if (point.z == 0) return;
    double contact = 0.0;
    eachTopAt(packing, point.z, [&](int index, const Box& box) {
        const int dx = shared(point.x, o.length, box.x, box.length);
        if (dx == 0) return false;
        const int dy = shared(point.y, o.width, box.y, box.width);
        if (dy == 0) return false;
        const double area = static_cast<double>(dx) * static_cast<double>(dy);
        out.emplace_back(index, area);
        contact += area;
        return false;
    });
    if (contact <= 0.0) {
        out.clear();
        return;
    }
    for (auto& support : out) support.second /= contact;
}

// The load `weight`, resting on `supports`, passed down through the placed
// boxes to those that carry a limit or stand on one that does: `added` gets
// (box, what it adds on that box), highest box index first. A box's supports
// come before it in `boxes`, so taking the entries by falling index passes
// everything a box receives on before the box is reached.
void passDown(const Packing& packing, const std::vector<std::pair<int, double>>& supports,
              double weight, std::vector<std::pair<int, double>>& added) {
    added.clear();
    const auto add = [&added](std::size_t from, int box, double load) {
        std::size_t at = from;
        while (at < added.size() && added[at].first > box) ++at;
        if (at < added.size() && added[at].first == box) {
            added[at].second += load;
        } else {
            added.insert(added.begin() + static_cast<std::ptrdiff_t>(at), {box, load});
        }
    };
    for (const auto& [box, share] : supports) {
        if (packing.bearsLimit[static_cast<std::size_t>(box)]) add(0, box, weight * share);
    }
    for (std::size_t next = 0; next < added.size(); ++next) {
        const auto [box, load] = added[next];
        for (int s = packing.supportStart[static_cast<std::size_t>(box)];
             s < packing.supportStart[static_cast<std::size_t>(box) + 1]; ++s) {
            const int below = packing.supportBox[static_cast<std::size_t>(s)];
            if (packing.bearsLimit[static_cast<std::size_t>(below)])
                add(next + 1, below, load * packing.supportShare[static_cast<std::size_t>(s)]);
        }
    }
}

// Whether an item of `weight` at `point` in orientation `o` would put more on
// a placed box than its maxLoadOnTop.
bool overloads(const ContainerLoadingProblem& problem, const Packing& packing, double weight,
               const Point& point, const Orientation& o,
               std::vector<std::pair<int, double>>& supports,
               std::vector<std::pair<int, double>>& added) {
    if (point.z == 0 || weight <= 0.0) return false;
    supportsOf(packing, point, o, supports);
    passDown(packing, supports, weight, added);
    for (const auto& [box, load] : added) {
        const auto& limit =
                problem.items[static_cast<std::size_t>(
                                      packing.boxes[static_cast<std::size_t>(box)].item)]
                        .maxLoadOnTop;
        if (limit && over(packing.carried[static_cast<std::size_t>(box)] + load,
                          static_cast<double>(*limit)))
            return true;
    }
    return false;
}

// For every box under box `from`, directly or not, how many layers up `from`
// is from it at most (the longest chain of supports between them); -1 for the
// others. `from` itself gets 0.
void reachDown(const Packing& packing, int from, std::vector<int>& reach) {
    reach.assign(packing.boxes.size(), -1);
    reach[static_cast<std::size_t>(from)] = 0;
    for (int b = from; b >= 0; --b) {
        const int at = reach[static_cast<std::size_t>(b)];
        if (at < 0) continue;
        for (int s = packing.supportStart[static_cast<std::size_t>(b)];
             s < packing.supportStart[static_cast<std::size_t>(b) + 1]; ++s) {
            int& below = reach[static_cast<std::size_t>(
                    packing.supportBox[static_cast<std::size_t>(s)])];
            below = std::max(below, at + 1);
        }
    }
}

// Whether item `item` at `point` in orientation `o` would stand on an item
// against a stacking rule, have one stand on it against a rule (an item
// placed earlier over an overhang), or make a column taller than an item's
// maxStackLayers: its own, or that of an item under it. The columns are the
// ones the supports recorded at placement make: a box that ends under an
// earlier box's overhang is not counted as carrying it here (as for the load
// limits), while stackingViolations, from the geometry, counts it. This only
// steers the decoder; the cost counts what is left.
bool breaksStacking(const ContainerLoadingProblem& problem, const Packing& packing, int item,
                    const Point& point, const Orientation& o, std::vector<int>& reach) {
    const auto touches = [&](const Box& box) {
        return shared(point.x, o.length, box.x, box.length) > 0 &&
               shared(point.y, o.width, box.y, box.width) > 0;
    };
    if (problem.anyStackRule && problem.stackGroupOf[static_cast<std::size_t>(item)] >= 0) {
        bool broken = false;
        if (point.z > 0) {
            eachTopAt(packing, point.z, [&](int, const Box& box) {
                broken = touches(box) && problem.forbids(item, box.item);
                return broken;
            });
            if (broken) return true;
        }
        const int top = point.z + o.height;
        if (anyInRange(packing, point.x, point.x + o.length, [&](const Box& box) {
                return box.z == top && touches(box) && problem.forbids(box.item, item);
            }))
            return true;
    }
    if (!problem.anyLayerLimit) return false;
    // The layer it would be in, and the boxes it would stand on, right under it.
    int level = 1;
    int highest = -1;
    reach.assign(packing.boxes.size(), -1);
    if (point.z > 0) {
        eachTopAt(packing, point.z, [&](int index, const Box& box) {
            if (!touches(box)) return false;
            level = std::max(level, packing.level[static_cast<std::size_t>(index)] + 1);
            reach[static_cast<std::size_t>(index)] = 1;
            highest = std::max(highest, index);
            return false;
        });
    }
    const auto& own = problem.items[static_cast<std::size_t>(item)].maxStackLayers;
    if (own && level > *own) return true;
    for (int b = highest; b >= 0; --b) {
        const int at = reach[static_cast<std::size_t>(b)];
        if (at < 0) continue;
        const auto& limit = problem.items[static_cast<std::size_t>(
                                                  packing.boxes[static_cast<std::size_t>(b)].item)]
                                    .maxStackLayers;
        if (limit && packing.level[static_cast<std::size_t>(b)] +
                                     std::max(packing.above[static_cast<std::size_t>(b)], at) >
                             *limit)
            return true;
        for (int s = packing.supportStart[static_cast<std::size_t>(b)];
             s < packing.supportStart[static_cast<std::size_t>(b) + 1]; ++s) {
            int& below = reach[static_cast<std::size_t>(
                    packing.supportBox[static_cast<std::size_t>(s)])];
            below = std::max(below, at + 1);
        }
    }
    return false;
}

void addPoint(Packing& packing, const Container& container, int x, int y, int z, int position) {
    if (x >= container.length || y >= container.width || z >= container.fillHeight) return;
    const bool inside = anyInRange(packing, x, x + 1, [&](const Box& box) {
        return box.x <= x && x < box.x + box.length && box.y <= y && y < box.y + box.width &&
               box.z <= z && z < box.z + box.height;
    });
    if (inside) return;  // inside a placed item
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
    point.room[0] = -1;  // measured when first asked for
    points.insert(at, point);
}

// What a box about to be placed at `at` in orientation `o` stands on, kept in
// the Packing (Packing::supportStart and what follows), with its layer when
// some item sets a layer limit; returns whether a load limit is under it. Out
// of line: only problems with load or layer limits call it, and it stays out
// of place()'s way for the others.
[[gnu::noinline]] bool recordSupports(const ContainerLoadingProblem& problem, Packing& packing,
                                      const Point& at, const Orientation& o, int item,
                                      std::vector<std::pair<int, double>>& supports) {
    supportsOf(packing, at, o, supports);
    bool reachesLimit = false;
    int level = 1;
    for (const auto& [below, share] : supports) {
        packing.supportBox.push_back(below);
        packing.supportShare.push_back(share);
        if (problem.anyLoadLimit)
            reachesLimit = reachesLimit || packing.bearsLimit[static_cast<std::size_t>(below)];
        if (problem.anyLayerLimit)
            level = std::max(level, packing.level[static_cast<std::size_t>(below)] + 1);
    }
    packing.supportStart.push_back(static_cast<int>(packing.supportBox.size()));
    if (problem.anyLoadLimit) {
        packing.bearsLimit.push_back(
                reachesLimit || problem.items[static_cast<std::size_t>(item)].maxLoadOnTop ? 1 : 0);
    }
    if (problem.anyLayerLimit) {
        packing.level.push_back(level);
        packing.above.push_back(0);
    }
    return reachesLimit;
}

// The columns under the box just placed (index `placed`) are a layer taller
// where it tops them.
[[gnu::noinline]] void raiseColumns(Packing& packing, int placed, std::vector<int>& reach) {
    reachDown(packing, placed, reach);
    for (std::size_t b = 0; b < reach.size(); ++b) {
        if (reach[b] > packing.above[b]) packing.above[b] = reach[b];
    }
}

void place(const ContainerLoadingProblem& problem, Packing& packing, const Container& container,
           const Point& at, const Orientation& o, int item, int position,
           std::vector<std::pair<int, double>>& supports, std::vector<int>& reach) {
    const bool reachesLimit =
            problem.trackSupports() && recordSupports(problem, packing, at, o, item, supports);
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
    const int index = static_cast<int>(packing.boxes.size());
    packing.boxes.push_back(box);
    if (problem.anyLayerLimit) raiseColumns(packing, index, reach);
    if (problem.anyLoadLimit) {
        // The loads passed down again, always the same way (recomputeCarried),
        // so that a decode resumed from a prefix has exactly the loads of one
        // made from scratch. Only the loads on boxes that carry a limit are
        // read, and those change only when the new box's weight reaches one.
        if (reachesLimit && problem.items[static_cast<std::size_t>(item)].weight > 0)
            recomputeCarried(problem, packing);
        else
            packing.carried.push_back(0.0);
    }
    for (int c = cellOf(packing, box.x); c <= cellOf(packing, box.x + box.length - 1); ++c)
        packing.cells[static_cast<std::size_t>(c)].push_back(index);
    const std::pair<int, int> top(box.z + box.height, index);
    packing.tops.insert(std::lower_bound(packing.tops.begin(), packing.tops.end(), top), top);
    // The rays from the free points that the new box now stops.
    for (Point& point : packing.points) {
        if (point.died != Point::kOpen || point.room[0] < 0) continue;
        const bool inX = box.x <= point.x && point.x < box.x + box.length;
        const bool inY = box.y <= point.y && point.y < box.y + box.width;
        const bool inZ = box.z <= point.z && point.z < box.z + box.height;
        if (inY && inZ && box.x >= point.x)
            point.room[0] = std::min(point.room[0], box.x - point.x);
        if (inX && inZ && box.y >= point.y)
            point.room[1] = std::min(point.room[1], box.y - point.y);
        if (inX && inY && box.z >= point.z)
            point.room[2] = std::min(point.room[2], box.z - point.z);
    }
    const int farX = box.x + box.length;
    const int farY = box.y + box.width;
    const int farZ = box.z + box.height;
    addPoint(packing, container, farX, slide(packing, farX, box.y, box.z, 1), box.z, position);
    addPoint(packing, container, farX, box.y, slide(packing, farX, box.y, box.z, 2), position);
    addPoint(packing, container, slide(packing, box.x, farY, box.z, 0), farY, box.z, position);
    addPoint(packing, container, box.x, farY, slide(packing, box.x, farY, box.z, 2), position);
    addPoint(packing, container, slide(packing, box.x, box.y, farZ, 0), box.y, farZ, position);
    addPoint(packing, container, box.x, slide(packing, box.x, box.y, farZ, 1), farZ, position);
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
        Point origin;
        origin.room[0] = space.length;
        origin.room[1] = space.width;
        origin.room[2] = space.fillHeight;
        out.points.push_back(origin);
        for (std::vector<int>& cell : out.cells) cell.clear();
        out.tops.clear();
        out.carried.clear();
        out.supportStart.assign(problem.trackSupports() ? 1 : 0, 0);
        out.supportBox.clear();
        out.supportShare.clear();
        out.bearsLimit.clear();
        out.level.clear();
        out.above.clear();
    } else {
        truncate(problem, out, keep);
    }
    // One cell per 16 items of the list, up to kCells: a short list is
    // checked box by box, where cells would cost more than they save. When
    // the count changes the kept boxes are filed again; the cells change how
    // long a decode takes and never what it gives.
    const int cells = std::clamp(count / 16, 1, kCells);
    if (static_cast<int>(out.cells.size()) != cells) {
        out.cellLength = std::max(1, (space.length + cells - 1) / cells);
        out.cells.assign(static_cast<std::size_t>(cells), std::vector<int>());
        for (std::size_t b = 0; b < out.boxes.size(); ++b) {
            const Box& box = out.boxes[b];
            for (int c = cellOf(out, box.x); c <= cellOf(out, box.x + box.length - 1); ++c)
                out.cells[static_cast<std::size_t>(c)].push_back(static_cast<int>(b));
        }
    }
    out.members.assign(members.begin(), members.end());
    out.boxOf.resize(static_cast<std::size_t>(count));

    std::vector<std::pair<int, double>> supports;
    std::vector<std::pair<int, double>> added;
    std::vector<int> reach;
    // The weight placed so far, which an optional item may not take above the
    // container's limit.
    const bool weighed = problem.anyOptional && space.maxWeight.has_value();
    std::int64_t placedWeight = 0;
    if (weighed) {
        for (const Box& kept : out.boxes)
            placedWeight += problem.items[static_cast<std::size_t>(kept.item)].weight;
    }
    for (int position = keep; position < count; ++position) {
        const int item = members[static_cast<std::size_t>(position)];
        const Item& box = problem.items[static_cast<std::size_t>(item)];
        if (weighed && box.optional && placedWeight + box.weight > *space.maxWeight) {
            out.boxOf[static_cast<std::size_t>(position)] = -1;
            continue;
        }
        const int smallest = problem.smallestSide[static_cast<std::size_t>(item)];
        const int first = problem.orientationStart[static_cast<std::size_t>(item)];
        const int last = problem.orientationStart[static_cast<std::size_t>(item) + 1];
        // What a place may break: 0 nothing, 1 an unloading order, 2 a load
        // limit, a stacking rule or a layer limit. Per class, the first point
        // that takes the item in an orientation of that class, and of those orientations the one
        // that divides the free room best (the earliest of equals). The search stops at the first
        // point of class 0; without orders and limits every place is of class 0.
        const bool ordered = problem.anyUnloadOrder && box.unloadOrder.has_value();
        const bool limited = problem.anyLoadLimit && box.weight > 0;
        const bool stacked = problem.anyStackRule || problem.anyLayerLimit;
        const double weight = static_cast<double>(box.weight);
        // The highest base it can have: on the floor when bottomOnly.
        const int topBase = box.bottomOnly ? 0 : space.fillHeight - smallest;
        int chosenPoint[3] = {-1, -1, -1};
        int chosenOrientation[3] = {-1, -1, -1};
        int chosenFit[3] = {-1, -1, -1};
        const int pointCount = static_cast<int>(out.points.size());
        for (int k = 0; k < pointCount && chosenPoint[0] < 0; ++k) {
            Point& point = out.points[static_cast<std::size_t>(k)];
            if (point.died != Point::kOpen) continue;
            if (point.x + smallest > space.length || point.y + smallest > space.width ||
                point.z > topBase)
                continue;
            if (point.room[0] < 0) freeRoom(space, out, point, point.room);
            for (int o = first; o < last; ++o) {
                const Orientation& orientation = problem.orientations[static_cast<std::size_t>(o)];
                // Longer than the room along an axis: the item in the way overlaps it.
                if (orientation.length > point.room[0] || orientation.width > point.room[1] ||
                    orientation.height > point.room[2])
                    continue;
                if (!feasible(problem, space, out, point, orientation)) continue;
                int kind = 0;
                if ((limited &&
                     overloads(problem, out, weight, point, orientation, supports, added)) ||
                    (stacked && breaksStacking(problem, out, item, point, orientation, reach)))
                    kind = 2;
                else if (ordered &&
                         blocks(problem, out.boxes, *box.unloadOrder, point, orientation))
                    kind = 1;
                // A class already taken by an earlier point stays with it.
                if (chosenPoint[kind] >= 0 && chosenPoint[kind] != k) continue;
                int fit = 0;
                if (last - first > 1) {
                    const int* room = point.room;
                    fit = (room[0] % orientation.length == 0 ? 1 : 0) +
                          (room[1] % orientation.width == 0 ? 1 : 0) +
                          (room[2] % orientation.height == 0 ? 1 : 0);
                    // An item that leaves at a stop: the shallowest
                    // orientation along x first, so that a stop builds a
                    // thin wall across the space and not a long row.
                    if (ordered) fit += 4 * (kMaxLength - orientation.length);
                }
                if (fit > chosenFit[kind]) {
                    chosenPoint[kind] = k;
                    chosenOrientation[kind] = o;
                    chosenFit[kind] = fit;
                }
            }
        }
        int kind = 0;
        while (kind < 3 && chosenPoint[kind] < 0) ++kind;
        if (kind == 3) {
            out.boxOf[static_cast<std::size_t>(position)] = -1;
            continue;
        }
        out.boxOf[static_cast<std::size_t>(position)] = static_cast<int>(out.boxes.size());
        placedWeight += box.weight;
        const Point at = out.points[static_cast<std::size_t>(chosenPoint[kind])];
        place(problem, out, space, at,
              problem.orientations[static_cast<std::size_t>(chosenOrientation[kind])], item,
              position, supports, reach);
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
    out.stackingViolations = -1;
    out.balanceExcess = -1;
    out.unplacedCost = -1;
}

void measureRoom(const ContainerLoadingProblem& problem, int container, const Packing& packing,
                 const Point& point, int room[3]) {
    freeRoom(problem.containers[static_cast<std::size_t>(container)], packing, point, room);
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
        const double tolerance = loadTolerance(load[index], carried);
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

// Over the unplaced items of the list: their priorities, the optional ones
// apart, and the weight of the optional ones. Kept with the decode.
void tallyUnplaced(const ContainerLoadingProblem& problem, const Packing& packing) {
    packing.unplacedCost = 0;
    packing.optionalCost = 0;
    packing.optionalWeight = 0;
    for (std::size_t position = 0; position < packing.members.size(); ++position) {
        if (packing.boxOf[position] >= 0) continue;
        const Item& item = problem.items[static_cast<std::size_t>(packing.members[position])];
        if (item.optional) {
            packing.optionalCost += item.priority;
            packing.optionalWeight += item.weight;
        } else {
            packing.unplacedCost += item.priority;
        }
    }
}

std::int64_t unplacedCost(const ContainerLoadingProblem& problem, const Packing& packing) {
    if (!problem.anyOptional && !problem.anyPriority) return packing.unplaced();
    if (packing.unplacedCost < 0) tallyUnplaced(problem, packing);
    return packing.unplacedCost;
}

std::int64_t optionalCost(const ContainerLoadingProblem& problem, const Packing& packing) {
    if (!problem.anyOptional) return 0;
    if (packing.unplacedCost < 0) tallyUnplaced(problem, packing);
    return packing.optionalCost;
}

std::int64_t unplacedOptionalWeight(const ContainerLoadingProblem& problem,
                                    const Packing& packing) {
    if (!problem.anyOptional) return 0;
    if (packing.unplacedCost < 0) tallyUnplaced(problem, packing);
    return packing.optionalWeight;
}

std::int64_t stackingViolations(const ContainerLoadingProblem& problem, const Packing& packing) {
    if (packing.stackingViolations >= 0) return packing.stackingViolations;
    packing.stackingViolations = 0;
    if (!problem.anyStackRule && !problem.anyLayerLimit) return 0;
    const std::vector<Box>& boxes = packing.boxes;
    const std::size_t count = boxes.size();
    // Bottom up: what an item stands on has a lower base, so its layer is known
    // by the time the item is reached.
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&boxes](std::size_t a, std::size_t b) { return boxes[a].z < boxes[b].z; });
    std::vector<std::size_t> underStart(count + 1, 0);
    std::vector<std::size_t> under;  // per box in `order`, the boxes right under it
    std::vector<int> level(count, 1);
    std::int64_t violations = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const Box& box = boxes[order[k]];
        for (std::size_t other = 0; other < count && box.z > 0; ++other) {
            const Box& below = boxes[other];
            if (below.z + below.height != box.z ||
                shared(box.x, box.length, below.x, below.length) == 0 ||
                shared(box.y, box.width, below.y, below.width) == 0)
                continue;
            under.push_back(other);
            level[order[k]] = std::max(level[order[k]], level[other] + 1);
            if (problem.anyStackRule && problem.forbids(box.item, below.item)) ++violations;
        }
        underStart[k + 1] = under.size();
    }
    if (problem.anyLayerLimit) {
        // Top down: everything on an item is reached before it.
        std::vector<int> above(count, 0);
        for (std::size_t k = count; k-- > 0;) {
            for (std::size_t u = underStart[k]; u < underStart[k + 1]; ++u)
                above[under[u]] = std::max(above[under[u]], above[order[k]] + 1);
        }
        for (std::size_t b = 0; b < count; ++b) {
            const auto& limit =
                    problem.items[static_cast<std::size_t>(boxes[b].item)].maxStackLayers;
            if (limit && level[b] + above[b] > *limit) violations += level[b] + above[b] - *limit;
        }
    }
    packing.stackingViolations = violations;
    return violations;
}

std::int64_t balanceExcess(const ContainerLoadingProblem& problem, int container,
                           const Packing& packing) {
    if (packing.balanceExcess >= 0) return packing.balanceExcess;
    packing.balanceExcess = 0;
    const Container& space = problem.containers[static_cast<std::size_t>(container)];
    if (!space.maxOffsetLength && !space.maxOffsetWidth) return 0;
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
    if (weight <= 0.0) return 0;
    std::int64_t excess = 0;
    // Per axis, the offset beyond the limit in percent of the half size,
    // rounded up; the tolerance keeps a rounding of the moments from counting.
    const auto beyond = [&excess](double centre, double half, const std::optional<double>& limit) {
        if (!limit) return;
        const double over = 100.0 * (std::fabs(centre - half) / half - *limit);
        if (over > 1e-6) excess += static_cast<std::int64_t>(std::ceil(over - 1e-6));
    };
    beyond(momentX / weight, 0.5 * static_cast<double>(space.length), space.maxOffsetLength);
    beyond(momentY / weight, 0.5 * static_cast<double>(space.width), space.maxOffsetWidth);
    packing.balanceExcess = excess;
    return excess;
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
        hash.add(container.fillHeight);  // what the decoder fills up to
        // An optional item may not take the placed weight above the limit.
        if (problem.anyOptional) hash.add(container.maxWeight ? *container.maxWeight : -1);
        // The balance limit's cost is kept with a decode too.
        if (container.maxOffsetLength || container.maxOffsetWidth) {
            hash.add(container.maxOffsetLength ? *container.maxOffsetLength : -1.0);
            hash.add(container.maxOffsetWidth ? *container.maxOffsetWidth : -1.0);
        }
    }
    hash.add(static_cast<std::int64_t>(problem.items.size()));
    for (const Item& item : problem.items) {
        hash.add(item.length);
        hash.add(item.width);
        hash.add(item.height);
        hash.add(static_cast<int>(item.rotations));
        hash.add(item.weight);
        hash.add(item.maxLoadOnTop ? *item.maxLoadOnTop : std::int64_t{-1});
        // Whether it has an unloading order, whether it is bottomOnly and its
        // layer limit (0 = none) in one word: a reset hashes every item.
        hash.add(static_cast<std::int64_t>(item.unloadOrder ? 1 : 0) |
                 static_cast<std::int64_t>(item.bottomOnly ? 2 : 0) |
                 (static_cast<std::int64_t>(item.optional ? 4 : 0)) |
                 (static_cast<std::int64_t>(item.maxStackLayers ? *item.maxStackLayers : 0) << 3) |
                 (static_cast<std::int64_t>(item.priority) << 34));
        hash.add(item.unloadOrder ? *item.unloadOrder : 0);
    }
    if (problem.anyStackRule) {
        for (int group : problem.stackGroupOf) hash.add(group);
        for (char rule : problem.forbidden) hash.add(static_cast<int>(rule));
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
