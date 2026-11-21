#ifndef BA_DOMAIN_HPP
#define BA_DOMAIN_HPP

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/hard_medium_soft_score.hpp"
#include "optimaru/score/levels.hpp"

namespace ba {

// ---------------------------------------------------------------------------
// Score  (hard / medium / soft)
// ---------------------------------------------------------------------------
// The domain scores on hard / medium / soft levels; the generic engine
// (optimaru/cpp/optimaru/) only knows Score(int64 hard, double soft). Mapping rule:
//
//   engine.hard = hard
//   engine.soft = medium * kMediumUnit + soft      (kMediumUnit = 10^9)
//
// All bed-allocation constraints penalize whole stay-nights, so medium and
// soft are integers; with |soft| < 10^9 the medium level strictly dominates
// the soft level inside the engine's double (exact up to 2^53), preserving
// the hard / medium / soft lexicographic order. |soft| is bounded by
// ~228 * totalStayNights at the default weights (the components'
// kDefaultWeight, ba/constraints/*.hpp), so the mapping is exact for any
// instance with fewer than ~4.3 million stay-nights.
//
// The fold is declared ONCE, here: toEngineScore below packs the monolithic
// Score with it, and the MediumFoldOf<BedPlan> specialization at the end of
// this header hands the same constant to the decomposed calculator, which
// keeps a separate medium accumulator. A medium constraint's weight is
// therefore in medium units (unassignedStaysCost: 1), never 10^9.
constexpr std::int64_t kMediumUnit = 1000000000LL;  // 10^9
constexpr optimaru::MediumFold kMediumFold =
        optimaru::MediumFold::into_soft(static_cast<double>(kMediumUnit));

using Score = optimaru::HardMediumSoftScore;

// On the medium or the soft level of Solver::Config::target_score: no floor on
// the soft side, so only the hard level is compared. stop_at_first_feasible
// passes (0, kNoLevelFloor, kNoLevelFloor).
using optimaru::kNoLevelFloor;

inline optimaru::Score toEngineScore(const Score& s) {
    return optimaru::to_engine_score(kMediumFold, s);
}

// A best-score limit (Solver::Config::target_score) in engine terms.
inline optimaru::Score toEngineScoreLimit(const Score& limit) {
    return optimaru::to_engine_score_limit(kMediumFold, limit);
}

// Inverse of toEngineScore.
inline Score fromEngineScore(const optimaru::Score& e) {
    return optimaru::from_engine_score(kMediumFold, e);
}

// ---------------------------------------------------------------------------
// Enums  (Gender / GenderLimitation)
// ---------------------------------------------------------------------------
enum class Gender { MALE, FEMALE };

enum class GenderLimitation {
    ANY_GENDER,  // mixed
    MALE_ONLY,
    FEMALE_ONLY,
    SAME_GENDER  // dependent on the first occupant
};

// ---------------------------------------------------------------------------
// Department  (problem fact)
// ---------------------------------------------------------------------------
// minimumAge / maximumAge nullopt == no age restriction.
struct Department {
    std::string id;
    std::string name;
    std::map<std::string, int> specialtyToPriority;
    std::optional<int> minimumAge;
    std::optional<int> maximumAge;
};

// ---------------------------------------------------------------------------
// Room  (problem fact)
// ---------------------------------------------------------------------------
struct Room {
    std::string id;
    std::string name;
    int department = 0;  // index into BedPlan::departments
    int capacity = 0;
    GenderLimitation genderLimitation = GenderLimitation::ANY_GENDER;
    std::vector<std::string> equipment;
};

// ---------------------------------------------------------------------------
// Bed  (problem fact; the value a stay is assigned)
// ---------------------------------------------------------------------------
struct Bed {
    std::string id;
    int room = 0;  // index into BedPlan::rooms
    int indexInRoom = 0;
};

// ---------------------------------------------------------------------------
// Stay  (the entity the solver assigns)
// ---------------------------------------------------------------------------
// `bed` is the decision variable; a stay may be left unassigned. Encoding:
//   -1                    uninitialized (construction heuristic input;
//                         contributes nothing to the score)
//   [0, bedCount)         index into BedPlan::beds
//   bedCount (sentinel)   "no bed" — the stay is left unassigned,
//                         penalized per night on the medium level
//                         (unassignedStaysCost)
// The sentinel keeps "may be unassigned" inside the engine's assignment
// value range [0, value_count): local search can move a stay to "no bed" when
// every real bed causes a hard conflict.
//
// Dates are integer day indices (e.g. days since the epoch); only
// differences matter. nightCount is departureDay - arrivalDay + 1.
struct Stay {
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

    int bed = -1;  // decision variable (see the encoding above)

    // Precomputed by ScoreCalculator::prepare (interned equipment ids and the
    // per-department priority of `specialty`; kept at the end so the
    // brace-initialisers elsewhere still compile):
    std::vector<int> requiredEquipmentIds;
    std::vector<int> preferredEquipmentIds;
    std::vector<int> specialtyPriorityByDept;  // -1 == specialty not offered

    int nightCount() const { return departureDay - arrivalDay + 1; }

    // The number of nights both stays are in house
    // (Stay::calculateSameNightCount).
    int sameNightCount(const Stay& other) const {
        const int maxArrival = arrivalDay > other.arrivalDay ? arrivalDay : other.arrivalDay;
        const int minDeparture =
                departureDay < other.departureDay ? departureDay : other.departureDay;
        const int nights = minDeparture - maxArrival + 1;
        return nights > 0 ? nights : 0;
    }
};

// ---------------------------------------------------------------------------
// BedPlan  (the whole problem and its solution)
// ---------------------------------------------------------------------------
struct BedPlan {
    std::vector<Department> departments;
    std::vector<Room> rooms;
    std::vector<Bed> beds;
    std::vector<Stay> stays;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against re-interning the
    // equipment ids on every score call.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare:
    int equipmentTypeCount = 0;
    // Flattened room equipment: roomEquipmentFlat[room * equipmentTypeCount
    // + equipmentId] != 0 iff the room has that equipment.
    std::vector<char> roomEquipmentFlat;

    int bedCount() const { return static_cast<int>(beds.size()); }
    // The "no bed" sentinel value (a stay on it is unassigned).
    int noBedValue() const { return bedCount(); }

    bool roomHasEquipment(int roomIndex, int equipmentId) const {
        return roomEquipmentFlat[static_cast<std::size_t>(roomIndex) * equipmentTypeCount +
                                 equipmentId] != 0;
    }
};

}  // namespace ba

// ---------------------------------------------------------------------------
// SolutionTraits specialization  (how the engine sees the domain): entities
// are stays, the decision variable is Stay::bed, the value range is the bed
// list plus the "no bed" sentinel. Nothing is pinned, so no is_pinned hook
// is declared.
// ---------------------------------------------------------------------------
template <>
struct optimaru::SolutionTraits<ba::BedPlan> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Assignment;
    static int entity_count(const ba::BedPlan& p) { return static_cast<int>(p.stays.size()); }
    static int value_count(const ba::BedPlan& p) { return p.bedCount() + 1; }
    static int value_of(const ba::BedPlan& p, int stayIndex) { return p.stays[stayIndex].bed; }
    static void assign(ba::BedPlan& p, int stayIndex, int bed) { p.stays[stayIndex].bed = bed; }
};

// ---------------------------------------------------------------------------
// Medium fold registration
// ---------------------------------------------------------------------------
// The decomposed calculator folds its medium accumulator with the domain's
// kMediumFold (see the Score adapter above).
template <>
struct optimaru::MediumFoldOf<ba::BedPlan> {
    static constexpr optimaru::MediumFold value = ba::kMediumFold;
};

#endif  // BA_DOMAIN_HPP
