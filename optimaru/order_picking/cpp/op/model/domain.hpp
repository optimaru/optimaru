#ifndef OP_DOMAIN_HPP
#define OP_DOMAIN_HPP

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"

namespace op {

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// Two levels: hard = missing trolley buckets, soft = travel distance +
// 1000 * order-split count. The engine's optimaru::Score(int64 hard,
// double soft) holds both exactly (all penalties are integral
// counts/meters).
using Score = optimaru::Score;

// ---------------------------------------------------------------------------
// Warehouse  (the layout: fixed input)
// ---------------------------------------------------------------------------
// A warehouse is one of two kinds of layout.
//
// A GRID of shelvings (the default): columnCount columns (A, B, ...) x
// shelvingRowCount rows (1, 2, ...). Every shelving is shelvingWidth wide and
// shelvingHeight tall, with shelvingPadding meters of corridor between
// neighbors. Products sit on the LEFT or RIGHT side of a shelving, at a row
// (0..shelvingHeight) measured from the shelving's top edge:
//
//  ------------------------------------------------------------------> x
//  |    |--------|   |--------|   |--------|   |--------|   |--------|
//  |    | (A,1)  |   | (B,1)  |   | (C,1)  |   | (D,1)  |   | (E,1)  |
//  |    |--------|   |--------|   |--------|   |--------|   |--------|
//  |    |--------|   |--------|   |--------|   |--------|   |--------|
//  |    | (A,2)  |   | (B,2)  |   | (C,2)  |   | (D,2)  |   | (E,2)  |
//  |    |--------|   |--------|   |--------|   |--------|   |--------|
//  |    ...
//  y
//
// A shelving is the (column, shelvingRow) integer pair; its string id
// "(A,1)" is derived for display/serialization. The default-constructed
// Warehouse is the grid every solution had before the layout became input:
// 5 columns (A..E) x 3 rows (1..3), shelvings 2 m x 10 m, 3 m corridors.
//
// NAMED LOCATIONS with a walking-distance table (`locations` set): the
// warehouse is a list of location ids and the meters from each one to each
// other one, whatever its shape. A location is then an index into that list
// (WarehouseLocation::node) and the grid numbers are not read.

// Available shelving sides where the products can be located.
enum class Side { LEFT = 0, RIGHT = 1 };

// Whole meters from every location to every other one, row-major. `ids` names
// the locations of a named layout; the table the solver builds over a grid's
// locations for the time of a solve (Warehouse::active) leaves it empty.
struct DistanceTable {
    int size = 0;
    std::vector<int> meters;       // size * size; meters[from * size + to]
    std::vector<std::string> ids;  // named layout: one per location

    int at(int from, int to) const { return meters[static_cast<std::size_t>(from) * size + to]; }
    bool holds(int node) const { return node >= 0 && node < size; }
};

struct Warehouse {
    // The default grid, and the largest one (three column letters; the sizes
    // keep every distance, and a walk's sum, far inside the integer types).
    static constexpr int DEFAULT_COLUMN_COUNT = 5;        // columns A..E
    static constexpr int DEFAULT_SHELVING_ROW_COUNT = 3;  // rows 1..3
    static constexpr int DEFAULT_SHELVING_WIDTH = 2;
    static constexpr int DEFAULT_SHELVING_HEIGHT = 10;
    static constexpr int DEFAULT_SHELVING_PADDING = 3;
    static constexpr int MAX_COLUMN_COUNT = 18278;  // A..ZZZ
    static constexpr int MAX_SHELVING_ROW_COUNT = 10000;
    static constexpr int MAX_SHELVING_SIZE = 1000;  // width, height and padding, meters
    // The longest distance a table of named locations may hold, meters.
    static constexpr int MAX_DISTANCE = 1000000;

    int columnCount = DEFAULT_COLUMN_COUNT;
    int shelvingRowCount = DEFAULT_SHELVING_ROW_COUNT;

    // Shelving geometry in meters. A shelving side has the product rows
    // 0..shelvingHeight.
    int shelvingWidth = DEFAULT_SHELVING_WIDTH;
    int shelvingHeight = DEFAULT_SHELVING_HEIGHT;
    int shelvingPadding = DEFAULT_SHELVING_PADDING;

    // Set for a layout of named locations (see makeDistanceTable in
    // op/model/layout.hpp); null for a grid. Shared, so that copying a
    // solution does not copy the table.
    std::shared_ptr<const DistanceTable> locations;

    // The table the distances are read from while a solve runs, set and
    // removed by Solver::solve (activateDistances / deactivateDistances in
    // op/model/layout.hpp): `locations` itself for a named layout, a table
    // over the grid locations the solution uses otherwise. While it is set
    // every location of the solution carries a valid `node`, so
    // calculateDistance reads it unchecked. Null outside a solve.
    std::shared_ptr<const DistanceTable> active;

    bool isGrid() const { return locations == nullptr; }

    // Absolute position of a shelving's top-left corner.
    int shelvingX(int column) const { return column * (shelvingWidth + shelvingPadding); }
    int shelvingY(int shelvingRow) const {
        return shelvingRow * (shelvingHeight + shelvingPadding);
    }
};

// The letters of a grid column as spreadsheets write them: 0 == "A",
// 25 == "Z", 26 == "AA", ...
inline std::string columnLetters(int column) {
    std::string letters;
    for (int rest = column; rest >= 0; rest = rest / 26 - 1)
        letters.insert(letters.begin(), static_cast<char>('A' + rest % 26));
    return letters;
}

// ---------------------------------------------------------------------------
// WarehouseLocation  (fixed input)
// ---------------------------------------------------------------------------
// A spot in the warehouse where a product is stored (or a trolley parks). On
// a grid: which shelving, which side of it, and the row within that side. In
// a layout of named locations: `node`, the location's index in
// Warehouse::locations, and nothing else.
struct WarehouseLocation {
    int column = 0;       // 0 == column A
    int shelvingRow = 0;  // 0 == row 1
    Side side = Side::LEFT;
    int row = 0;  // row within the shelving side, 0..shelvingHeight

    // Named layout: the index of the location. Grid: -1; the solver numbers
    // the locations in use here while it runs (Warehouse::active) and puts -1
    // back, so nothing outside a solve reads it.
    int node = -1;

    // A named location, by its index in Warehouse::locations.
    static WarehouseLocation named(int index) {
        WarehouseLocation location;
        location.node = index;
        return location;
    }

    // The shelving id of a grid location, e.g. "(A,1)".
    std::string shelvingId() const {
        return "(" + columnLetters(column) + "," + std::to_string(shelvingRow + 1) + ")";
    }
};

// What a location is called: the id of a named location, the shelving id of
// a grid one.
inline std::string locationLabel(const Warehouse& warehouse, const WarehouseLocation& location) {
    if (warehouse.isGrid()) return location.shelvingId();
    return warehouse.locations->holds(location.node) ? warehouse.locations->ids[location.node]
                                                     : std::string("?");
}

// Distance in meters between two locations of a grid, considering the
// warehouse structure.
inline int gridDistance(const Warehouse& warehouse, const WarehouseLocation& start,
                        const WarehouseLocation& end) {
    const int width = warehouse.shelvingWidth;
    const int height = warehouse.shelvingHeight;
    const int padding = warehouse.shelvingPadding;
    const auto distance = [](int a, int b) { return a < b ? b - a : a - b; };
    // Shortest vertical walk when switching shelving sides: leave through the
    // top (north) or the bottom (south) corridor.
    const auto bestYDistanceInShelvingRow = [height](int startY, int endY) {
        const int northDirectionDistance = startY + endY;
        const int southDirectionDistance = (height - startY) + (height - endY);
        return northDirectionDistance < southDirectionDistance ? northDirectionDistance
                                                               : southDirectionDistance;
    };

    const int startX = warehouse.shelvingX(start.column) + (start.side == Side::LEFT ? 0 : width);
    const int startY = warehouse.shelvingY(start.shelvingRow) + start.row;
    const int endX = warehouse.shelvingX(end.column) + (end.side == Side::LEFT ? 0 : width);
    const int endY = warehouse.shelvingY(end.shelvingRow) + end.row;

    int deltaX = 0;
    int deltaY = 0;
    if (start.column == end.column && start.shelvingRow == end.shelvingRow) {
        // same shelving
        if (start.side == end.side) {
            deltaY = distance(startY, endY);
        } else {
            // different side, calculate shortest walk.
            deltaX = width;
            deltaY = bestYDistanceInShelvingRow(start.row, end.row);
        }
    } else if (start.shelvingRow == end.shelvingRow) {
        // distinct shelvings but on the same warehouse row
        if (distance(startX, endX) == padding) {
            // neighbor shelvings, but also contiguous side
            deltaX = padding;
            deltaY = distance(startY, endY);
        } else {
            // any other combination of shelvings but in the same warehouse row
            deltaX = distance(startX, endX);
            deltaY = bestYDistanceInShelvingRow(start.row, end.row);
        }
    } else {
        // shelvings on different warehouse rows
        deltaX = distance(startX, endX);
        deltaY = distance(startY, endY);
    }
    return deltaX + deltaY;
}

// Distance in meters between two locations of `warehouse`: read from the
// solve's table while one is active (the hot path of every move), from the
// table of a named layout otherwise (checked: std::out_of_range for a
// location that is not one of its named ones), computed on a grid.
inline int calculateDistance(const Warehouse& warehouse, const WarehouseLocation& start,
                             const WarehouseLocation& end) {
    if (const DistanceTable* table = warehouse.active.get()) return table->at(start.node, end.node);
    if (const DistanceTable* table = warehouse.locations.get()) {
        if (!table->holds(start.node) || !table->holds(end.node))
            throw std::out_of_range("a location is not one of the warehouse's named locations");
        return table->at(start.node, end.node);
    }
    return gridDistance(warehouse, start, end);
}

// The same on the default grid (5 columns x 3 rows, see Warehouse).
inline int calculateDistance(const WarehouseLocation& start, const WarehouseLocation& end) {
    return gridDistance(Warehouse{}, start, end);
}

// ---------------------------------------------------------------------------
// Order  (fixed input)
// ---------------------------------------------------------------------------
// The customer order. Its items are flattened into the PickTasks that
// reference it by index (the constraints only ever need the order identity,
// the item volume and the product location, which live denormalized on
// PickTask below).
struct Order {
    std::string id;
};

// ---------------------------------------------------------------------------
// PickTask  (the list element)
// ---------------------------------------------------------------------------
// The picking of one order item by a trolley. The Solver assigns each
// PickTask to a position in a Trolley's pick list:
//
//   Trolley1: [PickTaskA, PickTaskB, PickTaskC]
//   Trolley2: [PickTaskD, PickTaskE]
//
// The derived fields — trolleyIndex (the trolley whose list holds the task),
// previousPickTask and nextPickTask (its neighbours in that list) — are
// indices, -1 meaning none. They are derived from the trolleys' pick lists by
// ScoreCalculator::updateShadowVariables (score refreshes them, and
// Solver::solve leaves them consistent with the final solution).
struct PickTask {
    std::string id;
    int orderIndex = -1;         // index into OrderPickingSolution::orders
    std::string productName;     // informational
    int volume = 0;              // the order item's (== product's) volume, cm3
    WarehouseLocation location;  // product location

    // --- derived fields (indices into trolleys / pickTasks; -1 == none) ---
    int trolleyIndex = -1;
    int previousPickTask = -1;
    int nextPickTask = -1;

    bool isLast() const { return nextPickTask == -1; }
};

// ---------------------------------------------------------------------------
// Trolley  (the list owner)
// ---------------------------------------------------------------------------
// The trolley that will be filled with the order items. pickTaskIndices is
// the decision: the ordered indices of the PickTasks the trolley
// picks along its path, which starts and ends at `location`.
struct Trolley {
    std::string id;
    int bucketCount = 0;
    int bucketCapacity = 0;
    WarehouseLocation location;
    std::vector<int> pickTaskIndices;  // ordered pick list
};

// ---------------------------------------------------------------------------
// OrderPickingSolution
// ---------------------------------------------------------------------------
struct OrderPickingSolution {
    Warehouse warehouse;  // the layout the locations below are in
    std::vector<Order> orders;
    std::vector<Trolley> trolleys;
    std::vector<PickTask> pickTasks;

    Score score;
};

// Distance travelled by a trolley along its current pick list, including the
// return to the path origin.
inline std::int64_t calculateDistanceToTravel(const OrderPickingSolution& solution,
                                              const Trolley& trolley) {
    const Warehouse& warehouse = solution.warehouse;
    std::int64_t distance = 0;
    const WarehouseLocation* previousLocation = &trolley.location;
    for (int pickIndex : trolley.pickTaskIndices) {
        const WarehouseLocation& location = solution.pickTasks[pickIndex].location;
        distance += calculateDistance(warehouse, *previousLocation, location);
        previousLocation = &location;
    }
    distance += calculateDistance(warehouse, *previousLocation, trolley.location);
    return distance;
}

}  // namespace op

// ---------------------------------------------------------------------------
// SolutionTraits specialization: the trolleys are the list owners, the
// pick tasks are the elements. Every pick task appears in at most one
// trolley's list; a task missing from all lists is unassigned (only during
// construction — the engine's moves keep every element assigned after the
// construction phase).
// ---------------------------------------------------------------------------
template <>
struct optimaru::SolutionTraits<op::OrderPickingSolution> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;
    static int list_count(const op::OrderPickingSolution& s) {
        return static_cast<int>(s.trolleys.size());
    }
    static int element_count(const op::OrderPickingSolution& s) {
        return static_cast<int>(s.pickTasks.size());
    }
    static int list_size(const op::OrderPickingSolution& s, int listIndex) {
        return static_cast<int>(s.trolleys[listIndex].pickTaskIndices.size());
    }
    static int element_at(const op::OrderPickingSolution& s, int listIndex, int position) {
        return s.trolleys[listIndex].pickTaskIndices[position];
    }
    static void insert_element(op::OrderPickingSolution& s, int listIndex, int position,
                               int element) {
        auto& list = s.trolleys[listIndex].pickTaskIndices;
        list.insert(list.begin() + position, element);
    }
    static int remove_element(op::OrderPickingSolution& s, int listIndex, int position) {
        auto& list = s.trolleys[listIndex].pickTaskIndices;
        const int element = list[position];
        list.erase(list.begin() + position);
        return element;
    }
    static void reverse_segment(op::OrderPickingSolution& s, int listIndex, int first, int last) {
        auto& list = s.trolleys[listIndex].pickTaskIndices;
        std::reverse(list.begin() + first, list.begin() + last + 1);
    }
};

#endif  // OP_DOMAIN_HPP
