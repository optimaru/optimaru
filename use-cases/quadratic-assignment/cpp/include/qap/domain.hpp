#ifndef QAP_DOMAIN_HPP
#define QAP_DOMAIN_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace qap
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// The engine's HardSoftScore, so the mapping is the identity and there is no
// medium level (no optimaru::MediumFoldOf specialization). At the default
// weights (every component's kDefaultWeight is 1):
//   hard = -(pinned locations not holding their facility
//            + locations holding a facility forbidden there
//            + demand above each location's capacity)
//   soft = -(direct assignment cost + flow-distance cost)
// Both levels are integral; the engine stores soft in a double, which is
// exact below 2^53.
using Score = optimaru::HardSoftScore;

// ---------------------------------------------------------------------------
// Facility  (@PlanningEntity)
// ---------------------------------------------------------------------------
// `location` is the single BASIC planning variable: the index into
// QuadraticAssignmentProblem::locations, -1 == unassigned. The solver keeps
// the facilities a permutation of the locations (swap moves only), so -1
// appears only in a partial state a caller wrote.
struct Facility
{
    std::string id;
    // facility_demands[id]; nullopt = not listed (scored as demand 0).
    std::optional<std::int64_t> demand;

    int location = -1;  // planning variable; -1 == unassigned

    bool isAssigned() const { return location >= 0; }
};

// ---------------------------------------------------------------------------
// Location  (problem fact; the value range)
// ---------------------------------------------------------------------------
struct Location
{
    std::string id;
    // location_capacities[id]; nullopt = no capacity limit.
    std::optional<std::int64_t> capacity;
};

// One fixed_assignments entry: `location` must hold `facility`. Several
// locations may name the same facility (then at least one is violated); a
// location is fixed at most once. Kept in input order (round trip).
struct FixedAssignment
{
    int location = 0;
    int facility = 0;
};

// One forbidden_assignments row: `facility` must not stand at `location`.
// Repeated rows are kept as given (round trip); scoring counts a pair once.
struct ForbiddenAssignment
{
    int facility = 0;
    int location = 0;
};

// ---------------------------------------------------------------------------
// QuadraticAssignmentProblem  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct QuadraticAssignmentProblem
{
    std::vector<Facility> facilities;
    std::vector<Location> locations;
    // n x n, facility by facility.
    std::vector<std::vector<std::int64_t>> flows;
    // n x n, location by location.
    std::vector<std::vector<std::int64_t>> distances;
    // n x n, facility by location; nullopt = no direct assignment cost.
    std::optional<std::vector<std::vector<std::int64_t>>> assignmentCosts;
    std::vector<FixedAssignment> fixedAssignments;
    std::vector<ForbiddenAssignment> forbiddenAssignments;

    // No constraint weights here: the defaults are the components'
    // kDefaultWeight (qap/constraints/*.hpp); re-weight through a constraint
    // assembly.

    // Start the next solve from the current assignment, which must then be a
    // full bijection; without it the solve starts from the identity (facility
    // i at location i). A seed is used once: the solve resets it to false.
    bool warmStart = false;

    // The score last computed on this problem: nullopt until it is scored
    // (ScoreCalculator::calculateScore or a solve), then the result of that
    // call. Not reset by later writes to the assignment or the facts.
    std::optional<Score> score;

    // --- prepared caches (ScoreCalculator::prepare; guarded by `prepared`).
    // Any write to a fact must reset `prepared` (the Python bindings do).
    bool prepared = false;
    int n = 0;
    std::vector<std::int64_t> flow;      // f * n + g
    std::vector<std::int64_t> distance;  // l * n + m
    std::vector<std::int64_t> direct;    // f * n + l (all 0 without a matrix)
    std::vector<std::int64_t> demandOf;  // per facility (0 when not listed)
    std::vector<char> hasCapacity;       // per location
    std::vector<std::int64_t> capacityOf;
    std::vector<int> fixedFacilityOf;                 // per location, -1 = not fixed
    std::vector<std::vector<int>> fixedLocationsOf;   // per facility
    std::vector<char> forbidden;                      // f * n + l, deduplicated
    // The locations each unit constraint can be non-zero on (scopedUnits()).
    std::vector<int> fixedLocations;
    std::vector<int> forbiddenLocations;
    std::vector<int> capacitatedLocations;

    int size() const { return static_cast<int>(facilities.size()); }
    int facilityCount() const { return static_cast<int>(facilities.size()); }
    int locationCount() const { return static_cast<int>(locations.size()); }

    // Prepared reads (require ScoreCalculator::prepare).
    std::int64_t flowOf(int a, int b) const
    {
        return flow[static_cast<std::size_t>(a) * n + b];
    }
    std::int64_t distanceOf(int i, int j) const
    {
        return distance[static_cast<std::size_t>(i) * n + j];
    }
    std::int64_t directCostOf(int f, int l) const
    {
        return direct[static_cast<std::size_t>(f) * n + l];
    }
    bool isForbidden(int f, int l) const
    {
        return forbidden[static_cast<std::size_t>(f) * n + l] != 0;
    }

    // Per location: the index of the facility standing there, -1 for an
    // empty location (the model's `assignment`, by index). With two
    // facilities on one location (a state only a direct write can make) the
    // lower facility index is reported.
    std::vector<int> assignment() const
    {
        std::vector<int> out(locations.size(), -1);
        for (int f = facilityCount() - 1; f >= 0; --f)
        {
            const int l = facilities[f].location;
            if (l >= 0 && l < locationCount())
                out[l] = f;
        }
        return out;
    }

    // Every facility at its own location and every location used once.
    bool isBijection() const
    {
        if (facilities.size() != locations.size())
            return false;
        std::vector<char> used(locations.size(), 0);
        for (const Facility &facility : facilities)
        {
            const int l = facility.location;
            if (l < 0 || l >= locationCount() || used[l])
                return false;
            used[l] = 1;
        }
        return true;
    }

    // --- raw objective totals, straight from the facts (no prepare needed);
    // unassigned facilities contribute nothing.

    // sum over assigned facilities a, b of flows[a][b] * distances[loc a][loc b]
    // (a == b included: the diagonal term flows[a][a] * distances[l][l]).
    std::int64_t flowDistanceCost() const
    {
        std::int64_t total = 0;
        for (int a = 0; a < facilityCount(); ++a)
        {
            const int la = facilities[a].location;
            if (la < 0)
                continue;
            for (int b = 0; b < facilityCount(); ++b)
            {
                const int lb = facilities[b].location;
                if (lb >= 0)
                    total += flows[a][b] * distances[la][lb];
            }
        }
        return total;
    }

    // sum over assigned facilities f of assignmentCosts[f][loc f]; 0 without
    // a matrix.
    std::int64_t assignmentCost() const
    {
        if (!assignmentCosts)
            return 0;
        std::int64_t total = 0;
        for (int f = 0; f < facilityCount(); ++f)
        {
            const int l = facilities[f].location;
            if (l >= 0)
                total += (*assignmentCosts)[f][l];
        }
        return total;
    }

    // The model's total_cost: flow-distance plus direct assignment cost, raw
    // (whatever constraints are selected).
    std::int64_t totalCost() const { return flowDistanceCost() + assignmentCost(); }
};

}  // namespace qap

// ---------------------------------------------------------------------------
// SolutionDescriptor specialization: entities are facilities, the planning
// variable is Facility::location, the value range is the location list. A
// DecomposedScoreCalculator unit is therefore one LOCATION, its members the
// facilities standing there (exactly one in a bijection).
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionDescriptor<qap::QuadraticAssignmentProblem>
{
    static constexpr optimaru::VariableKind kind = optimaru::VariableKind::BASIC;
    static int entityCount(const qap::QuadraticAssignmentProblem &problem)
    {
        return problem.facilityCount();
    }
    static int valueCount(const qap::QuadraticAssignmentProblem &problem)
    {
        return problem.locationCount();
    }
    static int getVariable(const qap::QuadraticAssignmentProblem &problem, int facilityIndex)
    {
        return problem.facilities[facilityIndex].location;
    }
    static void setVariable(qap::QuadraticAssignmentProblem &problem, int facilityIndex, int locationIndex)
    {
        problem.facilities[facilityIndex].location = locationIndex;
    }
};

#endif  // QAP_DOMAIN_HPP
