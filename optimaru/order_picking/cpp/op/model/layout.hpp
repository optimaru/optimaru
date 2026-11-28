#ifndef OP_LAYOUT_HPP
#define OP_LAYOUT_HPP

#include <memory>
#include <string>
#include <vector>

#include "op/model/domain.hpp"

namespace op {

// A grid of the given size. Throws std::invalid_argument when a number is
// outside its range: at least one column and one shelving row, at most
// Warehouse::MAX_COLUMN_COUNT / MAX_SHELVING_ROW_COUNT; width, height and
// padding 1..Warehouse::MAX_SHELVING_SIZE meters.
Warehouse makeGridWarehouse(int columnCount = Warehouse::DEFAULT_COLUMN_COUNT,
                            int shelvingRowCount = Warehouse::DEFAULT_SHELVING_ROW_COUNT,
                            int shelvingWidth = Warehouse::DEFAULT_SHELVING_WIDTH,
                            int shelvingHeight = Warehouse::DEFAULT_SHELVING_HEIGHT,
                            int shelvingPadding = Warehouse::DEFAULT_SHELVING_PADDING);

// A layout of named locations: `ids` (unique, at least one) and the whole
// meters from each to each, meters[from][to], one row per id. The table does
// not have to be symmetric. Throws std::invalid_argument when the table is
// not square over the ids, an id repeats, a distance is negative or above
// Warehouse::MAX_DISTANCE, or a location is not 0 meters from itself.
Warehouse makeNamedWarehouse(std::vector<std::string> ids,
                             const std::vector<std::vector<int>>& meters);

// Whether every trolley and pick task is at a location of the solution's
// warehouse: inside the grid, or one of the named locations. Throws
// std::invalid_argument naming the first one that is not (and for a grid
// whose numbers are out of range). The Python constructor and Solver::solve
// call it.
void checkLayout(const OrderPickingSolution& solution);

// The distance table of a solve (Warehouse::active), set while an object of
// this class lives: the table of a named layout as it is; for a grid, the
// distances between the distinct locations the solution uses, each of which
// gets its node. A grid that uses more than kMaxGridNodes distinct locations
// gets no table and keeps computing. Copies of the solution made meanwhile
// (the engine's best solution, parallel starts) share the table.
class ActiveDistances {
public:
    static constexpr int kMaxGridNodes = 2048;  // 16 MB of distances

    explicit ActiveDistances(OrderPickingSolution& solution);
    ~ActiveDistances();
    ActiveDistances(const ActiveDistances&) = delete;
    ActiveDistances& operator=(const ActiveDistances&) = delete;

private:
    OrderPickingSolution& solution_;
};

}  // namespace op

#endif  // OP_LAYOUT_HPP
