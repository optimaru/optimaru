#ifndef BA_CONSTRAINTS_HPP
#define BA_CONSTRAINTS_HPP

// ---------------------------------------------------------------------------
// Constraint plugins — umbrella header
// ---------------------------------------------------------------------------
// One component per file under constraints/; common.hpp holds what all of them
// need (the calculator aliases, the per-stay bases and the "prepare() has run"
// guard) and carries the pattern's documentation.
//
// Including this header still gives the whole set, so nothing that used it has
// to change. Adding a component is a file plus a line here (and a line in
// addDefaultConstraints below when it belongs to the default assembly).

#include "ba/constraints/assign_every_patient_to_a_bed.hpp"
#include "ba/constraints/common.hpp"
#include "ba/constraints/department_maximum_age.hpp"
#include "ba/constraints/department_minimum_age.hpp"
#include "ba/constraints/department_specialty.hpp"
#include "ba/constraints/department_specialty_not_first_priority.hpp"
#include "ba/constraints/different_gender_in_same_gender_room_in_same_night.hpp"
#include "ba/constraints/female_in_male_room.hpp"
#include "ba/constraints/male_in_female_room.hpp"
#include "ba/constraints/preferred_maximum_room_capacity.hpp"
#include "ba/constraints/preferred_patient_equipment.hpp"
#include "ba/constraints/required_patient_equipment.hpp"
#include "ba/constraints/same_bed_in_same_night.hpp"

namespace ba
{

// ---------------------------------------------------------------------------
// Standard assembly
// ---------------------------------------------------------------------------
// The full provider at the quickstart's weights. This assembly scores
// bit-for-bit like ScoreCalculator::calculateScore and the monolithic
// incremental calculator.
//
// `defaultConstraints()` hands the whole set back as a ConstraintAssembly,
// so a caller states only what differs from it:
//
//     DecomposedCalculator calc;
//     defaultConstraints()
//         .weight("assignEveryPatientToABed", 2)
//         .drop("departmentMaximumAge")
//         .into(calc);
//
// `addDefaultConstraints(calc)` is the same set poured straight into a
// calculator, for callers that change nothing.
inline ConstraintAssembly defaultConstraints()
{
    ConstraintAssembly assembly;
    assembly.emplace<SameBedInSameNight>();
    assembly.emplace<FemaleInMaleRoom>();
    assembly.emplace<MaleInFemaleRoom>();
    assembly.emplace<DifferentGenderInSameGenderRoomInSameNight>();
    assembly.emplace<DepartmentMinimumAge>();
    assembly.emplace<DepartmentMaximumAge>();
    assembly.emplace<RequiredPatientEquipment>();
    assembly.emplace<AssignEveryPatientToABed>();
    assembly.emplace<PreferredMaximumRoomCapacity>();
    assembly.emplace<DepartmentSpecialty>();
    assembly.emplace<DepartmentSpecialtyNotFirstPriority>();
    assembly.emplace<PreferredPatientEquipment>();
    return assembly;
}

inline void addDefaultConstraints(DecomposedCalculator &calculator)
{
    defaultConstraints().into(calculator);
}

}  // namespace ba

#endif  // BA_CONSTRAINTS_HPP
