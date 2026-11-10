#ifndef FL_DOMAIN_HPP
#define FL_DOMAIN_HPP

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"

namespace fl
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The domain scores with the engine's hard/soft Score directly, so there is
// no mapping and no medium level (no
// optimaru::MediumFoldOf specialization; a domain WITH a medium level declares
// its fold next to its Score adapter, see optimaru/api/score/level.hpp):
//   hard = -(sum over used facilities of max(0, usedDemand - capacity))
//   soft = -(2 * sum of used facilities' setup cost
//            + 6 * sum of consumer->facility meters)
// The weights are the components' kDefaultWeight (fl/constraints/*.hpp); the
// problem carries none. Both levels are integral; the engine
// stores soft in a double, which is exact for these magnitudes (< 2^53).
using Score = optimaru::Score;

// ---------------------------------------------------------------------------
// Location  (problem fact)
// ---------------------------------------------------------------------------
// Latitude and longitude in degrees, distance approximated with 1 degree = 111 km.
struct Location
{
    // Approximate metric equivalent of one degree (valid for latitude
    // anywhere and for longitude at the equator): 1 deg = 111 km.
    static constexpr double kMetersPerDegree = 111000.0;

    double latitude = 0.0;
    double longitude = 0.0;

    // Distance in meters: ceil of the Euclidean degree-distance scaled to
    // meters.
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
// A plain fact: the per-facility aggregates the capacity/setup-cost
// constraints need (used demand, consumer count) live in the score
// calculators, not here.
struct Facility
{
    std::string id;
    Location location;
    std::int64_t setupCost = 0;
    std::int64_t capacity = 0;
};

// ---------------------------------------------------------------------------
// Consumer  (entity)
// ---------------------------------------------------------------------------
// `facility` is the single assignment decision variable: the index into
// FacilityLocationProblem::facilities, -1 == unassigned.
struct Consumer
{
    std::string id;
    Location location;
    std::int64_t demand = 0;

    int facility = -1;  // decision variable; -1 == unassigned

    bool isAssigned() const { return facility >= 0; }
};

// ---------------------------------------------------------------------------
// FacilityLocationProblem  (solution)
// ---------------------------------------------------------------------------
struct FacilityLocationProblem
{
    std::vector<Facility> facilities;
    std::vector<Consumer> consumers;

    // Bounding box (informational; not used by any constraint).
    Location southWestCorner{-90.0, -180.0};
    Location northEastCorner{90.0, 180.0};

    // No constraint weights here: the defaults are the
    // components' kDefaultWeight (setup 2, distance 6), and a caller
    // re-weights through a constraint assembly instead.

    Score score;

    // Set once by ScoreCalculator::prepare; guards against rebuilding the
    // distance matrix on every score call.
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

    // --- summary getters ---

    // Sum of the setup costs of facilities serving at least one consumer.
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

    // Sum of every facility's setup cost.
    std::int64_t potentialCost() const
    {
        std::int64_t cost = 0;
        for (const Facility &facility : facilities)
            cost += facility.setupCost;
        return cost;
    }

    // Total consumer->facility distance in meters.
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
// SolutionTraits specialization: entities are consumers, the decision
// variable is Consumer::facility, the value range is the facility list
// (FacilityLocationProblem::facilities).
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionTraits<fl::FacilityLocationProblem>
{
    static constexpr optimaru::Shape shape
        = optimaru::Shape::Assignment;
    static int entity_count(const fl::FacilityLocationProblem &problem)
    {
        return problem.consumerCount();
    }
    static int value_count(const fl::FacilityLocationProblem &problem)
    {
        return problem.facilityCount();
    }
    static int value_of(const fl::FacilityLocationProblem &problem,
                           int consumerIndex)
    {
        return problem.consumers[consumerIndex].facility;
    }
    static void assign(fl::FacilityLocationProblem &problem,
                            int consumerIndex,
                            int facilityIndex)
    {
        problem.consumers[consumerIndex].facility = facilityIndex;
    }
};

#endif  // FL_DOMAIN_HPP
