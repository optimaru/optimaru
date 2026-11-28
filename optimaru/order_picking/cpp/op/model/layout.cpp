#include "op/model/layout.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace op {

namespace {

void checkRange(const char* name, int value, int low, int high) {
    if (value < low || value > high) {
        throw std::invalid_argument(std::string("warehouse: ") + name + " must be " +
                                    std::to_string(low) + ".." + std::to_string(high) + ", not " +
                                    std::to_string(value));
    }
}

void checkGrid(const Warehouse& warehouse) {
    checkRange("column_count", warehouse.columnCount, 1, Warehouse::MAX_COLUMN_COUNT);
    checkRange("shelving_row_count", warehouse.shelvingRowCount, 1,
               Warehouse::MAX_SHELVING_ROW_COUNT);
    checkRange("shelving_width", warehouse.shelvingWidth, 1, Warehouse::MAX_SHELVING_SIZE);
    checkRange("shelving_height", warehouse.shelvingHeight, 1, Warehouse::MAX_SHELVING_SIZE);
    checkRange("shelving_padding", warehouse.shelvingPadding, 1, Warehouse::MAX_SHELVING_SIZE);
}

// One number per grid location (the ranges checkLayout allows keep it in 64 bits).
std::int64_t gridKey(const Warehouse& warehouse, const WarehouseLocation& location) {
    std::int64_t key = location.column;
    key = key * warehouse.shelvingRowCount + location.shelvingRow;
    key = key * 2 + (location.side == Side::LEFT ? 0 : 1);
    return key * (warehouse.shelvingHeight + 1) + location.row;
}

template <typename Visit>
void forEachLocation(OrderPickingSolution& solution, Visit visit) {
    for (Trolley& trolley : solution.trolleys) visit(trolley.location);
    for (PickTask& task : solution.pickTasks) visit(task.location);
}

}  // namespace

void checkLocation(const Warehouse& warehouse, const WarehouseLocation& location,
                   const std::string& owner) {
    if (!warehouse.isGrid()) {
        if (!warehouse.locations->holds(location.node)) {
            throw std::invalid_argument(owner + ": its location is not one of the warehouse's " +
                                        std::to_string(warehouse.locations->size) +
                                        " named locations (a warehouse of named locations takes "
                                        "Warehouse.location(id))");
        }
        return;
    }
    // While a solve's table is set, a grid location carries its node in it.
    if (location.node >= 0 && warehouse.active == nullptr) {
        throw std::invalid_argument(owner +
                                    ": a named location in a grid warehouse (one solution uses "
                                    "one kind of layout)");
    }
    if (location.column < 0 || location.column >= warehouse.columnCount) {
        throw std::invalid_argument(owner + ": column " + std::to_string(location.column) +
                                    " is outside the warehouse's columns 0.." +
                                    std::to_string(warehouse.columnCount - 1) + " (A.." +
                                    columnLetters(warehouse.columnCount - 1) + ")");
    }
    if (location.shelvingRow < 0 || location.shelvingRow >= warehouse.shelvingRowCount) {
        throw std::invalid_argument(owner + ": shelving_row " +
                                    std::to_string(location.shelvingRow) +
                                    " is outside the warehouse's shelving rows 0.." +
                                    std::to_string(warehouse.shelvingRowCount - 1) + " (1.." +
                                    std::to_string(warehouse.shelvingRowCount) + ")");
    }
    if (location.row < 0 || location.row > warehouse.shelvingHeight) {
        throw std::invalid_argument(owner + ": row " + std::to_string(location.row) +
                                    " is outside a shelving side's rows 0.." +
                                    std::to_string(warehouse.shelvingHeight));
    }
}

Warehouse makeGridWarehouse(int columnCount, int shelvingRowCount, int shelvingWidth,
                            int shelvingHeight, int shelvingPadding) {
    Warehouse warehouse;
    warehouse.columnCount = columnCount;
    warehouse.shelvingRowCount = shelvingRowCount;
    warehouse.shelvingWidth = shelvingWidth;
    warehouse.shelvingHeight = shelvingHeight;
    warehouse.shelvingPadding = shelvingPadding;
    checkGrid(warehouse);
    return warehouse;
}

Warehouse makeNamedWarehouse(std::vector<std::string> ids,
                             const std::vector<std::vector<int>>& meters) {
    if (ids.empty()) throw std::invalid_argument("warehouse: no locations");
    const std::size_t size = ids.size();
    if (size > static_cast<std::size_t>(Warehouse::MAX_LOCATIONS)) {
        throw std::invalid_argument("warehouse: at most " +
                                    std::to_string(Warehouse::MAX_LOCATIONS) +
                                    " named locations, not " + std::to_string(size));
    }
    auto table = std::make_shared<DistanceTable>();
    for (std::size_t index = 0; index < size; ++index) {
        if (!table->indexOf.emplace(ids[index], static_cast<int>(index)).second)
            throw std::invalid_argument("warehouse: duplicate location id '" + ids[index] + "'");
    }
    if (meters.size() != size) {
        throw std::invalid_argument("warehouse: " + std::to_string(size) + " locations need " +
                                    std::to_string(size) + " rows of distances, not " +
                                    std::to_string(meters.size()));
    }
    table->size = static_cast<int>(size);
    table->meters.reserve(size * size);
    for (std::size_t from = 0; from < size; ++from) {
        if (meters[from].size() != size) {
            throw std::invalid_argument("warehouse: the distances from '" + ids[from] +
                                        "' must have one value per location (" +
                                        std::to_string(size) + "), not " +
                                        std::to_string(meters[from].size()));
        }
        for (std::size_t to = 0; to < size; ++to) {
            const int distance = meters[from][to];
            if (distance < 0 || distance > Warehouse::MAX_DISTANCE) {
                throw std::invalid_argument("warehouse: the distance from '" + ids[from] +
                                            "' to '" + ids[to] + "' must be 0.." +
                                            std::to_string(Warehouse::MAX_DISTANCE) + ", not " +
                                            std::to_string(distance));
            }
            if (from == to && distance != 0) {
                throw std::invalid_argument("warehouse: the distance from '" + ids[from] +
                                            "' to itself must be 0");
            }
            table->meters.push_back(distance);
        }
    }
    table->ids = std::move(ids);
    Warehouse warehouse;
    warehouse.locations = std::move(table);
    return warehouse;
}

void checkLayout(const OrderPickingSolution& solution) {
    const Warehouse& warehouse = solution.warehouse;
    if (warehouse.isGrid()) checkGrid(warehouse);
    for (const Trolley& trolley : solution.trolleys)
        checkLocation(warehouse, trolley.location, "trolley '" + trolley.id + "'");
    for (const PickTask& task : solution.pickTasks)
        checkLocation(warehouse, task.location, "pick task '" + task.id + "'");
}

ActiveDistances::ActiveDistances(OrderPickingSolution& solution) : solution_(solution) {
    Warehouse& warehouse = solution.warehouse;
    if (warehouse.active != nullptr) {
        owner_ = false;
        return;
    }
    if (!warehouse.isGrid()) {
        warehouse.active = warehouse.locations;
        return;
    }
    // Number the distinct grid locations in use, in order of appearance, and
    // build the table over them. The solution is written only once the table
    // stands, so an allocation that fails leaves it as it was (the destructor
    // of an object that was never made does not run).
    std::unordered_map<std::int64_t, int> nodeOf;
    std::vector<WarehouseLocation> places;
    forEachLocation(solution, [&](WarehouseLocation& location) {
        if (nodeOf.emplace(gridKey(warehouse, location), static_cast<int>(places.size())).second)
            places.push_back(location);
    });
    const std::size_t size = places.size();
    // No table above the limit. calculateDistance reads the table unchecked,
    // by the nodes, whenever `active` is set: it is set only together with a
    // valid node on every location, below, and here neither is.
    if (size > static_cast<std::size_t>(kMaxGridNodes)) return;
    auto table = std::make_shared<DistanceTable>();
    table->size = static_cast<int>(size);
    table->meters.resize(size * size);
    for (std::size_t from = 0; from < size; ++from) {
        for (std::size_t to = 0; to < size; ++to)
            table->meters[from * size + to] = gridDistance(warehouse, places[from], places[to]);
    }
    forEachLocation(solution, [&](WarehouseLocation& location) {
        location.node = nodeOf.find(gridKey(warehouse, location))->second;
    });
    warehouse.active = std::move(table);
}

ActiveDistances::~ActiveDistances() {
    if (!owner_) return;
    Warehouse& warehouse = solution_.warehouse;
    warehouse.active.reset();
    if (warehouse.isGrid())
        forEachLocation(solution_, [](WarehouseLocation& location) { location.node = -1; });
}

}  // namespace op
