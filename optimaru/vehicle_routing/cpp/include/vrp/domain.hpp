#ifndef VRP_DOMAIN_HPP
#define VRP_DOMAIN_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/levels.hpp"

namespace vrp
{

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// Every timestamp in a single problem falls on the same day ("tomorrow at
// HH:MM"), so time is an int64 number of seconds from a common reference
// (start of that day). All arithmetic (arrival = departure + drivingSeconds,
// etc.) is then plain integer math.
using Seconds = std::int64_t;

constexpr Seconds NULL_TIME = -1;  // no time (e.g. an unassigned visit)

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// A three-level hard / medium / soft score.
// Constraints *penalize*, so scores are <= 0; higher (closer to zero) is
// better. Comparison is lexicographic: hard dominates medium dominates soft.
//
// The generic engine (optimaru/cpp/optimaru/) only knows Score(int64 hard, double
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
constexpr optimaru::MediumFold kMediumFold
    = optimaru::MediumFold::into_soft(static_cast<double>(kMediumUnit));

struct Score
{
    std::int64_t hard = 0;
    std::int64_t medium = 0;
    std::int64_t soft = 0;

    bool feasible() const { return hard >= 0; }

    // Lexicographic ordering. a < b means a is a *worse* solution than b.
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

    std::string str() const
    {
        return std::to_string(hard) + "hard/" + std::to_string(medium)
               + "medium/" + std::to_string(soft) + "soft";
    }
};

inline optimaru::Score toEngineScore(const Score &s)
{
    // soft + medium * 10^9: exact for the integral levels of this domain
    // (|engine soft| < 2^53), i.e. the old `medium * kMediumUnit + soft`.
    return kMediumFold.to_engine(s.hard,
                                static_cast<double>(s.medium),
                                static_cast<double>(s.soft));
}

// A best-score LIMIT (Solver::Config::target_score) in engine terms.
// kNoLevelFloor (INT64_MIN) on the medium or the soft level means "no floor
// on the soft side": the engine's soft limit becomes -inf, so only the hard
// level is compared and no soft score, however low, can miss the stop.
// stop_at_first_feasible passes (0, kNoLevelFloor, kNoLevelFloor). Through
// toEngineScore the sentinel would fold into a finite engine soft that a
// low enough soft score never reaches. Any other limit folds as a score.
constexpr std::int64_t kNoLevelFloor = std::numeric_limits<std::int64_t>::min();

inline optimaru::Score toEngineScoreLimit(const Score &limit)
{
    if (limit.medium == kNoLevelFloor || limit.soft == kNoLevelFloor)
        return {limit.hard, -std::numeric_limits<double>::infinity()};
    return toEngineScore(limit);
}

// Inverse of toEngineScore. Every constraint penalizes (medium and soft are
// <= 0 and integral), so C++'s truncating division — which rounds toward zero
// on both — decodes the two levels exactly.
inline Score fromEngineScore(const optimaru::Score &e)
{
    const auto combined = optimaru::round_score_level(e.soft, "soft");
    return {e.hard, combined / kMediumUnit, combined % kMediumUnit};
}

// ---------------------------------------------------------------------------
// Location
// ---------------------------------------------------------------------------
// Every distinct point (vehicle home + each visit) is registered once and
// referenced by index into VehicleRoutePlan::drivingSeconds, the precomputed
// driving-time matrix.
struct Location
{
    double latitude = 0.0;
    double longitude = 0.0;
    int index = -1;  // row/column into the driving-time matrix
};

// ---------------------------------------------------------------------------
// Visit  (the list element)
// ---------------------------------------------------------------------------
// The input fields are immutable; the derived fields (vehicleIndex,
// arrivalTime) are recomputed from the vehicle routes.
struct Visit
{
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
    std::int64_t unassignedPenalty() const
    {
        return unassignedPenaltyMinutes == kUnassignedPenaltyAuto
                   ? serviceDuration / 60
                   : unassignedPenaltyMinutes;
    }

    // --- derived (recomputed by ScoreCalculator::updateShadowVariables) ---
    int vehicleIndex = -1;  // -1 == unassigned
    Seconds arrivalTime = NULL_TIME;
};

// ---------------------------------------------------------------------------
// Vehicle  (the list owner)
// ---------------------------------------------------------------------------
// visitIndices is the decision: the ordered list of visits this vehicle
// services. A visit may appear in no vehicle's list at all (see
// VehicleRoutePlan::unassignedVisitIndices).
struct Vehicle
{
    std::string id;
    int capacity = 0;
    int homeLocationIndex = -1;
    Seconds departureTime = 0;
    // Optional route policies. Non-positive means disabled.
    Seconds maxTravelTime = 0;
    Seconds maxRouteDuration = 0;
    Seconds maxEndTime = 0;
    int maxStops = 0;
    double minLoadRatio = 0.0;
    double fixedCost = 0.0;
    double maxRouteRadiusMeters = 0.0;
    // Does the route end back at home? Off by default: the route is open and
    // ends at its last visit, so the last-visit -> home leg counts toward
    // neither driving time nor the route end time. On, the leg is charged
    // (a round trip).
    bool returnToDepot = false;
    std::vector<int> visitIndices;  // ordered route

    // The driving seconds from the last stop back home, or 0 for an open
    // route. Every scoring path charges the closing leg through this one
    // helper so the monolithic, incremental and plugin costs cannot drift.
    Seconds homeLeg(const std::vector<std::vector<Seconds>> &drivingSeconds,
                    int lastLocation) const
    {
        return returnToDepot ? drivingSeconds[static_cast<std::size_t>(lastLocation)]
                                             [static_cast<std::size_t>(homeLocationIndex)]
                             : 0;
    }
};

// ---------------------------------------------------------------------------
// VehicleRoutePlan  (the solution)
// ---------------------------------------------------------------------------
struct VehicleRoutePlan
{
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

    // Precomputed driving time in seconds between every pair of locations,
    // indexed by Location::index. Filled by HaversineDrivingTimeCalculator.
    std::vector<std::vector<Seconds>> drivingSeconds;

    Score score;

    Seconds drivingTime(int fromLocation, int toLocation) const
    {
        return drivingSeconds[fromLocation][toLocation];
    }

    // Puts every visit that no vehicle services into the unassigned pool, so
    // the plan satisfies the engine's "every element is in exactly one list"
    // invariant. Idempotent.
    void rebuildUnassignedPool()
    {
        std::vector<char> routed(visits.size(), 0);
        for (const Vehicle &vehicle : vehicles)
        {
            for (int visitIndex : vehicle.visitIndices)
                routed[static_cast<std::size_t>(visitIndex)] = 1;
        }
        unassignedVisitIndices.clear();
        for (std::size_t i = 0; i < visits.size(); ++i)
        {
            if (!routed[i])
                unassignedVisitIndices.push_back(static_cast<int>(i));
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
template <> struct optimaru::SolutionTraits<vrp::VehicleRoutePlan>
{
    static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;

    // The index of the virtual unassigned-pool list.
    static int unassigned_list(const vrp::VehicleRoutePlan &s)
    {
        return static_cast<int>(s.vehicles.size());
    }

    static int list_count(const vrp::VehicleRoutePlan &s)
    {
        return static_cast<int>(s.vehicles.size()) + 1;
    }
    static int element_count(const vrp::VehicleRoutePlan &s)
    {
        return static_cast<int>(s.visits.size());
    }

    static std::vector<int> &list_of(vrp::VehicleRoutePlan &s, int listIndex)
    {
        return listIndex == unassigned_list(s) ? s.unassignedVisitIndices
                                              : s.vehicles[listIndex].visitIndices;
    }
    static const std::vector<int> &list_of(const vrp::VehicleRoutePlan &s,
                                          int listIndex)
    {
        return listIndex == unassigned_list(s) ? s.unassignedVisitIndices
                                              : s.vehicles[listIndex].visitIndices;
    }

    static int list_size(const vrp::VehicleRoutePlan &s, int listIndex)
    {
        return static_cast<int>(list_of(s, listIndex).size());
    }
    static int
    element_at(const vrp::VehicleRoutePlan &s, int listIndex, int position)
    {
        return list_of(s, listIndex)[static_cast<std::size_t>(position)];
    }
    static void insert_element(vrp::VehicleRoutePlan &s,
                              int listIndex,
                              int position,
                              int element)
    {
        std::vector<int> &list = list_of(s, listIndex);
        list.insert(list.begin() + position, element);
    }
    static int
    remove_element(vrp::VehicleRoutePlan &s, int listIndex, int position)
    {
        std::vector<int> &list = list_of(s, listIndex);
        const int element = list[static_cast<std::size_t>(position)];
        list.erase(list.begin() + position);
        return element;
    }
    static void reverse_segment(vrp::VehicleRoutePlan &s,
                               int listIndex,
                               int first,
                               int last)
    {
        std::vector<int> &list = list_of(s, listIndex);
        std::reverse(list.begin() + first, list.begin() + last + 1);
    }
};

// ---------------------------------------------------------------------------
// Medium fold registration
// ---------------------------------------------------------------------------
// The decomposed calculator folds its medium accumulator with the domain's
// kMediumFold (see the Score adapter above).
template <> struct optimaru::MediumFoldOf<vrp::VehicleRoutePlan>
{
    static constexpr optimaru::MediumFold value = vrp::kMediumFold;
};

#endif  // VRP_DOMAIN_HPP
