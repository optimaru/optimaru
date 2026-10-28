#ifndef FL_DOMAIN_HPP
#define FL_DOMAIN_HPP

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace fl
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The quickstart scores with HardSoftScore, exactly the engine's score type,
// so the mapping is the identity:
//   hard = -(sum over used facilities of max(0, usedDemand - capacity))
//   soft = -(setupCostWeight * sum of used facilities' setup cost
//            + distanceFromFacilityWeight * sum of consumer->facility meters)
// Both levels are integral (Java uses long); the engine stores soft in a
// double, which is exact for these magnitudes (< 2^53).
using Score = optimaru::HardSoftScore;

// ---------------------------------------------------------------------------
// Location  (problem fact)
// ---------------------------------------------------------------------------
// Straight port of org.acme.facilitylocation.domain.Location: latitude and
// longitude in degrees, distance approximated with 1 degree = 111 km.
struct Location
{
    // Approximate metric equivalent of one degree (valid for latitude
    // anywhere and for longitude at the equator): 1 deg = 111 km.
    static constexpr double kMetersPerDegree = 111000.0;

    double latitude = 0.0;
    double longitude = 0.0;

    // Distance in meters, Java's getDistanceTo: ceil of the Euclidean
    // degree-distance scaled to meters.
    std::int64_t distanceTo(const Location &other) const
    {
        const double latitudeDiff = other.latitude - latitude;
        const double longitudeDiff = other.longitude - longitude;
        return static_cast<std::int64_t>(
            std::ceil(std::sqrt(latitudeDiff * latitudeDiff
                                + longitudeDiff * longitudeDiff)
                      * kMetersPerDegree));
    }
};

// ---------------------------------------------------------------------------
// Facility  (problem fact)
// ---------------------------------------------------------------------------
// In the quickstart Facility is a shadow planning entity whose `consumers`
// inverse-relation list feeds the capacity/setup-cost constraints. Here the
// aggregates (used demand, consumer count) live in the score calculators,
// so the C++ Facility is a plain fact.
struct Facility
{
    std::string id;
    Location location;
    std::int64_t setupCost = 0;
    std::int64_t capacity = 0;
};

// ---------------------------------------------------------------------------
// Consumer  (@PlanningEntity)
// ---------------------------------------------------------------------------
// `facility` is the single BASIC planning variable: the index into
// FacilityLocationProblem::facilities, -1 == unassigned (Java null).
struct Consumer
{
    std::string id;
    Location location;
    std::int64_t demand = 0;

    int facility = -1;  // planning variable; -1 == unassigned

    bool isAssigned() const { return facility >= 0; }
};

// ---------------------------------------------------------------------------
// FacilityLocationProblem  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct FacilityLocationProblem
{
    std::vector<Facility> facilities;
    std::vector<Consumer> consumers;

    // Bounding box (kept for parity with the Java solution's getBounds();
    // not used by any constraint).
    Location southWestCorner{-90.0, -180.0};
    Location northEastCorner{90.0, 180.0};

    // Constraint weights. setupCostWeight mirrors the provider's fixed
    // HardSoftScore.ofSoft(2). distanceFromFacilityWeight defaults to 6
    // because the Java solution demonstrates ConstraintWeightOverrides:
    // the provider declares ofSoft(5), but every FacilityLocationProblem
    // carries an override map {"distance from facility": ofSoft(6)} — so 6
    // is the effective weight of the quickstart.
    std::int64_t setupCostWeight = 2;
    std::int64_t distanceFromFacilityWeight = 6;

    Score score;

    // Set once by ScoreCalculator::prepare; guards against rebuilding the
    // distance matrix on every calculateScore call.
    bool prepared = false;

    // Filled by ScoreCalculator::prepare: consumer->facility meters,
    // flattened as consumerIndex * facilityCount() + facilityIndex.
    std::vector<std::int64_t> distanceMeters;

    int facilityCount() const { return static_cast<int>(facilities.size()); }
    int consumerCount() const { return static_cast<int>(consumers.size()); }

    std::int64_t distanceOf(int consumerIndex, int facilityIndex) const
    {
        return distanceMeters[static_cast<std::size_t>(consumerIndex)
                                  * facilityCount()
                              + facilityIndex];
    }

    // --- convenience mirrors of the Java solution's summary getters ---

    // Sum of the setup costs of facilities serving at least one consumer
    // (Java getTotalCost).
    std::int64_t totalCost() const
    {
        std::vector<char> used(facilities.size(), 0);
        for (const Consumer &consumer : consumers)
        {
            if (consumer.isAssigned())
                used[consumer.facility] = 1;
        }
        std::int64_t cost = 0;
        for (std::size_t f = 0; f < facilities.size(); ++f)
        {
            if (used[f])
                cost += facilities[f].setupCost;
        }
        return cost;
    }

    // Sum of every facility's setup cost (Java getPotentialCost).
    std::int64_t potentialCost() const
    {
        std::int64_t cost = 0;
        for (const Facility &facility : facilities)
            cost += facility.setupCost;
        return cost;
    }

    // Total consumer->facility distance in meters (Java getTotalDistance
    // reports this divided by 1000 as km).
    std::int64_t totalDistanceMeters() const
    {
        std::int64_t distance = 0;
        for (const Consumer &consumer : consumers)
        {
            if (consumer.isAssigned())
            {
                distance += facilities[consumer.facility].location.distanceTo(
                    consumer.location);
            }
        }
        return distance;
    }

    int usedFacilityCount() const
    {
        std::vector<char> used(facilities.size(), 0);
        for (const Consumer &consumer : consumers)
        {
            if (consumer.isAssigned())
                used[consumer.facility] = 1;
        }
        int count = 0;
        for (char u : used)
            count += u;
        return count;
    }
};

}  // namespace fl

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization  (the @PlanningEntity/@PlanningVariable
// annotations of the C++ engine): entities are consumers, the planning
// variable is Consumer::facility, the value range is the facility list
// (@ValueRangeProvider on FacilityLocationProblem::facilities).
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<fl::FacilityLocationProblem>
{
    static constexpr optimaru::VariableKind kind
        = optimaru::VariableKind::BASIC;
    static int entityCount(const fl::FacilityLocationProblem &problem)
    {
        return problem.consumerCount();
    }
    static int valueCount(const fl::FacilityLocationProblem &problem)
    {
        return problem.facilityCount();
    }
    static int getVariable(const fl::FacilityLocationProblem &problem,
                           int consumerIndex)
    {
        return problem.consumers[consumerIndex].facility;
    }
    static void setVariable(fl::FacilityLocationProblem &problem,
                            int consumerIndex,
                            int facilityIndex)
    {
        problem.consumers[consumerIndex].facility = facilityIndex;
    }
};

#endif  // FL_DOMAIN_HPP
