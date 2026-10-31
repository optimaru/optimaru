#include "pdp/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pdp/incremental_calculator.hpp"
#include "pdp/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace pdp
{

namespace
{

using Descriptor = optimaru::SolutionDescriptor<PdpRoutePlan>;

// Whether a request's coupling rules currently hold, read from the engine's
// live element index (O(1), no scoring).
bool pairIntact(const PdpRoutePlan &plan,
                const optimaru::OperatorContext<PdpRoutePlan> &ctx,
                const Delivery &delivery)
{
    const int pool = Descriptor::unassignedList(plan);
    const int pickupList = ctx.elementList[delivery.pickupNodeIndex];
    const int dropoffList = ctx.elementList[delivery.dropoffNodeIndex];
    if (pickupList != dropoffList)
        return false;
    if (pickupList == pool)
        return true;  // both parked: a dropped request is coupled, just
                      // unrouted
    return ctx.elementPosition[delivery.pickupNodeIndex]
           < ctx.elementPosition[delivery.dropoffNodeIndex];
}

// ---------------------------------------------------------------------------
// PairRelocateMove
// ---------------------------------------------------------------------------
// Moves one whole request. Three tiers, in the order they matter:
//
//  1. Repair — pick a request whose pair is currently broken (split across
//     vehicles, or dropoff before pickup, or only half routed) and re-lay it
//     as a proper pair. This is what lets the search recover from the
//     element-granular built-in moves and from an LNS kick: the engine's
//     recreate places stops one at a time and has no notion of a pair, so
//     after every kick some requests come back broken.
//  2. Assignment — move a whole request to or from the unassigned pool. The
//     medium level (maximizeRequestsAssigned) is otherwise unreachable, since
//     moving a single stop into the pool only ever creates a half-assigned
//     request, which is hard-infeasible and always rejected.
//  3. Relocate — the ordinary move: take an arbitrary request and re-insert
//     it into a random vehicle at random positions with the pickup first.
//
// Positions are random rather than cheapest-insertion on purpose: this runs
// once per local-search step, so an O(route^2) scan of insertion pairs would
// cost more than the acceptor saves. The ALNS repair does the greedy work.
class PairRelocateMove final : public optimaru::MoveOperator<PdpRoutePlan>
{
public:
    PairRelocateMove(double repairProbability, double assignmentProbability)
        : repairProbability_(repairProbability),
          assignmentProbability_(assignmentProbability)
    {
    }

    const char *name() const override { return "pairRelocate"; }

    bool apply(optimaru::OperatorContext<PdpRoutePlan> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        PdpRoutePlan &plan = ctx.director.workingSolution();
        const int deliveryCount = static_cast<int>(plan.deliveries.size());
        const int vehicleCount = static_cast<int>(plan.vehicles.size());
        if (deliveryCount == 0 || vehicleCount == 0)
            return false;
        const int pool = Descriptor::unassignedList(plan);

        std::uniform_real_distribution<double> unit(0.0, 1.0);

        // Tier 1: prefer a broken request when there is one.
        int deliveryIndex = -1;
        if (unit(rng) < repairProbability_)
        {
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                const int candidate = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(deliveryCount));
                if (!pairIntact(
                        plan,
                        ctx,
                        plan.deliveries[static_cast<std::size_t>(candidate)]))
                {
                    deliveryIndex = candidate;
                    break;
                }
            }
        }
        if (deliveryIndex < 0)
        {
            deliveryIndex = static_cast<int>(
                rng() % static_cast<std::uint64_t>(deliveryCount));
        }

        const Delivery &delivery
            = plan.deliveries[static_cast<std::size_t>(deliveryIndex)];
        const int pickup = delivery.pickupNodeIndex;
        const int dropoff = delivery.dropoffNodeIndex;

        // Tier 2 vs 3: where does the request go?
        //
        // The target is chosen through an ANCHOR — a stop already routed near
        // the pickup — rather than uniformly at random, mirroring the engine's
        // nearby-biased relocate. Dropping a request next to a spatial
        // neighbour is the only version of this move with a real chance of
        // being accepted; a uniformly random vehicle and position is rejected
        // almost every time and just burns the step budget. Anchors are
        // resolved to positions AFTER the detach below, because the engine's
        // element index goes stale the moment a custom operator mutates a list.
        int target = -1;
        int pickupAnchor = -1;
        int dropoffAnchor = -1;
        int greedyPickupPos = -1;
        int greedyDropoffPos = -1;
        const bool currentlyPooled = ctx.elementList[pickup] == pool
                                     && ctx.elementList[dropoff] == pool;

        if (currentlyPooled)
        {
            // Assignment is the lexicographically important move. Random
            // insertion misses almost every feasible pickup/dropoff slot on
            // large PDPTW routes, so scan the pair slots and take the least
            // added travel among capacity/time-window-feasible candidates.
            if (!bestInsertion(
                    plan, delivery, target, greedyPickupPos, greedyDropoffPos))
                return false;
        }
        else if (unit(rng) < assignmentProbability_)
        {
            target = pool;  // drop the request
        }
        else
        {
            // A bound request only moves along its own route, which is also
            // where a repair has to put it back.
            const int required = delivery.requiredVehicleIndex;
            if (ctx.nearby.enabled())
            {
                pickupAnchor
                    = required >= 0
                          ? routedNeighborIn(ctx, plan, pickup, required, rng)
                          : routedNeighbor(ctx, plan, pickup, pool, rng);
                if (pickupAnchor >= 0)
                {
                    target = ctx.elementList[pickupAnchor];
                    // A neighbour of the dropoff sitting in the SAME route is
                    // the natural place for the second stop.
                    const int candidate
                        = routedNeighborIn(ctx, plan, dropoff, target, rng);
                    dropoffAnchor = candidate;
                }
            }
            if (target < 0)
            {
                target = required >= 0
                             ? required
                             : static_cast<int>(
                                   rng()
                                   % static_cast<std::uint64_t>(vehicleCount));
            }
        }

        // Save every list we are about to touch, for the undo.
        saved_.clear();
        save(plan, ctx.elementList[pickup]);
        save(plan, ctx.elementList[dropoff]);
        save(plan, target);

        // Detach both stops, then re-insert them together.
        removeNode(ctx, plan, pickup);
        removeNode(ctx, plan, dropoff);

        const std::vector<int> &targetList = Descriptor::listOf(plan, target);
        const int targetSize = static_cast<int>(targetList.size());
        if (target == pool)
        {
            // Order is meaningless in the pool; append both.
            ctx.director.beginListChange(target);
            Descriptor::insertElement(plan, target, targetSize, pickup);
            Descriptor::insertElement(plan, target, targetSize + 1, dropoff);
            ctx.director.endListChange(target);
        }
        else
        {
            // Positions are resolved against the post-detach list, so the
            // anchors are located by value rather than by their stale index.
            int pickupPos = greedyPickupPos >= 0
                                ? greedyPickupPos
                                : positionAfter(targetList, pickupAnchor);
            // Pinned stops stay in front: a slot among them is no slot at all.
            const int firstFree = std::min(
                static_cast<int>(plan.vehicles[static_cast<std::size_t>(target)]
                                     .pinnedNodeIndices.size()),
                targetSize);
            if (pickupPos < 0 || pickupPos < firstFree)
            {
                pickupPos = firstFree
                            + static_cast<int>(
                                rng()
                                % static_cast<std::uint64_t>(
                                    targetSize - firstFree + 1));
            }
            int dropoffPos = greedyDropoffPos >= 0
                                 ? greedyDropoffPos
                                 : positionAfter(targetList, dropoffAnchor);
            // The dropoff must land strictly after the pickup for the pair to
            // be legal; an anchor that would put it earlier is discarded.
            if (dropoffPos <= pickupPos)
            {
                dropoffPos
                    = pickupPos + 1
                      + static_cast<int>(rng()
                                         % static_cast<std::uint64_t>(
                                             targetSize - pickupPos + 1));
            }
            else if (greedyDropoffPos < 0)
            {
                // +1 because inserting the pickup first shifts everything
                // at or after pickupPos one place to the right.
                ++dropoffPos;
            }
            ctx.director.beginListChange(target);
            Descriptor::insertElement(plan, target, pickupPos, pickup);
            Descriptor::insertElement(plan, target, dropoffPos, dropoff);
            ctx.director.endListChange(target);
        }

        entities.push_back(pickup);
        entities.push_back(dropoff);
        return true;
    }

    void undoLast(optimaru::OperatorContext<PdpRoutePlan> &ctx) override
    {
        PdpRoutePlan &plan = ctx.director.workingSolution();
        for (const auto &[listIndex, contents] : saved_)
        {
            ctx.director.beginListChange(listIndex);
            Descriptor::listOf(plan, listIndex) = contents;
            ctx.director.endListChange(listIndex);
        }
        saved_.clear();
    }

    static bool findBestInsertion(const PdpRoutePlan &plan,
                                  const Delivery &delivery,
                                  int &bestVehicle,
                                  int &bestPickup,
                                  int &bestDropoff)
    {
        return bestInsertion(
            plan, delivery, bestVehicle, bestPickup, bestDropoff);
    }

private:
    static bool routeFeasible(const PdpRoutePlan &plan,
                              int vehicleIndex,
                              const std::vector<int> &route)
    {
        const Vehicle &vehicle
            = plan.vehicles[static_cast<std::size_t>(vehicleIndex)];
        std::int64_t load = 0;
        Seconds departure = vehicle.departureTime;
        int previous = vehicle.homeLocationIndex;
        for (int nodeIndex : route)
        {
            const Node &node = plan.nodes[static_cast<std::size_t>(nodeIndex)];
            const Delivery &request
                = plan.deliveries[static_cast<std::size_t>(node.deliveryIndex)];
            load += node.pickup ? request.demand : -request.demand;
            if (load > vehicle.capacity)
                return false;
            const Seconds arrival
                = departure + plan.drivingTime(previous, node.locationIndex);
            if (arrival + node.serviceDuration > node.maxEndTime)
                return false;
            departure
                = std::max(arrival, node.minStartTime) + node.serviceDuration;
            previous = node.locationIndex;
        }
        return true;
    }

    static Seconds routeTravel(const PdpRoutePlan &plan,
                               int vehicleIndex,
                               const std::vector<int> &route)
    {
        if (route.empty())
            return 0;
        const Vehicle &vehicle
            = plan.vehicles[static_cast<std::size_t>(vehicleIndex)];
        Seconds travel = 0;
        int previous = vehicle.homeLocationIndex;
        for (int nodeIndex : route)
        {
            const int location
                = plan.nodes[static_cast<std::size_t>(nodeIndex)].locationIndex;
            travel += plan.drivingTime(previous, location);
            previous = location;
        }
        return travel + vehicle.homeLeg(plan.drivingSeconds, previous);
    }

    static bool bestInsertion(const PdpRoutePlan &plan,
                              const Delivery &delivery,
                              int &bestVehicle,
                              int &bestPickup,
                              int &bestDropoff)
    {
        Seconds bestAdded = std::numeric_limits<Seconds>::max();
        std::vector<int> candidate;
        for (std::size_t vehicleIndex = 0; vehicleIndex < plan.vehicles.size();
             ++vehicleIndex)
        {
            // A bound request has no other vehicle to fall back on: with no
            // feasible slot on its own it stays unassigned.
            if (delivery.requiredVehicleIndex >= 0
                && delivery.requiredVehicleIndex
                       != static_cast<int>(vehicleIndex))
                continue;
            const std::vector<int> &route
                = plan.vehicles[vehicleIndex].nodeIndices;
            const Seconds before
                = routeTravel(plan, static_cast<int>(vehicleIndex), route);
            const auto firstFree = static_cast<int>(
                plan.vehicles[vehicleIndex].pinnedNodeIndices.size());
            for (int pickupPos = firstFree;
                 pickupPos <= static_cast<int>(route.size());
                 ++pickupPos)
            {
                for (int dropoffPos = pickupPos + 1;
                     dropoffPos <= static_cast<int>(route.size()) + 1;
                     ++dropoffPos)
                {
                    candidate = route;
                    candidate.insert(candidate.begin() + pickupPos,
                                     delivery.pickupNodeIndex);
                    candidate.insert(candidate.begin() + dropoffPos,
                                     delivery.dropoffNodeIndex);
                    if (!routeFeasible(
                            plan, static_cast<int>(vehicleIndex), candidate))
                        continue;
                    const Seconds added
                        = routeTravel(
                              plan, static_cast<int>(vehicleIndex), candidate)
                          - before;
                    if (added < bestAdded)
                    {
                        bestAdded = added;
                        bestVehicle = static_cast<int>(vehicleIndex);
                        bestPickup = pickupPos;
                        bestDropoff = dropoffPos;
                    }
                }
            }
        }
        return bestVehicle >= 0;
    }

    // A random nearby stop of `node` that is currently on some vehicle.
    static int
    routedNeighbor(const optimaru::OperatorContext<PdpRoutePlan> &ctx,
                   const PdpRoutePlan &plan,
                   int node,
                   int pool,
                   std::mt19937_64 &rng)
    {
        const std::vector<int> &neighbors = ctx.nearby.of(node);
        if (neighbors.empty())
            return -1;
        const auto start = rng() % static_cast<std::uint64_t>(neighbors.size());
        for (std::size_t i = 0; i < neighbors.size(); ++i)
        {
            const int candidate = neighbors[(start + i) % neighbors.size()];
            const int list = ctx.elementList[candidate];
            if (list < 0 || list == pool)
                continue;
            if (candidate == node || candidate == plan.partnerOf(node))
                continue;  // the pair itself is about to be detached
            return candidate;
        }
        return -1;
    }

    // The same, restricted to one route.
    static int
    routedNeighborIn(const optimaru::OperatorContext<PdpRoutePlan> &ctx,
                     const PdpRoutePlan &plan,
                     int node,
                     int list,
                     std::mt19937_64 &rng)
    {
        const std::vector<int> &neighbors = ctx.nearby.of(node);
        if (neighbors.empty())
            return -1;
        const auto start = rng() % static_cast<std::uint64_t>(neighbors.size());
        for (std::size_t i = 0; i < neighbors.size(); ++i)
        {
            const int candidate = neighbors[(start + i) % neighbors.size()];
            if (ctx.elementList[candidate] != list)
                continue;
            if (candidate == node || candidate == plan.partnerOf(node))
                continue;
            return candidate;
        }
        return -1;
    }

    // Where `anchor` currently sits in `list`, plus one — i.e. the slot right
    // after it. -1 when there is no anchor or it is no longer in the list.
    static int positionAfter(const std::vector<int> &list, int anchor)
    {
        if (anchor < 0)
            return -1;
        const auto at = std::find(list.begin(), list.end(), anchor);
        if (at == list.end())
            return -1;
        return static_cast<int>(at - list.begin()) + 1;
    }

    void save(const PdpRoutePlan &plan, int listIndex)
    {
        for (const auto &entry : saved_)
        {
            if (entry.first == listIndex)
                return;  // already saved
        }
        saved_.emplace_back(listIndex, Descriptor::listOf(plan, listIndex));
    }

    // Removes a node from whichever list holds it, through the director so the
    // calculator sees the change.
    static void removeNode(optimaru::OperatorContext<PdpRoutePlan> &ctx,
                           PdpRoutePlan &plan,
                           int node)
    {
        const int listIndex = ctx.elementList[node];
        if (listIndex < 0)
            return;
        std::vector<int> &list = Descriptor::listOf(plan, listIndex);
        const auto at = std::find(list.begin(), list.end(), node);
        if (at == list.end())
            return;  // already removed (pickup and dropoff share a list)
        ctx.director.beginListChange(listIndex);
        list.erase(at);
        ctx.director.endListChange(listIndex);
    }

    double repairProbability_;
    double assignmentProbability_;
    std::vector<std::pair<int, std::vector<int>>> saved_;
};

// Places the requests no route holds yet. Routes already in the plan are the
// starting point of the solve and stay as they are.
void constructPairs(PdpRoutePlan &plan, bool decreasing)
{
    std::vector<char> routed(plan.nodes.size(), 0);
    for (const Vehicle &vehicle : plan.vehicles)
    {
        for (int nodeIndex : vehicle.nodeIndices)
            routed[static_cast<std::size_t>(nodeIndex)] = 1;
    }
    plan.unassignedNodeIndices.clear();

    std::vector<int> order;
    order.reserve(plan.deliveries.size());
    for (std::size_t i = 0; i < plan.deliveries.size(); ++i)
    {
        const std::size_t pickup
            = static_cast<std::size_t>(plan.deliveries[i].pickupNodeIndex);
        if (!routed[pickup])
            order.push_back(static_cast<int>(i));
    }
    if (decreasing)
    {
        std::stable_sort(order.begin(),
                         order.end(),
                         [&plan](int a, int b)
                         {
                             const Delivery &left = plan.deliveries[a];
                             const Delivery &right = plan.deliveries[b];
                             const Node &lp = plan.nodes[left.pickupNodeIndex];
                             const Node &rp = plan.nodes[right.pickupNodeIndex];
                             const Seconds lw = lp.maxEndTime - lp.minStartTime;
                             const Seconds rw = rp.maxEndTime - rp.minStartTime;
                             if (lw != rw)
                                 return lw < rw;
                             return left.demand > right.demand;
                         });
    }
    // A bound request has one vehicle to go to, so it goes first: a free
    // request placed before it could take the only slot that fits.
    std::stable_partition(
        order.begin(),
        order.end(),
        [&plan](int deliveryIndex)
        { return plan.deliveries[deliveryIndex].requiredVehicleIndex >= 0; });

    for (int deliveryIndex : order)
    {
        const Delivery &delivery = plan.deliveries[deliveryIndex];
        int vehicle = -1;
        int pickup = -1;
        int dropoff = -1;
        if (PairRelocateMove::findBestInsertion(
                plan, delivery, vehicle, pickup, dropoff))
        {
            std::vector<int> &route = plan.vehicles[vehicle].nodeIndices;
            route.insert(route.begin() + pickup, delivery.pickupNodeIndex);
            route.insert(route.begin() + dropoff, delivery.dropoffNodeIndex);
        }
        else
        {
            plan.unassignedNodeIndices.push_back(delivery.pickupNodeIndex);
            plan.unassignedNodeIndices.push_back(delivery.dropoffNodeIndex);
        }
    }
}

// Swaps two complete requests between routes. Doing the exchange at request
// granularity preserves both
// same-vehicle and pickup-before-dropoff throughout the candidate move.
class PairExchangeMove final : public optimaru::MoveOperator<PdpRoutePlan>
{
public:
    const char *name() const override { return "pairExchange"; }

    bool apply(optimaru::OperatorContext<PdpRoutePlan> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        PdpRoutePlan &plan = ctx.director.workingSolution();
        if (plan.deliveries.size() < 2)
            return false;
        const int pool = Descriptor::unassignedList(plan);
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            const int a = static_cast<int>(rng() % plan.deliveries.size());
            const int b = static_cast<int>(rng() % plan.deliveries.size());
            if (a == b)
                continue;
            const Delivery &da = plan.deliveries[static_cast<std::size_t>(a)];
            const Delivery &db = plan.deliveries[static_cast<std::size_t>(b)];
            const int listA = ctx.elementList[da.pickupNodeIndex];
            const int listB = ctx.elementList[db.pickupNodeIndex];
            if (listA < 0 || listB < 0 || listA == listB || listA == pool
                || listB == pool || !pairIntact(plan, ctx, da)
                || !pairIntact(plan, ctx, db))
                continue;

            saved_.clear();
            saved_.emplace_back(listA, Descriptor::listOf(plan, listA));
            saved_.emplace_back(listB, Descriptor::listOf(plan, listB));
            const int pickupPosA = ctx.elementPosition[da.pickupNodeIndex];
            const int dropoffPosA = ctx.elementPosition[da.dropoffNodeIndex];
            const int pickupPosB = ctx.elementPosition[db.pickupNodeIndex];
            const int dropoffPosB = ctx.elementPosition[db.dropoffNodeIndex];

            erasePair(ctx, plan, listA, da);
            erasePair(ctx, plan, listB, db);
            insertPair(ctx, plan, listA, db, pickupPosA, dropoffPosA);
            insertPair(ctx, plan, listB, da, pickupPosB, dropoffPosB);
            entities.insert(entities.end(),
                            {da.pickupNodeIndex,
                             da.dropoffNodeIndex,
                             db.pickupNodeIndex,
                             db.dropoffNodeIndex});
            return true;
        }
        return false;
    }

    void undoLast(optimaru::OperatorContext<PdpRoutePlan> &ctx) override
    {
        PdpRoutePlan &plan = ctx.director.workingSolution();
        for (const auto &[list, contents] : saved_)
        {
            ctx.director.beginListChange(list);
            Descriptor::listOf(plan, list) = contents;
            ctx.director.endListChange(list);
        }
        saved_.clear();
    }

private:
    static void erasePair(optimaru::OperatorContext<PdpRoutePlan> &ctx,
                          PdpRoutePlan &plan,
                          int list,
                          const Delivery &delivery)
    {
        ctx.director.beginListChange(list);
        auto &route = Descriptor::listOf(plan, list);
        route.erase(
            std::remove(route.begin(), route.end(), delivery.pickupNodeIndex),
            route.end());
        route.erase(
            std::remove(route.begin(), route.end(), delivery.dropoffNodeIndex),
            route.end());
        ctx.director.endListChange(list);
    }

    static void insertPair(optimaru::OperatorContext<PdpRoutePlan> &ctx,
                           PdpRoutePlan &plan,
                           int list,
                           const Delivery &delivery,
                           int pickupPosition,
                           int dropoffPosition)
    {
        auto &route = Descriptor::listOf(plan, list);
        const int p
            = std::clamp(pickupPosition, 0, static_cast<int>(route.size()));
        // Preserve roughly the replaced request's span while guaranteeing
        // strict pickup-before-dropoff after the first insertion shifts it.
        const int d = std::clamp(
            dropoffPosition, p + 1, static_cast<int>(route.size()) + 1);
        ctx.director.beginListChange(list);
        route.insert(route.begin() + p, delivery.pickupNodeIndex);
        route.insert(route.begin() + d, delivery.dropoffNodeIndex);
        ctx.director.endListChange(list);
    }

    std::vector<std::pair<int, std::vector<int>>> saved_;
};

// ---------------------------------------------------------------------------
// PairRuin
// ---------------------------------------------------------------------------
// Ruins whole requests. The built-in ruins pick individual stops, which leaves
// half of a request routed and half in limbo — hard-infeasible states the
// recreate then has to guess its way out of. Selecting both stops of each
// chosen request keeps every kick coupling-clean by construction.
//
// The seed request is random and the rest are its nearest neighbours (by the
// configured driving-time meter), so the kick tears out a spatially coherent
// cluster of requests rather than a scatter — the same rationale as the
// related ruin, lifted to request granularity.
class PairRuin final : public optimaru::RuinOperator<PdpRoutePlan>
{
public:
    const char *name() const override { return "pairRuin"; }

    std::vector<int> selectRuinSet(optimaru::OperatorContext<PdpRoutePlan> &ctx,
                                   int ruinCount,
                                   std::mt19937_64 &rng) override
    {
        const PdpRoutePlan &plan = ctx.director.workingSolution();
        const int nodeCount = static_cast<int>(plan.nodes.size());
        if (nodeCount == 0)
            return {};

        std::vector<char> taken(plan.deliveries.size(), 0);
        std::vector<int> ruined;
        ruined.reserve(static_cast<std::size_t>(ruinCount));

        const int seed
            = static_cast<int>(rng() % static_cast<std::uint64_t>(nodeCount));
        addRequestOf(plan, seed, taken, ruined, ruinCount);

        if (ctx.nearby.enabled())
        {
            for (int neighbor : ctx.nearby.of(seed))
            {
                if (static_cast<int>(ruined.size()) >= ruinCount)
                    break;
                addRequestOf(plan, neighbor, taken, ruined, ruinCount);
            }
        }
        // Top up randomly when the neighbour list runs out before the budget.
        for (int attempt = 0; attempt < 4 * ruinCount
                              && static_cast<int>(ruined.size()) < ruinCount;
             ++attempt)
        {
            const int node = static_cast<int>(
                rng() % static_cast<std::uint64_t>(nodeCount));
            addRequestOf(plan, node, taken, ruined, ruinCount);
        }
        return ruined;
    }

private:
    // Adds both stops of the request owning `node`, unless already taken.
    // Both go in together or not at all — a partial add would defeat the point.
    static void addRequestOf(const PdpRoutePlan &plan,
                             int node,
                             std::vector<char> &taken,
                             std::vector<int> &ruined,
                             int ruinCount)
    {
        const int deliveryIndex
            = plan.nodes[static_cast<std::size_t>(node)].deliveryIndex;
        const auto d = static_cast<std::size_t>(deliveryIndex);
        if (taken[d])
            return;
        if (static_cast<int>(ruined.size()) + 2 > ruinCount)
            return;
        taken[d] = 1;
        ruined.push_back(plan.deliveries[d].pickupNodeIndex);
        ruined.push_back(plan.deliveries[d].dropoffNodeIndex);
    }
};

void solveInParallel(const Solver::Config &config,
                     PdpRoutePlan &plan,
                     int workerCount)
{
    std::vector<PdpRoutePlan> candidates(static_cast<std::size_t>(workerCount),
                                         plan);
    std::vector<std::exception_ptr> failures(
        static_cast<std::size_t>(workerCount));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i)
    {
        workers.emplace_back(
            [&config, i, &candidates, &failures]
            {
                try
                {
                    Solver::Config workerConfig = config;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.randomSeed += static_cast<std::uint64_t>(i);
                    if (i != 0)
                        workerConfig.onNewBest = nullptr;
                    Solver(std::move(workerConfig))
                        .solve(candidates[static_cast<std::size_t>(i)]);
                }
                catch (...)
                {
                    failures[static_cast<std::size_t>(i)]
                        = std::current_exception();
                }
            });
    }
    for (std::thread &worker : workers)
        worker.join();
    for (const std::exception_ptr &failure : failures)
    {
        if (failure)
            std::rethrow_exception(failure);
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < candidates.size(); ++i)
    {
        if (candidates[i].score > candidates[best].score)
            best = i;
    }
    plan = std::move(candidates[best]);
}

}  // namespace

namespace
{

// A required vehicle is a raw position in plan.vehicles, and the pair move uses
// it as a list index, so a bad one has to stop here.
void checkRequiredVehicles(const PdpRoutePlan &plan)
{
    const int vehicleCount = static_cast<int>(plan.vehicles.size());
    for (const Delivery &delivery : plan.deliveries)
    {
        if (delivery.requiredVehicleIndex < -1
            || delivery.requiredVehicleIndex >= vehicleCount)
        {
            throw std::invalid_argument(
                "request " + delivery.id + ": requiredVehicleIndex "
                + std::to_string(delivery.requiredVehicleIndex)
                + " is out of range -1.." + std::to_string(vehicleCount - 1));
        }
    }
}

// Shared engine wiring for both solve() overloads: termination, late
// acceptance, the LIST move mix, the ALNS knobs and the driving-time nearby
// selection (all pure problem-fact lookups, so they apply to the plugin path
// too).
optimaru::SolverConfig buildEngineConfig(const Solver::Config &config,
                                         const PdpRoutePlan &plan)
{
    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.multiStartCount = 1;
    if (config.onNewBest)
    {
        auto callback = config.onNewBest;
        engineConfig.onNewBest =
            [callback](std::int64_t step, const optimaru::HardSoftScore &score)
        { callback(step, fromEngineScore(score)); };
    }

    engineConfig.termination.spentLimit = config.spentLimit;
    if (config.bestScoreLimit.medium
        != std::numeric_limits<std::int64_t>::max())
        engineConfig.termination.bestScoreLimit
            = toEngineScore(config.bestScoreLimit);
    engineConfig.termination.unimprovedSpentLimit = config.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit
        = config.unimprovedStepCountLimit;

    // Construction order: place a request's pickup right before its dropoff by
    // giving the pair adjacent difficulty, tightest window first. The stops are
    // still placed one at a time (the engine's CH is element-granular), but
    // consecutive placement means the dropoff is inserted while its pickup is
    // the freshest thing in the route.
    if (config.firstFitDecreasing)
    {
        const PdpRoutePlan *facts = &plan;
        engineConfig.constructionHeuristic.entityDifficulty
            = [facts](int nodeIndex)
        {
            const Node &node
                = facts->nodes[static_cast<std::size_t>(nodeIndex)];
            const Delivery &delivery
                = facts->deliveries[static_cast<std::size_t>(
                    node.deliveryIndex)];
            const double window = static_cast<double>(
                std::max<Seconds>(node.maxEndTime - node.minStartTime, 1));
            // Pickups rank marginally above their dropoff so the pair is laid
            // down in the legal order.
            return static_cast<double>(delivery.demand) + 1e6 / window
                   + (node.pickup ? 1e-3 : 0.0);
        };
    }

    auto &acceptor = engineConfig.localSearch.acceptor;
    acceptor.acceptorType = config.acceptorType;
    acceptor.lateAcceptanceSize
        = config.lateAcceptanceSize != 0
              ? config.lateAcceptanceSize
              : (plan.nodes.size() > 60
                     ? 5
                     : std::clamp<std::size_t>(
                           4 * plan.nodes.size(), 200, 1600));
    acceptor.recordToRecordTravelStartGap = config.recordToRecordStartGap;
    acceptor.recordToRecordTravelEndGap = config.recordToRecordEndGap;

    auto &moves = engineConfig.localSearch.moveSelector;
    moves.relocateMoveWeight = config.relocateMoveWeight;
    moves.listSwapMoveWeight = config.listSwapMoveWeight;
    moves.twoOptMoveWeight = config.twoOptMoveWeight;
    moves.orOptMoveWeight = config.orOptMoveWeight;
    moves.tailSwapMoveWeight = config.tailSwapMoveWeight;
    moves.interExchangeMoveWeight = config.interExchangeMoveWeight;
    moves.interExchangeMaxSegment = config.interExchangeMaxSegment;

    auto &lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config.ruinRecreate;
    lns.unimprovedStepTrigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config.ruinRecreateMaxRuinCount;
    lns.polishSteps = config.ruinRecreatePolishSteps;
    lns.asMoveWeight = config.ruinRecreateAsMoveWeight;
    lns.asMoveMaxRuin = config.ruinRecreateAsMoveMaxRuin;
    engineConfig.localSearch.descentWeight = config.descentWeight;
    engineConfig.localSearch.descentMaxSweeps = config.descentMaxSweeps;
    engineConfig.localSearch.routeEliminationWeight
        = config.routeEliminationWeight;
    engineConfig.localSearch.routeEliminationMaxSize
        = config.routeEliminationMaxSize;
    lns.selectionType = config.ruinRecreateSelectionType;
    lns.operatorDecay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.selectionSegmentLength = config.ruinRecreateSelectionSegmentLength;
    lns.selectionAlpha = config.ruinRecreateSelectionAlpha;
    lns.sisrRuin = config.sisrRuin;
    lns.routeRuin = config.routeRuin;
    lns.sisrMaxStringRemovals = config.sisrMaxStringRemovals;
    lns.sisrMaxStringSize = config.sisrMaxStringSize;
    lns.regretK = config.regretK;
    lns.blinkRate = config.blinkRate;

    if (config.drivingTimeNearby && plan.nodes.size() > 1)
    {
        auto locationOfNode = std::make_shared<std::vector<int>>();
        locationOfNode->reserve(plan.nodes.size());
        for (const Node &node : plan.nodes)
            locationOfNode->push_back(node.locationIndex);
        auto matrix = std::make_shared<std::vector<std::vector<Seconds>>>(
            plan.drivingSeconds);
        engineConfig.localSearch.nearby.distanceMeter
            = [locationOfNode, matrix](int a, int b)
        {
            return static_cast<double>(
                (*matrix)[static_cast<std::size_t>((*locationOfNode)[a])]
                         [static_cast<std::size_t>((*locationOfNode)[b])]);
        };
        engineConfig.localSearch.nearby.size
            = std::max(1,
                       std::min(config.drivingTimeNearbySize,
                                static_cast<int>(plan.nodes.size()) - 1));
        engineConfig.localSearch.nearby.probability
            = config.drivingTimeNearbyProbability;
    }

    return engineConfig;
}

}  // namespace

void Solver::solve(PdpRoutePlan &plan)
{
    checkRequiredVehicles(plan);
    // Empty pool so the construction heuristic places every stop itself.
    if (config_.pairConstructionHeuristic && !plan.vehicles.empty())
        constructPairs(plan, config_.firstFitDecreasing);
    else
        plan.unassignedNodeIndices.clear();

    if (plan.vehicles.empty() || plan.nodes.empty())
    {
        ScoreCalculator::calculateScore(plan);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        solveInParallel(config_, plan, workerCount);
        return;
    }

    optimaru::SolverConfig engineConfig = buildEngineConfig(config_, plan);
    PdpIncrementalScoreCalculator calculator;
    optimaru::Solver<PdpRoutePlan> engine(std::move(engineConfig));
    if (config_.pairMoves)
    {
        engine.addMoveOperator(std::make_unique<PairRelocateMove>(
            config_.pairRepairProbability, config_.pairAssignmentProbability));
    }
    if (config_.pairExchangeMoves
        && static_cast<int>(plan.nodes.size()) >= config_.pairExchangeMinNodes)
        engine.addMoveOperator(std::make_unique<PairExchangeMove>());
    if (config_.pairRuin)
        engine.addRuinOperator(std::make_unique<PairRuin>());
    engine.solve(plan, calculator);

    plan.score = ScoreCalculator::calculateScore(plan);
}

void Solver::solve(
    PdpRoutePlan &plan,
    optimaru::DecomposedScoreCalculator<PdpRoutePlan> &calculator)
{
    checkRequiredVehicles(plan);
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (config_.pairConstructionHeuristic && !plan.vehicles.empty())
        constructPairs(plan, config_.firstFitDecreasing);
    if (!plan.nodes.empty() && !plan.vehicles.empty())
    {
        // The pair-aware operators only read the plan (which request a node
        // belongs to, where its partner sits), never the calculator, so the
        // plugin path keeps them — they are what makes a PDP search work.
        optimaru::Solver<PdpRoutePlan> engine(buildEngineConfig(config_, plan));
        if (config_.pairMoves)
        {
            engine.addMoveOperator(std::make_unique<PairRelocateMove>(
                config_.pairRepairProbability,
                config_.pairAssignmentProbability));
        }
        if (config_.pairExchangeMoves
            && static_cast<int>(plan.nodes.size())
                   >= config_.pairExchangeMinNodes)
            engine.addMoveOperator(std::make_unique<PairExchangeMove>());
        if (config_.pairRuin)
            engine.addRuinOperator(std::make_unique<PairRuin>());
        engine.solve(plan, calculator);
    }

    // Refresh the node shadow variables (vehicleIndex / arrivalTime) on the
    // best solution — the plugin components derive their own chain and never
    // write these — then score it with the caller's constraint assembly,
    // leaving the calculator primed on it so constraintTotals() explains
    // exactly this score.
    ScoreCalculator::updateShadowVariables(plan);
    calculator.resetWorkingSolution(plan);
    plan.score = fromEngineScore(calculator.calculateScore());
}

}  // namespace pdp
