#ifndef VRP_DOMAIN_HPP
#define VRP_DOMAIN_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "optimaru/solution/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/hard_medium_soft_score.hpp"
#include "optimaru/score/levels.hpp"

namespace vrp {

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// Every timestamp in a single problem falls on the same day ("tomorrow at
// HH:MM"), so time is an int64 number of seconds from a common reference
// (start of that day). All arithmetic (arrival = departure + drivingSeconds,
// etc.) is then plain integer math.
using Seconds = std::int64_t;
using Meters = std::int64_t;

constexpr Seconds NULL_TIME = -1;  // no time (e.g. an unassigned visit)

// A square matrix over the plan's locations, indexed by Location::index:
// driving seconds or driving meters from row to column.
using Matrix = std::vector<std::vector<std::int64_t>>;

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// A three-level hard / medium / soft score.
// Constraints *penalize*, so scores are <= 0; higher (closer to zero) is
// better. Comparison is lexicographic: hard dominates medium dominates soft.
//
// The generic engine (core/optimaru/) only knows Score(int64 hard, double
// soft), so the two lower levels are packed into one (the food-packaging
// use case's convention):
//
//   engine.hard = hard
//   engine.soft = medium * kMediumUnit + soft      (kMediumUnit = 10^9)
//
// Every constraint penalizes whole seconds or whole minutes, so medium and
// soft are integers and the combined int64 converts to double exactly while
// |medium * kMediumUnit + soft| < 2^53. That preserves the hard / medium / soft
// lexicographic order as long as
//   |soft|   < 10^9   (total driving seconds == ~31 years of driving), and
//   |medium| < 9*10^6 (service minutes of the unassigned visits == ~17 years),
// which holds with an enormous margin at any instance size this solver is
// used on. Feasibility stays hard-only.
//
// The fold is declared ONCE, here: toEngineScore below packs the monolithic
// Score with it, and the MediumFoldOf<VehicleRoutePlan> specialization at the
// end of this header hands the same constant to the decomposed calculator,
// which keeps a separate medium accumulator. A medium constraint's weight is
// therefore in medium units (unassignedVisitsCost: 1), never 10^9.
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
// Location
// ---------------------------------------------------------------------------
// Every distinct point (each depot, each visit) is registered once and
// referenced by index into the matrices of VehicleRoutePlan.
struct Location {
    double latitude = 0.0;
    double longitude = 0.0;
    int index = -1;  // row/column into every matrix
};

// ---------------------------------------------------------------------------
// Visit  (the list element)
// ---------------------------------------------------------------------------
// The input fields are immutable; the derived fields (vehicleIndex,
// arrivalTime) are recomputed from the vehicle routes.
struct Visit {
    std::string id;
    std::string name;
    int locationIndex = -1;
    int demand = 0;
    Seconds minStartTime = 0;
    Seconds maxEndTime = 0;
    Seconds serviceDuration = 0;  // seconds

    // Medium-level cost of leaving this visit unrouted.
    //
    // By default (kUnassignedPenaltyAuto) an unassigned visit is penalized
    // on the medium level by its service duration in minutes. That rule is degenerate whenever
    // service durations are small or zero — the penalty rounds to 0, "route
    // nothing" scores 0hard/0medium/0soft, and dropping every visit becomes
    // the optimum. Real instances (which typically carry an
    // explicit per-visit unassigned_penalty) need to say what a dropped visit
    // actually costs, so this field lets them.
    static constexpr Seconds kUnassignedPenaltyAuto = -1;
    std::int64_t unassignedPenaltyMinutes = kUnassignedPenaltyAuto;

    // The medium penalty actually charged when this visit is unrouted.
    std::int64_t unassignedPenalty() const {
        return unassignedPenaltyMinutes == kUnassignedPenaltyAuto ? serviceDuration / 60
                                                                  : unassignedPenaltyMinutes;
    }

    // The group this visit travels in, or -1 for none. The visits of one
    // group ride one vehicle together, or all wait (visitGroupCost): several
    // orders the input has merged into one job.
    int groupIndex = -1;
    // The visits of its group this one comes after on the route
    // (visitOrderCost). Together they are a partial order on the group: a
    // visit named by none and naming none may go anywhere among the others.
    std::vector<int> afterVisitIndices = {};

    // --- derived (recomputed by ScoreCalculator::updateShadowVariables) ---
    int vehicleIndex = -1;  // -1 == unassigned
    Seconds arrivalTime = NULL_TIME;
};

// ---------------------------------------------------------------------------
// ProfileVisits
// ---------------------------------------------------------------------------
// What one kind of vehicle may serve: the visits it may not
// (profileForbiddenVisitCost, hard) and the visits it is preferred for
// (profilePreferredVisitCost, soft). A vehicle names its entry with
// Vehicle::profileVisitsIndex. Both lists hold indices into
// VehicleRoutePlan::visits in strictly increasing order, and no index is in
// both (checkProfileVisits in solver.hpp refuses anything else).
struct ProfileVisits {
    std::vector<int> forbiddenVisitIndices;
    std::vector<int> preferredVisitIndices;
};

// ---------------------------------------------------------------------------
// Vehicle  (the list owner)
// ---------------------------------------------------------------------------
// visitIndices is the decision: the ordered list of visits this vehicle
// services. A visit may appear in no vehicle's list at all (see
// VehicleRoutePlan::unassignedVisitIndices).
struct Vehicle {
    std::string id;
    int capacity = 0;
    // Where the route starts, and where it ends: -1 for an open route, which
    // ends at its last visit, so no closing leg is driven or counted toward
    // the route end time. Two vehicles may share a depot's location.
    int startLocationIndex = -1;
    int endLocationIndex = -1;
    // Which of the plan's matrices this vehicle drives on (its profile).
    int profileIndex = 0;
    // Which entry of VehicleRoutePlan::profileVisits says what it may serve;
    // -1 (the default) for none: it may serve every visit and is preferred
    // for none.
    int profileVisitsIndex = -1;
    Seconds departureTime = 0;
    // Optional route policies. Non-positive means disabled.
    Seconds maxTravelTime = 0;
    Seconds maxRouteDuration = 0;
    Seconds maxEndTime = 0;
    int maxStops = 0;
    double minLoadRatio = 0.0;
    double fixedCost = 0.0;
    double maxRouteRadiusMeters = 0.0;
    std::vector<int> visitIndices;  // ordered route

    bool returnsToDepot() const { return endLocationIndex >= 0; }

    // The closing leg from the last visit to the end depot in `matrix` (one
    // of this vehicle's profile), or 0 for an open route. Callers charge it
    // only for a route with visits: an idle vehicle drives nothing, even when
    // its two depots differ. Every scoring path charges the closing leg
    // through this one helper so the monolithic, incremental and plugin costs
    // cannot drift.
    std::int64_t endLeg(const Matrix& matrix, int lastLocation) const {
        return returnsToDepot() ? matrix[static_cast<std::size_t>(lastLocation)]
                                        [static_cast<std::size_t>(endLocationIndex)]
                                : 0;
    }
};

// ---------------------------------------------------------------------------
// VehicleRoutePlan  (the solution)
// ---------------------------------------------------------------------------
struct VehicleRoutePlan {
    std::string name;
    std::vector<Location> locations;
    std::vector<Vehicle> vehicles;
    std::vector<Visit> visits;

    // The unassigned pool. A visit may be left unassigned, but the engine's
    // sequence shape requires every
    // element to live in exactly one list, so the pool is materialized as one
    // extra virtual list at index vehicles.size() (see the SolutionTraits
    // below). Everything in it is unassigned: it has no route, no arrival
    // times, and contributes only the medium unassigned penalty.
    //
    // Callers building a plan by hand may leave this empty and let
    // rebuildUnassignedPool() fill it — Solver::solve does that itself.
    std::vector<int> unassignedVisitIndices;

    // One driving-seconds and one driving-meters matrix per profile
    // (Vehicle::profileIndex), each square over the locations. Given by the
    // caller, or filled from the coordinates by HaversineDrivingTimeCalculator.
    std::vector<Matrix> durationMatrices;
    std::vector<Matrix> distanceMatrices;

    // The visits each kind of vehicle may not serve or is preferred for (see
    // ProfileVisits); empty for a plan without such lists.
    std::vector<ProfileVisits> profileVisits;

    Score score;

    // The entry of `vehicle`, or nullptr when it names none.
    const ProfileVisits* profileVisitsOf(const Vehicle& vehicle) const {
        return vehicle.profileVisitsIndex < 0
                       ? nullptr
                       : &profileVisits[static_cast<std::size_t>(vehicle.profileVisitsIndex)];
    }
    // Whether vehicle `vehicleIndex` may not serve visit `visitIndex`: a
    // binary search of its sorted forbidden list, for the operators that place
    // visits from the plan (the constraint components keep a table instead).
    bool forbids(int vehicleIndex, int visitIndex) const {
        const ProfileVisits* entry =
                profileVisitsOf(vehicles[static_cast<std::size_t>(vehicleIndex)]);
        return entry != nullptr &&
               std::binary_search(entry->forbiddenVisitIndices.begin(),
                                  entry->forbiddenVisitIndices.end(), visitIndex);
    }
    // Whether some vehicle names a profile entry with a visit in one of its
    // lists. Such a plan is scored with the constraint assembly, which holds
    // the two profile visit constraints; the core constraints do not read the
    // lists.
    bool hasProfileVisits() const { return hasForbiddenVisits() || hasPreferredVisits(); }
    // Whether some vehicle's entry forbids a visit / prefers one.
    bool hasForbiddenVisits() const {
        for (const Vehicle& vehicle : vehicles) {
            const ProfileVisits* entry = profileVisitsOf(vehicle);
            if (entry != nullptr && !entry->forbiddenVisitIndices.empty()) return true;
        }
        return false;
    }
    bool hasPreferredVisits() const {
        for (const Vehicle& vehicle : vehicles) {
            const ProfileVisits* entry = profileVisitsOf(vehicle);
            if (entry != nullptr && !entry->preferredVisitIndices.empty()) return true;
        }
        return false;
    }

    const Matrix& durationsOf(const Vehicle& vehicle) const {
        return durationMatrices[static_cast<std::size_t>(vehicle.profileIndex)];
    }
    const Matrix& distancesOf(const Vehicle& vehicle) const {
        return distanceMatrices[static_cast<std::size_t>(vehicle.profileIndex)];
    }
    Seconds drivingTime(const Vehicle& vehicle, int fromLocation, int toLocation) const {
        return durationsOf(vehicle)[static_cast<std::size_t>(fromLocation)]
                                   [static_cast<std::size_t>(toLocation)];
    }

    // The legs between consecutive visits of elements[begin, end) in
    // `matrix`, walked backwards when `reversed`: what a run of one route
    // drives inside another vehicle's route when the two profiles differ.
    std::int64_t runLegs(const Matrix& matrix, const std::vector<int>& elements, std::size_t begin,
                         std::size_t end, bool reversed) const {
        std::int64_t total = 0;
        for (std::size_t k = begin + 1; k < end; ++k) {
            const auto a = static_cast<std::size_t>(
                    visits[static_cast<std::size_t>(elements[k - 1])].locationIndex);
            const auto b = static_cast<std::size_t>(
                    visits[static_cast<std::size_t>(elements[k])].locationIndex);
            total += reversed ? matrix[b][a] : matrix[a][b];
        }
        return total;
    }

    // Whether some group (Visit::groupIndex) holds two visits or more. Such a
    // plan is scored with the constraint assembly, which holds the two group
    // rules; a visit alone in its group has nobody to be kept with, so groups
    // of one change nothing.
    bool hasVisitGroups() const {
        std::unordered_set<int> seen;
        for (const Visit& visit : visits) {
            if (visit.groupIndex >= 0 && !seen.insert(visit.groupIndex).second) return true;
        }
        return false;
    }

    // Puts every visit that no vehicle services into the unassigned pool, so
    // the plan satisfies the engine's "every element is in exactly one list"
    // invariant. Idempotent.
    void rebuildUnassignedPool() {
        std::vector<char> routed(visits.size(), 0);
        for (const Vehicle& vehicle : vehicles) {
            for (int visitIndex : vehicle.visitIndices)
                routed[static_cast<std::size_t>(visitIndex)] = 1;
        }
        unassignedVisitIndices.clear();
        for (std::size_t i = 0; i < visits.size(); ++i) {
            if (!routed[i]) unassignedVisitIndices.push_back(static_cast<int>(i));
        }
    }
};

}  // namespace vrp

// ---------------------------------------------------------------------------
// SolutionTraits specialization
// ---------------------------------------------------------------------------
// The vehicles are the list owners and the visits are the elements — plus ONE
// extra list at index vehicles.size() holding the unassigned visits.
//
// Why the extra list rather than "an element in no list": the engine's sequence
// moves index elements by (list, position) and assume every element is
// reachable, so genuinely-unassigned elements would be invisible to relocate,
// swap and the LNS ruin — the solver could never assign a dropped visit back,
// nor drop an unprofitable one. Modelling the pool as a real list makes
// assignment and unassignment ordinary relocate moves, which is what lets the
// medium-level unassignedVisitsCost constraint actually be optimized.
template <>
struct optimaru::SolutionTraits<vrp::VehicleRoutePlan> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;

    // The index of the virtual unassigned-pool list.
    static int unassigned_list(const vrp::VehicleRoutePlan& s) {
        return static_cast<int>(s.vehicles.size());
    }

    static int list_count(const vrp::VehicleRoutePlan& s) {
        return static_cast<int>(s.vehicles.size()) + 1;
    }
    static int element_count(const vrp::VehicleRoutePlan& s) {
        return static_cast<int>(s.visits.size());
    }

    static std::vector<int>& list_of(vrp::VehicleRoutePlan& s, int listIndex) {
        return listIndex == unassigned_list(s) ? s.unassignedVisitIndices
                                               : s.vehicles[listIndex].visitIndices;
    }
    static const std::vector<int>& list_of(const vrp::VehicleRoutePlan& s, int listIndex) {
        return listIndex == unassigned_list(s) ? s.unassignedVisitIndices
                                               : s.vehicles[listIndex].visitIndices;
    }

    static int list_size(const vrp::VehicleRoutePlan& s, int listIndex) {
        return static_cast<int>(list_of(s, listIndex).size());
    }
    static int element_at(const vrp::VehicleRoutePlan& s, int listIndex, int position) {
        return list_of(s, listIndex)[static_cast<std::size_t>(position)];
    }
    static void insert_element(vrp::VehicleRoutePlan& s, int listIndex, int position, int element) {
        std::vector<int>& list = list_of(s, listIndex);
        list.insert(list.begin() + position, element);
    }
    static int remove_element(vrp::VehicleRoutePlan& s, int listIndex, int position) {
        std::vector<int>& list = list_of(s, listIndex);
        const int element = list[static_cast<std::size_t>(position)];
        list.erase(list.begin() + position);
        return element;
    }
    static void reverse_segment(vrp::VehicleRoutePlan& s, int listIndex, int first, int last) {
        std::vector<int>& list = list_of(s, listIndex);
        std::reverse(list.begin() + first, list.begin() + last + 1);
    }
};

// ---------------------------------------------------------------------------
// Medium fold registration
// ---------------------------------------------------------------------------
// The decomposed calculator folds its medium accumulator with the domain's
// kMediumFold (see the Score adapter above).
template <>
struct optimaru::MediumFoldOf<vrp::VehicleRoutePlan> {
    static constexpr optimaru::MediumFold value = vrp::kMediumFold;
};

#endif  // VRP_DOMAIN_HPP
