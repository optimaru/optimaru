#ifndef ALB_CONSTRAINTS_HPP
#define ALB_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (StationCapacityCost -> constraints/station_capacity_cost.hpp); common.hpp
// holds what all of them need (the calculator aliases, the "prepare() has
// run" guard, the PerTaskConstraint / MemberChangeGlobalConstraint bases) and
// carries the pattern's documentation.
//
// Adding a component is a file plus a line here (and a line in
// forEachDefaultConstraint below when it belongs to the default assembly).

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "alb/constraints/common.hpp"
#include "alb/constraints/different_station_pairs_cost.hpp"
#include "alb/constraints/fixed_task_stations_cost.hpp"
#include "alb/constraints/forbidden_task_stations_cost.hpp"
#include "alb/constraints/max_stations_cost.hpp"
#include "alb/constraints/same_station_groups_cost.hpp"
#include "alb/constraints/station_capacity_cost.hpp"
#include "alb/constraints/task_precedence_cost.hpp"
#include "alb/constraints/task_station_bounds_cost.hpp"
#include "alb/constraints/used_stations_cost.hpp"
#include "alb/constraints/workload_imbalance_cost.hpp"

namespace alb {

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// Every component at its own defaults, in list_constraints() order:
//
//   stationCapacityCost         hard, weight 1   (per station)
//   taskPrecedenceCost          hard, weight 1   (global)
//   maxStationsCost             hard, weight 1   (per station)
//   taskStationBoundsCost       hard, weight 1   (per station)
//   fixedTaskStationsCost       hard, weight 1   (per station)
//   forbiddenTaskStationsCost   hard, weight 1   (per station)
//   sameStationGroupsCost       hard, weight 1   (global)
//   differentStationPairsCost   hard, weight 1   (per station)
//   usedStationsCost            soft, weight 1   (global)
//   workloadImbalanceCost       soft, weight 0   (global; opt-in objective)
//
// This assembly scores bit-for-bit like ScoreCalculator::score and
// the monolithic incremental calculator (all read the same kDefaultWeight
// constants). It is also the domain's constraint catalog: constraint_info()
// reads the defaults off it, so they live in the component constructors and
// nowhere else. It needs no problem to be built.
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("workloadImbalanceCost", 1)
//         .soft("maxStationsCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
//
// Catalog order: an assembly keeps its unit and global components apart
// (names() / info() list the units first), but this domain's catalog order —
// list_constraints(), constraint_info(), the CP-SAT registry — interleaves
// them (the reference's catalog order). The order is stated ONCE, here:
// defaultConstraints() and catalogNames() are both built from it.
template <typename Visit>
void forEachDefaultConstraint(Visit&& visit) {
    visit(std::make_unique<StationCapacityCost>());
    visit(std::make_unique<TaskPrecedenceCost>());
    visit(std::make_unique<MaxStationsCost>());
    visit(std::make_unique<TaskStationBoundsCost>());
    visit(std::make_unique<FixedTaskStationsCost>());
    visit(std::make_unique<ForbiddenTaskStationsCost>());
    visit(std::make_unique<SameStationGroupsCost>());
    visit(std::make_unique<DifferentStationPairsCost>());
    visit(std::make_unique<UsedStationsCost>());
    visit(std::make_unique<WorkloadImbalanceCost>());
}

inline ConstraintAssembly defaultConstraints() {
    ConstraintAssembly assembly;
    forEachDefaultConstraint([&assembly](auto component) { assembly.add(std::move(component)); });
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator& calculator) {
    defaultConstraints().into(calculator);
}

// The registry names of the default set, in catalog order.
inline std::vector<std::string> catalogNames() {
    std::vector<std::string> names;
    forEachDefaultConstraint([&names](auto component) { names.push_back(component->name); });
    return names;
}

// defaultConstraints().info() rows, in catalog order: the constraint catalog
// constraint_info() reports.
inline std::vector<optimaru::ConstraintInfo> catalogRows() {
    const std::vector<optimaru::ConstraintInfo> rows = defaultConstraints().info();
    std::vector<optimaru::ConstraintInfo> ordered;
    ordered.reserve(rows.size());
    for (const std::string& name : catalogNames()) {
        for (const optimaru::ConstraintInfo& row : rows) {
            if (row.name == name) ordered.push_back(row);
        }
    }
    return ordered;
}

}  // namespace alb

#endif  // ALB_CONSTRAINTS_HPP
