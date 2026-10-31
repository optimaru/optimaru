#include "ba/score_calculator.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "ba/constraints/bed_conflict_cost.hpp"
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

namespace
{

// The monolithic path keeps the quickstart's integer arithmetic, with the
// weights read off the components (their kDefaultWeight is the only place a
// default lives). The medium weight is in medium units: toEngineScore folds
// the medium level into soft with kMediumFold.
template <typename Component> constexpr std::int64_t integralWeight()
{
    constexpr auto weight
        = static_cast<std::int64_t>(Component::kDefaultWeight);
    static_assert(static_cast<double>(weight) == Component::kDefaultWeight,
                  "the monolithic calculator assumes integral default "
                  "weights");
    return weight;
}

constexpr std::int64_t kBedConflictWeight = integralWeight<BedConflictCost>();
constexpr std::int64_t kFemaleInMaleRoomWeight
    = integralWeight<FemaleInMaleRoomCost>();
constexpr std::int64_t kMaleInFemaleRoomWeight
    = integralWeight<MaleInFemaleRoomCost>();
constexpr std::int64_t kMixedGenderRoomWeight
    = integralWeight<MixedGenderRoomCost>();
constexpr std::int64_t kDepartmentMinAgeWeight
    = integralWeight<DepartmentMinAgeCost>();
constexpr std::int64_t kDepartmentMaxAgeWeight
    = integralWeight<DepartmentMaxAgeCost>();
constexpr std::int64_t kRequiredEquipmentWeight
    = integralWeight<RequiredPatientEquipmentCost>();
constexpr std::int64_t kUnassignedStaysWeight
    = integralWeight<UnassignedStaysCost>();
constexpr std::int64_t kPreferredCapacityWeight
    = integralWeight<PreferredMaxRoomCapacityCost>();
constexpr std::int64_t kDepartmentSpecialtyWeight
    = integralWeight<DepartmentSpecialtyCost>();
constexpr std::int64_t kSpecialtyPriorityWeight
    = integralWeight<DepartmentSpecialtyPriorityCost>();
constexpr std::int64_t kPreferredEquipmentWeight
    = integralWeight<PreferredPatientEquipmentCost>();

// The monolith assigns every term to its component's default level.
static_assert(BedConflictCost::kDefaultLevel == optimaru::Level::Hard
                  && FemaleInMaleRoomCost::kDefaultLevel
                         == optimaru::Level::Hard
                  && MaleInFemaleRoomCost::kDefaultLevel
                         == optimaru::Level::Hard
                  && MixedGenderRoomCost::kDefaultLevel
                         == optimaru::Level::Hard
                  && DepartmentMinAgeCost::kDefaultLevel
                         == optimaru::Level::Hard
                  && DepartmentMaxAgeCost::kDefaultLevel
                         == optimaru::Level::Hard
                  && RequiredPatientEquipmentCost::kDefaultLevel
                         == optimaru::Level::Hard
                  && UnassignedStaysCost::kDefaultLevel
                         == optimaru::Level::Medium
                  && PreferredMaxRoomCapacityCost::kDefaultLevel
                         == optimaru::Level::Soft
                  && DepartmentSpecialtyCost::kDefaultLevel
                         == optimaru::Level::Soft
                  && DepartmentSpecialtyPriorityCost::kDefaultLevel
                         == optimaru::Level::Soft
                  && PreferredPatientEquipmentCost::kDefaultLevel
                         == optimaru::Level::Soft,
              "the monolithic calculator scores every term on its "
              "component's default level");

}  // namespace

std::int64_t ScoreCalculator::sameBedPairPenalty(const Stay &a, const Stay &b)
{
    return kBedConflictWeight * a.sameNightCount(b);
}

std::int64_t ScoreCalculator::mixedGenderPairPenalty(const Stay &a,
                                                     const Stay &b)
{
    if (a.patientGender == b.patientGender)
        return 0;
    return kMixedGenderRoomWeight * a.sameNightCount(b);
}

void ScoreCalculator::prepare(BedPlan &plan)
{
    if (plan.prepared)
        return;

    // Intern every equipment name that appears anywhere (rooms, required,
    // preferred) into a dense id.
    std::map<std::string, int> equipmentIds;
    const auto internEquipment = [&](const std::string &name)
    {
        const auto [it, inserted]
            = equipmentIds.emplace(name, static_cast<int>(equipmentIds.size()));
        return it->second;
    };
    for (const Room &room : plan.rooms)
    {
        for (const std::string &equipment : room.equipment)
            internEquipment(equipment);
    }
    const auto internedUnique = [&](const std::vector<std::string> &names)
    {
        std::vector<int> ids;
        ids.reserve(names.size());
        for (const std::string &name : names)
        {
            const int id = internEquipment(name);
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
        }
        return ids;
    };
    for (Stay &stay : plan.stays)
    {
        stay.requiredEquipmentIds
            = internedUnique(stay.patientRequiredEquipment);
        stay.preferredEquipmentIds
            = internedUnique(stay.patientPreferredEquipment);
    }
    plan.equipmentTypeCount = static_cast<int>(equipmentIds.size());

    // Flatten the room equipment sets for O(1) lookups.
    plan.roomEquipmentFlat.assign(
        plan.rooms.size() * std::max(1, plan.equipmentTypeCount), 0);
    for (std::size_t r = 0; r < plan.rooms.size(); ++r)
    {
        for (const std::string &equipment : plan.rooms[r].equipment)
        {
            plan.roomEquipmentFlat[r * plan.equipmentTypeCount
                                   + equipmentIds[equipment]] = 1;
        }
    }

    // Resolve each stay's specialty priority per department (-1 == the
    // department does not offer the specialty).
    for (Stay &stay : plan.stays)
    {
        stay.specialtyPriorityByDept.assign(plan.departments.size(), -1);
        for (std::size_t d = 0; d < plan.departments.size(); ++d)
        {
            const auto &priorities = plan.departments[d].specialtyToPriority;
            const auto it = priorities.find(stay.specialty);
            if (it != priorities.end())
                stay.specialtyPriorityByDept[d] = it->second;
        }
    }

    plan.prepared = true;
}

ScoreCalculator::SingleImpact
ScoreCalculator::singleStayImpact(const BedPlan &plan, int stayIndex, int value)
{
    SingleImpact impact;
    if (value < 0)
        return impact;  // uninitialized: contributes nothing

    const Stay &stay = plan.stays[stayIndex];
    const std::int64_t nights = stay.nightCount();

    if (value == plan.noBedValue())
    {
        // medium 8: unassignedStaysCost.
        impact.medium += kUnassignedStaysWeight * nights;
        return impact;
    }

    const Room &room = plan.rooms[plan.beds[value].room];
    const Department &department = plan.departments[room.department];

    // hard 2 + 3: femaleInMaleRoomCost / maleInFemaleRoomCost.
    if (stay.patientGender == Gender::FEMALE
        && room.genderLimitation == GenderLimitation::MALE_ONLY)
    {
        impact.hard += kFemaleInMaleRoomWeight * nights;
    }
    if (stay.patientGender == Gender::MALE
        && room.genderLimitation == GenderLimitation::FEMALE_ONLY)
    {
        impact.hard += kMaleInFemaleRoomWeight * nights;
    }

    // hard 5 + 6: departmentMinAgeCost / departmentMaxAgeCost.
    if (department.minimumAge && stay.patientAge < *department.minimumAge)
        impact.hard += kDepartmentMinAgeWeight * nights;
    if (department.maximumAge && stay.patientAge > *department.maximumAge)
        impact.hard += kDepartmentMaxAgeWeight * nights;

    // hard 7: requiredPatientEquipmentCost — when the room misses at least one
    // required equipment, weigh by the count of required equipment the room
    // DOES have (the quickstart's exact, test-pinned semantics).
    const int roomIndex = plan.beds[value].room;
    std::int64_t requiredPresent = 0;
    for (const int equipmentId : stay.requiredEquipmentIds)
    {
        if (plan.roomHasEquipment(roomIndex, equipmentId))
            ++requiredPresent;
    }
    if (requiredPresent
        < static_cast<std::int64_t>(stay.requiredEquipmentIds.size()))
    {
        impact.hard += kRequiredEquipmentWeight * nights * requiredPresent;
    }

    // soft 9: preferredMaxRoomCapacityCost.
    if (stay.patientPreferredMaximumRoomCapacity
        && *stay.patientPreferredMaximumRoomCapacity < room.capacity)
    {
        impact.soft += kPreferredCapacityWeight * nights;
    }

    // soft 10 + 11: departmentSpecialtyCost / departmentSpecialtyPriorityCost.
    const int priority = stay.specialtyPriorityByDept[room.department];
    if (priority < 0)
        impact.soft += kDepartmentSpecialtyWeight * nights;
    else if (priority > 1)
        impact.soft += kSpecialtyPriorityWeight * (priority - 1) * nights;

    // soft 12: preferredPatientEquipmentCost — weigh by the count of preferred
    // equipment the room lacks.
    std::int64_t preferredMissing = 0;
    for (const int equipmentId : stay.preferredEquipmentIds)
    {
        if (!plan.roomHasEquipment(roomIndex, equipmentId))
            ++preferredMissing;
    }
    impact.soft += kPreferredEquipmentWeight * nights * preferredMissing;

    return impact;
}

Score ScoreCalculator::calculateScore(BedPlan &plan)
{
    prepare(plan);

    std::int64_t hard = 0;
    std::int64_t medium = 0;
    std::int64_t soft = 0;

    // Per-stay constraints (hard 2/3/5/6/7, medium 8, soft 9~12).
    for (std::size_t s = 0; s < plan.stays.size(); ++s)
    {
        const SingleImpact impact
            = singleStayImpact(plan, static_cast<int>(s), plan.stays[s].bed);
        hard += impact.hard;
        medium += impact.medium;
        soft += impact.soft;
    }

    // Pair constraints (hard 1 + 4) over the stays in real beds.
    std::vector<std::vector<int>> bedStays(plan.beds.size());
    std::vector<std::vector<int>> roomStays(plan.rooms.size());
    for (std::size_t s = 0; s < plan.stays.size(); ++s)
    {
        const int bed = plan.stays[s].bed;
        if (bed < 0 || bed >= plan.bedCount())
            continue;
        bedStays[bed].push_back(static_cast<int>(s));
        roomStays[plan.beds[bed].room].push_back(static_cast<int>(s));
    }
    for (const std::vector<int> &occupants : bedStays)
    {
        for (std::size_t i = 0; i < occupants.size(); ++i)
        {
            for (std::size_t j = i + 1; j < occupants.size(); ++j)
            {
                hard += sameBedPairPenalty(plan.stays[occupants[i]],
                                           plan.stays[occupants[j]]);
            }
        }
    }
    for (std::size_t r = 0; r < roomStays.size(); ++r)
    {
        if (plan.rooms[r].genderLimitation != GenderLimitation::SAME_GENDER)
            continue;
        const std::vector<int> &occupants = roomStays[r];
        for (std::size_t i = 0; i < occupants.size(); ++i)
        {
            for (std::size_t j = i + 1; j < occupants.size(); ++j)
            {
                hard += mixedGenderPairPenalty(plan.stays[occupants[i]],
                                               plan.stays[occupants[j]]);
            }
        }
    }

    plan.score = Score{-hard, -medium, -soft};
    return plan.score;
}

}  // namespace ba
