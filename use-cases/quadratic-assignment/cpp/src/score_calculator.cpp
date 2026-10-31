#include "qap/score_calculator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "qap/constraints/assignment_cost.hpp"
#include "qap/constraints/flow_distance_cost.hpp"
#include "qap/constraints/forbidden_assignments_cost.hpp"
#include "qap/constraints/location_capacity_cost.hpp"
#include "qap/constraints/pinned_assignments_cost.hpp"

namespace qap
{

namespace
{

// The monolithic score is the default assembly, so it reads the same
// constants the components carry. Integral, so both levels are computed in
// int64 exactly.
constexpr auto kPinnedWeight = static_cast<std::int64_t>(PinnedAssignmentsCost::kDefaultWeight);
constexpr auto kForbiddenWeight = static_cast<std::int64_t>(ForbiddenAssignmentsCost::kDefaultWeight);
constexpr auto kCapacityWeight = static_cast<std::int64_t>(LocationCapacityCost::kDefaultWeight);
constexpr auto kAssignmentWeight = static_cast<std::int64_t>(AssignmentCost::kDefaultWeight);
constexpr auto kFlowWeight = static_cast<std::int64_t>(FlowDistanceCost::kDefaultWeight);
static_assert(kPinnedWeight == PinnedAssignmentsCost::kDefaultWeight
                  && kForbiddenWeight == ForbiddenAssignmentsCost::kDefaultWeight
                  && kCapacityWeight == LocationCapacityCost::kDefaultWeight
                  && kAssignmentWeight == AssignmentCost::kDefaultWeight
                  && kFlowWeight == FlowDistanceCost::kDefaultWeight,
              "the monolithic calculator assumes integral default weights");
static_assert(PinnedAssignmentsCost::kDefaultLevel == optimaru::Level::Hard
                  && ForbiddenAssignmentsCost::kDefaultLevel == optimaru::Level::Hard
                  && LocationCapacityCost::kDefaultLevel == optimaru::Level::Hard
                  && AssignmentCost::kDefaultLevel == optimaru::Level::Soft
                  && FlowDistanceCost::kDefaultLevel == optimaru::Level::Soft,
              "the monolithic calculator assumes the default levels");

void requireSquare(const std::vector<std::vector<std::int64_t>> &matrix, int n, const char *what)
{
    if (static_cast<int>(matrix.size()) != n)
    {
        throw std::invalid_argument(std::string(what) + " must have " + std::to_string(n) + " rows, not "
                                    + std::to_string(matrix.size()));
    }
    for (const auto &row : matrix)
    {
        if (static_cast<int>(row.size()) != n)
        {
            throw std::invalid_argument(std::string(what) + " must be a " + std::to_string(n) + " x "
                                        + std::to_string(n) + " matrix");
        }
    }
}

void requireIndex(int index, int n, const std::string &what)
{
    if (index < 0 || index >= n)
        throw std::invalid_argument(what + " index " + std::to_string(index) + " out of range");
}

std::int64_t maxAbs(const std::vector<std::vector<std::int64_t>> &matrix)
{
    std::int64_t best = 0;
    for (const auto &row : matrix)
    {
        for (std::int64_t value : row)
        {
            if (value == std::numeric_limits<std::int64_t>::min())
                throw std::overflow_error("matrix entries must fit in int64 with their negation");
            best = std::max(best, value < 0 ? -value : value);
        }
    }
    return best;
}

// The worst-case magnitudes of the two objectives must stay inside int64:
// sum |flow| * max |distance| and sum over facilities of max |direct cost|.
// Python ints are unbounded; the C++ totals are not, so refuse what could
// wrap instead of scoring it wrong.
void requireNoOverflow(const QuadraticAssignmentProblem &problem)
{
    const long double limit = static_cast<long double>(std::numeric_limits<std::int64_t>::max()) / 4;
    const long double maxDistance = static_cast<long double>(maxAbs(problem.distances));
    long double bound = 0;
    for (const auto &row : problem.flows)
    {
        for (std::int64_t value : row)
            bound += std::fabs(static_cast<long double>(value)) * maxDistance;
    }
    if (problem.assignmentCosts)
    {
        maxAbs(*problem.assignmentCosts);
        for (const auto &row : *problem.assignmentCosts)
        {
            long double rowMax = 0;
            for (std::int64_t value : row)
                rowMax = std::max(rowMax, std::fabs(static_cast<long double>(value)));
            bound += rowMax;
        }
    }
    if (bound > limit)
    {
        throw std::overflow_error("flows * distances and assignment_costs are too large to score in int64");
    }
}

}  // namespace

void ScoreCalculator::prepare(QuadraticAssignmentProblem &problem)
{
    if (problem.prepared)
        return;

    const int n = problem.facilityCount();
    if (problem.locationCount() != n)
    {
        throw std::invalid_argument("facilities and locations must have the same size (" + std::to_string(n)
                                    + " != " + std::to_string(problem.locationCount()) + ")");
    }
    requireSquare(problem.flows, n, "flows");
    requireSquare(problem.distances, n, "distances");
    if (problem.assignmentCosts)
        requireSquare(*problem.assignmentCosts, n, "assignment_costs");
    requireNoOverflow(problem);

    for (const Facility &facility : problem.facilities)
    {
        if (facility.demand && *facility.demand < 0)
            throw std::invalid_argument("facility '" + facility.id + "': demand must be non-negative");
        if (facility.location < -1 || facility.location >= n)
        {
            throw std::invalid_argument("facility '" + facility.id + "': location index "
                                        + std::to_string(facility.location) + " out of range");
        }
    }
    for (const Location &location : problem.locations)
    {
        if (location.capacity && *location.capacity < 0)
            throw std::invalid_argument("location '" + location.id + "': capacity must be non-negative");
    }

    problem.n = n;
    const std::size_t cells = static_cast<std::size_t>(n) * n;
    problem.flow.assign(cells, 0);
    problem.distance.assign(cells, 0);
    problem.direct.assign(cells, 0);
    for (int a = 0; a < n; ++a)
    {
        for (int b = 0; b < n; ++b)
        {
            const std::size_t cell = static_cast<std::size_t>(a) * n + b;
            problem.flow[cell] = problem.flows[a][b];
            problem.distance[cell] = problem.distances[a][b];
            if (problem.assignmentCosts)
                problem.direct[cell] = (*problem.assignmentCosts)[a][b];
        }
    }

    problem.demandOf.assign(static_cast<std::size_t>(n), 0);
    for (int f = 0; f < n; ++f)
        problem.demandOf[f] = problem.facilities[f].demand.value_or(0);
    problem.hasCapacity.assign(static_cast<std::size_t>(n), 0);
    problem.capacityOf.assign(static_cast<std::size_t>(n), 0);
    problem.capacitatedLocations.clear();
    for (int l = 0; l < n; ++l)
    {
        if (problem.locations[l].capacity)
        {
            problem.hasCapacity[l] = 1;
            problem.capacityOf[l] = *problem.locations[l].capacity;
            problem.capacitatedLocations.push_back(l);
        }
    }

    problem.fixedFacilityOf.assign(static_cast<std::size_t>(n), -1);
    problem.fixedLocationsOf.assign(static_cast<std::size_t>(n), {});
    problem.fixedLocations.clear();
    for (const FixedAssignment &fixed : problem.fixedAssignments)
    {
        requireIndex(fixed.location, n, "fixed assignment location");
        requireIndex(fixed.facility, n, "fixed assignment facility");
        if (problem.fixedFacilityOf[fixed.location] >= 0)
        {
            throw std::invalid_argument("location '" + problem.locations[fixed.location].id
                                        + "' is fixed more than once");
        }
        problem.fixedFacilityOf[fixed.location] = fixed.facility;
        problem.fixedLocationsOf[fixed.facility].push_back(fixed.location);
    }
    for (int l = 0; l < n; ++l)
    {
        if (problem.fixedFacilityOf[l] >= 0)
            problem.fixedLocations.push_back(l);
    }

    // The rows are a set: a pair listed twice is forbidden once.
    problem.forbidden.assign(cells, 0);
    std::vector<char> involved(static_cast<std::size_t>(n), 0);
    for (const ForbiddenAssignment &row : problem.forbiddenAssignments)
    {
        requireIndex(row.facility, n, "forbidden assignment facility");
        requireIndex(row.location, n, "forbidden assignment location");
        problem.forbidden[static_cast<std::size_t>(row.facility) * n + row.location] = 1;
        involved[row.location] = 1;
    }
    problem.forbiddenLocations.clear();
    for (int l = 0; l < n; ++l)
    {
        if (involved[l])
            problem.forbiddenLocations.push_back(l);
    }

    problem.prepared = true;
}

ScoreCalculator::RawCosts ScoreCalculator::rawCosts(QuadraticAssignmentProblem &problem)
{
    prepare(problem);
    const int n = problem.n;
    RawCosts raw;

    // hard 1: pinnedAssignmentsCost — per fixed location, its facility elsewhere.
    for (int l : problem.fixedLocations)
    {
        if (problem.facilities[problem.fixedFacilityOf[l]].location != l)
            ++raw.pinnedAssignmentsCost;
    }

    std::vector<std::int64_t> load(static_cast<std::size_t>(n), 0);
    for (int f = 0; f < n; ++f)
    {
        const int l = problem.facilities[f].location;
        if (l < 0)
            continue;  // unassigned: contributes nothing
        // hard 2: forbiddenAssignmentsCost.
        if (problem.isForbidden(f, l))
            ++raw.forbiddenAssignmentsCost;
        load[l] += problem.demandOf[f];
        // soft 4: assignmentCost.
        raw.assignmentCost += problem.directCostOf(f, l);
        // soft 5: flowDistanceCost (diagonal included).
        for (int g = 0; g < n; ++g)
        {
            const int m = problem.facilities[g].location;
            if (m >= 0)
                raw.flowDistanceCost += problem.flowOf(f, g) * problem.distanceOf(l, m);
        }
    }
    // hard 3: locationCapacityCost — capacitated locations only.
    for (int l : problem.capacitatedLocations)
        raw.locationCapacityCost += std::max<std::int64_t>(0, load[l] - problem.capacityOf[l]);
    return raw;
}

Score ScoreCalculator::calculateScore(QuadraticAssignmentProblem &problem)
{
    const RawCosts raw = rawCosts(problem);
    Score score;
    score.hard = -(kPinnedWeight * raw.pinnedAssignmentsCost + kForbiddenWeight * raw.forbiddenAssignmentsCost
                   + kCapacityWeight * raw.locationCapacityCost);
    // Negate in integer space so a zero penalty prints as 0, not -0.
    score.soft = static_cast<double>(-(kAssignmentWeight * raw.assignmentCost + kFlowWeight * raw.flowDistanceCost));
    problem.score = score;
    return score;
}

}  // namespace qap
