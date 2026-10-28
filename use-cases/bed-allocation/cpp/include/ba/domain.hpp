#ifndef BA_DOMAIN_HPP
#define BA_DOMAIN_HPP

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace ba
{

// ---------------------------------------------------------------------------
// Score  (HardMediumSoftScore mirror)
// ---------------------------------------------------------------------------
// The Java quickstart scores with HardMediumSoftScore; the generic engine
// (include/optimaru/) only knows HardSoftScore(int64 hard, double soft). Mapping rule:
//
//   engine.hard = hard
//   engine.soft = medium * kMediumUnit + soft      (kMediumUnit = 10^9)
//
// All bed-allocation constraints penalize whole stay-nights, so medium and
// soft are integers; with |soft| < 10^9 the medium level strictly dominates
// the soft level inside the engine's double (exact up to 2^53), preserving
// the HardMediumSoft lexicographic order. |soft| is bounded by
// ~228 * totalStayNights (see the constraint weights below), so the mapping
// is exact for any instance with fewer than ~4.3 million stay-nights.
constexpr std::int64_t kMediumUnit = 1000000000LL;

struct Score
{
    std::int64_t hard = 0;
    std::int64_t medium = 0;
    std::int64_t soft = 0;

    bool isFeasible() const { return hard >= 0; }

    bool operator<(const Score &o) const
    {
        if (hard != o.hard)
            return hard < o.hard;
        if (medium != o.medium)
            return medium < o.medium;
        return soft < o.soft;
    }
    bool operator>(const Score &o) const { return o < *this; }
    bool operator<=(const Score &o) const { return !(o < *this); }
    bool operator>=(const Score &o) const { return !(*this < o); }
    bool operator==(const Score &o) const
    {
        return hard == o.hard && medium == o.medium && soft == o.soft;
    }

    std::string toString() const
    {
        return std::to_string(hard) + "hard/" + std::to_string(medium)
               + "medium/" + std::to_string(soft) + "soft";
    }
};

inline optimaru::HardSoftScore toEngineScore(const Score &s)
{
    return {s.hard,
            static_cast<double>(s.medium) * static_cast<double>(kMediumUnit)
                + static_cast<double>(s.soft)};
}

// Inverse of toEngineScore. Every constraint penalizes (medium/soft <= 0 and
// integral), so truncating integer division decodes the two levels exactly.
inline Score fromEngineScore(const optimaru::HardSoftScore &e)
{
    const std::int64_t combined
        = static_cast<std::int64_t>(std::llround(e.soft));
    return {e.hard, combined / kMediumUnit, combined % kMediumUnit};
}

// ---------------------------------------------------------------------------
// Enums  (org.acme.bedallocation.domain.Gender / GenderLimitation)
// ---------------------------------------------------------------------------
enum class Gender
{
    MALE,
    FEMALE
};

enum class GenderLimitation
{
    ANY_GENDER,  // mixed
    MALE_ONLY,
    FEMALE_ONLY,
    SAME_GENDER  // dependent on the first occupant
};

// ---------------------------------------------------------------------------
// Department  (problem fact)
// ---------------------------------------------------------------------------
// minimumAge / maximumAge nullopt == no age restriction (Java null).
struct Department
{
    std::string id;
    std::string name;
    std::map<std::string, int> specialtyToPriority;
    std::optional<int> minimumAge;
    std::optional<int> maximumAge;
};

// ---------------------------------------------------------------------------
// Room  (problem fact)
// ---------------------------------------------------------------------------
struct Room
{
    std::string id;
    std::string name;
    int department = 0;  // index into BedPlan::departments
    int capacity = 0;
    GenderLimitation genderLimitation = GenderLimitation::ANY_GENDER;
    std::vector<std::string> equipment;
};

// ---------------------------------------------------------------------------
// Bed  (problem fact; the planning value)
// ---------------------------------------------------------------------------
struct Bed
{
    std::string id;
    int room = 0;  // index into BedPlan::rooms
    int indexInRoom = 0;
};

// ---------------------------------------------------------------------------
// Stay  (@PlanningEntity)
// ---------------------------------------------------------------------------
// `bed` is the basic @PlanningVariable(allowsUnassigned = true). Encoding:
//   -1                    uninitialized (construction heuristic input;
//                         contributes nothing to the score)
//   [0, bedCount)         index into BedPlan::beds
//   bedCount (sentinel)   "no bed" — the quickstart's null bed after solving,
//                         penalized per night on the medium level
// The sentinel keeps allowsUnassigned semantics inside the engine's BASIC
// value range [0, valueCount): local search can move a stay to "no bed" when
// every real bed causes a hard conflict.
//
// Dates are integer day indices (e.g. LocalDate::toEpochDay); only
// differences matter. nightCount keeps the quickstart's "+ 1".
struct Stay
{
    std::string id;
    std::string patientName;
    Gender patientGender = Gender::MALE;
    int patientAge = 0;
    std::optional<int> patientPreferredMaximumRoomCapacity;
    std::vector<std::string> patientRequiredEquipment;
    std::vector<std::string> patientPreferredEquipment;
    int arrivalDay = 0;
    int departureDay = 0;
    std::string specialty;

    int bed = -1;  // planning variable (see the encoding above)

    // Precomputed by ScoreCalculator::prepare (interned equipment ids and the
    // per-department priority of `specialty`; kept at the end so the
    // brace-initialisers elsewhere still compile):
    std::vector<int> requiredEquipmentIds;
    std::vector<int> preferredEquipmentIds;
    std::vector<int> specialtyPriorityByDept;  // -1 == specialty not offered

    int nightCount() const { return departureDay - arrivalDay + 1; }

    // The number of nights both stays are in house
    // (Stay::calculateSameNightCount).
    int sameNightCount(const Stay &other) const
    {
        const int maxArrival
            = arrivalDay > other.arrivalDay ? arrivalDay : other.arrivalDay;
        const int minDeparture = departureDay < other.departureDay
                                     ? departureDay
                                     : other.departureDay;
        const int nights = minDeparture - maxArrival + 1;
        return nights > 0 ? nights : 0;
    }
};

// ---------------------------------------------------------------------------
// BedPlan  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct BedPlan
{
    std::vector<Department> departments;
    std::vector<Room> rooms;
    std::vector<Bed> beds;
    std::vector<Stay> stays;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against re-interning the
    // equipment ids on every calculateScore call.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int equipmentTypeCount = 0;
    // Flattened room equipment: roomEquipmentFlat[room * equipmentTypeCount
    // + equipmentId] != 0 iff the room has that equipment.
    std::vector<char> roomEquipmentFlat;

    int bedCount() const { return static_cast<int>(beds.size()); }
    // The "no bed" sentinel planning value (the quickstart's null bed).
    int noBedValue() const { return bedCount(); }

    bool roomHasEquipment(int roomIndex, int equipmentId) const
    {
        return roomEquipmentFlat[static_cast<std::size_t>(roomIndex)
                                     * equipmentTypeCount
                                 + equipmentId]
               != 0;
    }
};

}  // namespace ba

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are stays, the planning variable
// is Stay::bed, the value range is the bed list plus the "no bed" sentinel
// (allowsUnassigned = true). The Java quickstart has no @PlanningPin, so no
// isPinned hook is declared.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<ba::BedPlan>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const ba::BedPlan &p)
    {
        return static_cast<int>(p.stays.size());
    }
    static int valueCount(const ba::BedPlan &p) { return p.bedCount() + 1; }
    static int getVariable(const ba::BedPlan &p, int stayIndex)
    {
        return p.stays[stayIndex].bed;
    }
    static void setVariable(ba::BedPlan &p, int stayIndex, int bed)
    {
        p.stays[stayIndex].bed = bed;
    }
};

#endif  // BA_DOMAIN_HPP
