#ifndef BA_CONSTRAINTS_HPP
#define BA_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/, named after its struct
// (BedConflictCost -> constraints/bed_conflict_cost.hpp); common.hpp holds
// what all of them need (the calculator aliases, the per-stay bases and the
// "prepare() has run" guard) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// defaultConstraints below when it belongs to the default assembly).

#include "ba/constraints/bed_conflict_cost.hpp"
#include "ba/constraints/common.hpp"
#include "ba/constraints/department_max_age_cost.hpp"
#include "ba/constraints/department_min_age_cost.hpp"
#include "ba/constraints/department_specialty_cost.hpp"
#include "ba/constraints/department_specialty_priority_cost.hpp"
#include "ba/constraints/female_in_male_room_cost.hpp"
#include "ba/constraints/male_in_female_room_cost.hpp"
#include "ba/constraints/mixed_gender_room_cost.hpp"
#include "ba/constraints/preferred_max_room_capacity_cost.hpp"
#include "ba/constraints/preferred_patient_equipment_cost.hpp"
#include "ba/constraints/required_patient_equipment_cost.hpp"
#include "ba/constraints/unassigned_stays_cost.hpp"

namespace ba
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider, every component at its own defaults (the quickstart's
// weights):
//
//   bedConflictCost                   hard,   weight 1000
//   femaleInMaleRoomCost              hard,   weight 50
//   maleInFemaleRoomCost              hard,   weight 50
//   mixedGenderRoomCost               hard,   weight 1000  (global)
//   departmentMinAgeCost              hard,   weight 100
//   departmentMaxAgeCost              hard,   weight 100
//   requiredPatientEquipmentCost      hard,   weight 50
//   unassignedStaysCost               medium, weight 1 (medium units)
//   preferredMaxRoomCapacityCost      soft,   weight 8
//   departmentSpecialtyCost           soft,   weight 10
//   departmentSpecialtyPriorityCost   soft,   weight 10
//   preferredPatientEquipmentCost     soft,   weight 50
//
// The assembly lists unit constraints first and globals after, so
// mixedGenderRoomCost comes LAST in names() / info() / list_constraints().
//
// This assembly scores bit-for-bit like ScoreCalculator::calculateScore and
// the monolithic incremental calculator (both read the same kDefaultWeight
// constants and fold medium with the same kMediumFold). It is also the
// domain's constraint catalog: constraint_info() reads the defaults off it,
// so they live in the component constructors and nowhere else.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("unassignedStaysCost", 2)      // 2 medium per night
//         .drop("departmentMaxAgeCost")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<BedConflictCost>();
    assembly.emplace<FemaleInMaleRoomCost>();
    assembly.emplace<MaleInFemaleRoomCost>();
    assembly.emplace<MixedGenderRoomCost>();
    assembly.emplace<DepartmentMinAgeCost>();
    assembly.emplace<DepartmentMaxAgeCost>();
    assembly.emplace<RequiredPatientEquipmentCost>();
    assembly.emplace<UnassignedStaysCost>();
    assembly.emplace<PreferredMaxRoomCapacityCost>();
    assembly.emplace<DepartmentSpecialtyCost>();
    assembly.emplace<DepartmentSpecialtyPriorityCost>();
    assembly.emplace<PreferredPatientEquipmentCost>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace ba

#endif  // BA_CONSTRAINTS_HPP
