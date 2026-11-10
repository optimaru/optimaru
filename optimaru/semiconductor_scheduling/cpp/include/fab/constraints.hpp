#ifndef FAB_CONSTRAINTS_HPP
#define FAB_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (MachineCapacityCost -> constraints/machine_capacity_cost.hpp); common.hpp
// holds what all of them need (the calculator aliases, the shared schedule
// view and its global base) and carries the pattern's documentation.
//
// Adding a component is a file, a Term, a line here (and an `emplace` in
// defaultConstraints() below when it belongs to the default assembly), and
// its unit formula in the evaluator and rawCostsOf (src/).

#include "fab/constraints/auxiliary_resource_capacity_cost.hpp"
#include "fab/constraints/auxiliary_resource_unavailability_cost.hpp"
#include "fab/constraints/batch_capacity_cost.hpp"
#include "fab/constraints/batch_family_conflict_cost.hpp"
#include "fab/constraints/common.hpp"
#include "fab/constraints/cycle_time_cost.hpp"
#include "fab/constraints/lot_route_precedence_cost.hpp"
#include "fab/constraints/machine_capacity_cost.hpp"
#include "fab/constraints/machine_eligibility_cost.hpp"
#include "fab/constraints/machine_load_imbalance_cost.hpp"
#include "fab/constraints/machine_preference_cost.hpp"
#include "fab/constraints/machine_unavailability_cost.hpp"
#include "fab/constraints/makespan_cost.hpp"
#include "fab/constraints/max_cycle_time_cost.hpp"
#include "fab/constraints/max_queue_time_cost.hpp"
#include "fab/constraints/min_batch_size_cost.hpp"
#include "fab/constraints/min_queue_time_cost.hpp"
#include "fab/constraints/missing_setup_time_cost.hpp"
#include "fab/constraints/missing_transport_time_cost.hpp"
#include "fab/constraints/pinned_assignments_cost.hpp"
#include "fab/constraints/weighted_tardiness_cost.hpp"

namespace fab
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// Every component at its own defaults, in list_constraints() order:
//
//   machineEligibilityCost               hard, 1
//   lotRoutePrecedenceCost               hard, 1
//   machineCapacityCost                  hard, 1
//   batchCapacityCost                    hard, 1
//   batchFamilyConflictCost              hard, 1
//   missingSetupTimeCost                 hard, 1
//   machineUnavailabilityCost            hard, 1
//   maxQueueTimeCost                     hard, 1
//   minQueueTimeCost                     hard, 1
//   missingTransportTimeCost             hard, 1
//   auxiliaryResourceCapacityCost        hard, 1
//   auxiliaryResourceUnavailabilityCost  hard, 1
//   pinnedAssignmentsCost                hard, 1
//   maxCycleTimeCost                     hard, 1
//   machinePreferenceCost                soft, 1
//   minBatchSizeCost                     soft, 1
//   machineLoadImbalanceCost             soft, 1
//   weightedTardinessCost                soft, 10
//   cycleTimeCost                        soft, 1
//   makespanCost                         soft, 1
//
// This assembly scores bit-for-bit like ScoreCalculator and the monolithic
// incremental calculator (both read the same kDefaultWeight constants). It is
// also the domain's constraint catalog: constraint_info() reads the defaults
// off it, so they live in the component constructors and nowhere else.
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("weightedTardinessCost", 4)
//         .soft("maxQueueTimeCost")
//         .drop("makespanCost")
//         .into(calc);
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<MachineEligibilityCost>();
    assembly.emplace<LotRoutePrecedenceCost>();
    assembly.emplace<MachineCapacityCost>();
    assembly.emplace<BatchCapacityCost>();
    assembly.emplace<BatchFamilyConflictCost>();
    assembly.emplace<MissingSetupTimeCost>();
    assembly.emplace<MachineUnavailabilityCost>();
    assembly.emplace<MaxQueueTimeCost>();
    assembly.emplace<MinQueueTimeCost>();
    assembly.emplace<MissingTransportTimeCost>();
    assembly.emplace<AuxiliaryResourceCapacityCost>();
    assembly.emplace<AuxiliaryResourceUnavailabilityCost>();
    assembly.emplace<PinnedAssignmentsCost>();
    assembly.emplace<MaxCycleTimeCost>();
    assembly.emplace<MachinePreferenceCost>();
    assembly.emplace<MinBatchSizeCost>();
    assembly.emplace<MachineLoadImbalanceCost>();
    assembly.emplace<WeightedTardinessCost>();
    assembly.emplace<CycleTimeCost>();
    assembly.emplace<MakespanCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace fab

#endif  // FAB_CONSTRAINTS_HPP
