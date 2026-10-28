#ifndef OP_DOMAIN_HPP
#define OP_DOMAIN_HPP

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace op
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The Java quickstart uses plain HardSoftScore (int levels): hard = missing
// trolley buckets, soft = travel distance + 1000 * order-split count. The
// engine's optimaru::HardSoftScore(int64 hard, double soft) holds both levels
// exactly (all penalties are integral counts/meters), so the mapping is 1:1 —
// no level folding needed.
using Score = optimaru::HardSoftScore;

// ---------------------------------------------------------------------------
// Warehouse  (static layout, port of org.acme.orderpicking.domain.Warehouse)
// ---------------------------------------------------------------------------
// The warehouse is a fixed grid of shelvings: 5 columns (A..E) x 3 rows
// (1..3). Every shelving is SHELVING_WIDTH wide and SHELVING_HEIGHT tall,
// with SHELVING_PADDING meters of corridor between neighbors. Products sit
// on the LEFT or RIGHT side of a shelving, at a row (0..SHELVING_HEIGHT)
// measured from the shelving's top edge:
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
// Java identifies a shelving by the string id "(A,1)"; here a shelving is the
// (column, shelvingRow) integer pair and the id string is derived for
// display/serialization.

// Available shelving sides where the products can be located.
enum class Side
{
    LEFT = 0,
    RIGHT = 1
};

struct Warehouse
{
    static constexpr int COLUMN_COUNT = 5;  // columns A..E
    static constexpr int ROW_COUNT = 3;     // rows 1..3

    // Shelving geometry in meters (Warehouse.SHELVING_* in Java).
    static constexpr int SHELVING_WIDTH = 2;
    static constexpr int SHELVING_HEIGHT = 10;
    static constexpr int SHELVING_PADDING = 3;

    // Product rows available on each shelving side (Shelving.ROWS_SIZE).
    static constexpr int SHELVING_ROWS_SIZE = 10;

    // Absolute position of a shelving's top-left corner (the Java static
    // SHELVING_MAP is this closed-form grid).
    static constexpr int shelvingX(int column)
    {
        return column * (SHELVING_WIDTH + SHELVING_PADDING);
    }
    static constexpr int shelvingY(int shelvingRow)
    {
        return shelvingRow * (SHELVING_HEIGHT + SHELVING_PADDING);
    }
};

// ---------------------------------------------------------------------------
// WarehouseLocation  (problem fact)
// ---------------------------------------------------------------------------
// A spot in the warehouse where a product is stored (or a trolley parks):
// which shelving, which side of it, and the row within that side.
struct WarehouseLocation
{
    int column = 0;       // 0..4 == Warehouse.Column A..E
    int shelvingRow = 0;  // 0..2 == Warehouse.Row 1..3
    Side side = Side::LEFT;
    int row = 0;  // row within the shelving side, 0..SHELVING_HEIGHT

    // The Java shelving id, e.g. "(A,1)".
    std::string shelvingId() const
    {
        return std::string("(") + static_cast<char>('A' + column) + ","
               + std::to_string(shelvingRow + 1) + ")";
    }
};

// Distance in meters between two locations considering the warehouse
// structure — a literal port of Warehouse.calculateDistance.
inline int calculateDistance(const WarehouseLocation &start,
                             const WarehouseLocation &end)
{
    // Shortest vertical walk when switching shelving sides: leave through the
    // top (north) or the bottom (south) corridor.
    const auto bestYDistanceInShelvingRow = [](int startY, int endY)
    {
        const int northDirectionDistance = startY + endY;
        const int southDirectionDistance
            = (Warehouse::SHELVING_HEIGHT - startY)
              + (Warehouse::SHELVING_HEIGHT - endY);
        return northDirectionDistance < southDirectionDistance
                   ? northDirectionDistance
                   : southDirectionDistance;
    };

    const int startX
        = Warehouse::shelvingX(start.column)
          + (start.side == Side::LEFT ? 0 : Warehouse::SHELVING_WIDTH);
    const int startY = Warehouse::shelvingY(start.shelvingRow) + start.row;
    const int endX = Warehouse::shelvingX(end.column)
                     + (end.side == Side::LEFT ? 0 : Warehouse::SHELVING_WIDTH);
    const int endY = Warehouse::shelvingY(end.shelvingRow) + end.row;

    int deltaX = 0;
    int deltaY = 0;
    if (start.column == end.column && start.shelvingRow == end.shelvingRow)
    {
        // same shelving
        if (start.side == end.side)
        {
            deltaY = std::abs(startY - endY);
        }
        else
        {
            // different side, calculate shortest walk.
            deltaX = Warehouse::SHELVING_WIDTH;
            deltaY = bestYDistanceInShelvingRow(start.row, end.row);
        }
    }
    else if (start.shelvingRow == end.shelvingRow)
    {
        // distinct shelvings but on the same warehouse row
        if (std::abs(startX - endX) == Warehouse::SHELVING_PADDING)
        {
            // neighbor shelvings, but also contiguous side
            deltaX = Warehouse::SHELVING_PADDING;
            deltaY = std::abs(startY - endY);
        }
        else
        {
            // any other combination of shelvings but in the same warehouse row
            deltaX = std::abs(startX - endX);
            deltaY = bestYDistanceInShelvingRow(start.row, end.row);
        }
    }
    else
    {
        // shelvings on different warehouse rows
        deltaX = std::abs(startX - endX);
        deltaY = std::abs(startY - endY);
    }
    return deltaX + deltaY;
}

// ---------------------------------------------------------------------------
// Order  (problem fact)
// ---------------------------------------------------------------------------
// The customer order. Its items are flattened into the PickTasks that
// reference it by index (Java keeps Order -> OrderItem -> Product object
// links; the constraints only ever need the order identity, the item volume
// and the product location, which live denormalized on PickTask below).
struct Order
{
    std::string id;
};

// ---------------------------------------------------------------------------
// PickTask  (@PlanningEntity — the LIST element)
// ---------------------------------------------------------------------------
// The picking of one order item by a trolley. The Solver assigns each
// PickTask to a position in a Trolley's pick list:
//
//   Trolley1: [PickTaskA, PickTaskB, PickTaskC]
//   Trolley2: [PickTaskD, PickTaskE]
//
// The shadow fields mirror the Java shadow variables — trolley
// (@InverseRelationShadowVariable), previousPickTask
// (@PreviousElementShadowVariable) and nextPickTask
// (@NextElementShadowVariable) — as indices, -1 standing in for null. They
// are derived from the trolleys' pick lists by
// ScoreCalculator::updateShadowVariables (calculateScore refreshes them, and
// Solver::solve leaves them consistent with the final solution).
struct PickTask
{
    std::string id;
    int orderIndex = -1;         // index into OrderPickingSolution::orders
    std::string productName;     // informational (Java product.getName())
    int volume = 0;              // orderItem.getVolume() == product volume, cm3
    WarehouseLocation location;  // product location (PickTask.getLocation())

    // --- shadow variables (indices into trolleys / pickTasks; -1 == null) ---
    int trolleyIndex = -1;
    int previousPickTask = -1;
    int nextPickTask = -1;

    bool isLast() const { return nextPickTask == -1; }
};

// ---------------------------------------------------------------------------
// Trolley  (@PlanningEntity — the LIST owner)
// ---------------------------------------------------------------------------
// The trolley that will be filled with the order items. pickTaskIndices is
// the @PlanningListVariable: the ordered indices of the PickTasks the trolley
// picks along its path, which starts and ends at `location`.
struct Trolley
{
    std::string id;
    int bucketCount = 0;
    int bucketCapacity = 0;
    WarehouseLocation location;
    std::vector<int> pickTaskIndices;  // ordered pick list
};

// ---------------------------------------------------------------------------
// OrderPickingSolution  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct OrderPickingSolution
{
    std::vector<Order> orders;
    std::vector<Trolley> trolleys;
    std::vector<PickTask> pickTasks;

    Score score;
};

// Distance travelled by a trolley along its current pick list, including the
// return to the path origin (Warehouse.calculateDistanceToTravel).
inline int calculateDistanceToTravel(const OrderPickingSolution &solution,
                                     const Trolley &trolley)
{
    int distance = 0;
    const WarehouseLocation *previousLocation = &trolley.location;
    for (int pickIndex : trolley.pickTaskIndices)
    {
        const WarehouseLocation &location
            = solution.pickTasks[pickIndex].location;
        distance += calculateDistance(*previousLocation, location);
        previousLocation = &location;
    }
    distance += calculateDistance(*previousLocation, trolley.location);
    return distance;
}

}  // namespace op

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the C++ analog of the @PlanningEntity /
// @PlanningListVariable annotations): the trolleys are the list owners, the
// pick tasks are the elements. Every pick task appears in at most one
// trolley's list; a task missing from all lists is unassigned (only during
// construction — the quickstart does not allow unassigned values, and the
// engine's moves keep every element assigned after the construction phase).
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<op::OrderPickingSolution>
{
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::LIST;
    static int listCount(const op::OrderPickingSolution &s)
    {
        return static_cast<int>(s.trolleys.size());
    }
    static int elementCount(const op::OrderPickingSolution &s)
    {
        return static_cast<int>(s.pickTasks.size());
    }
    static int listSize(const op::OrderPickingSolution &s, int listIndex)
    {
        return static_cast<int>(s.trolleys[listIndex].pickTaskIndices.size());
    }
    static int
    getElement(const op::OrderPickingSolution &s, int listIndex, int position)
    {
        return s.trolleys[listIndex].pickTaskIndices[position];
    }
    static void insertElement(op::OrderPickingSolution &s,
                              int listIndex,
                              int position,
                              int element)
    {
        auto &list = s.trolleys[listIndex].pickTaskIndices;
        list.insert(list.begin() + position, element);
    }
    static int
    removeElement(op::OrderPickingSolution &s, int listIndex, int position)
    {
        auto &list = s.trolleys[listIndex].pickTaskIndices;
        const int element = list[position];
        list.erase(list.begin() + position);
        return element;
    }
    static void reverseSegment(op::OrderPickingSolution &s,
                               int listIndex,
                               int first,
                               int last)
    {
        auto &list = s.trolleys[listIndex].pickTaskIndices;
        std::reverse(list.begin() + first, list.begin() + last + 1);
    }
};

#endif  // OP_DOMAIN_HPP
