#include "pdp/solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pdp/incremental_calculator.hpp"
#include "pdp/request_insertion.hpp"
#include "pdp/request_moves.hpp"
#include "pdp/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/search/operators.hpp"

namespace pdp {

namespace {

using Descriptor = optimaru::SolutionTraits<PdpRoutePlan>;

// Whether a request's coupling rules currently hold, read from the engine's
// live element index (O(1), no scoring).
bool pairIntact(const PdpRoutePlan& plan, const optimaru::OperatorContext<PdpRoutePlan>& ctx,
                const Delivery& delivery) {
    const int pool = Descriptor::unassigned_list(plan);
    const int pickupList = ctx.element_list[delivery.pickupNodeIndex];
    const int dropoffList = ctx.element_list[delivery.dropoffNodeIndex];
    if (pickupList != dropoffList) return false;
    if (pickupList == pool)
        return true;  // both parked: a dropped request is coupled, just
                      // unrouted
    return ctx.element_position[delivery.pickupNodeIndex] <
           ctx.element_position[delivery.dropoffNodeIndex];
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
// A relocate takes its positions from nearby stops rather than from a scan of
// every slot pair: placing a request at its cheapest slot is what the request
// moves (pdp/request_moves.hpp) do. This move is here for the first two tiers.
//
// It moves one request, so a request of a group (Delivery::groupIndex) only
// goes where the rest of its group rides: it is neither dropped alone nor
// routed alone. Taking a whole group to another vehicle, or in from the pool,
// is the request moves' work.
class PairRelocateMove final : public optimaru::MoveOperator<PdpRoutePlan> {
public:
    PairRelocateMove(double repairProbability, double assignmentProbability)
        : repairProbability_(repairProbability), assignmentProbability_(assignmentProbability) {}

    const char* name() const override { return "pairRelocate"; }

    bool apply(optimaru::OperatorContext<PdpRoutePlan>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        PdpRoutePlan& plan = ctx.editor.solution();
        const int deliveryCount = static_cast<int>(plan.deliveries.size());
        const int vehicleCount = static_cast<int>(plan.vehicles.size());
        if (deliveryCount == 0 || vehicleCount == 0) return false;
        const int pool = Descriptor::unassigned_list(plan);
        if (!groupsBuilt_) {
            groups_.build(plan);
            groupsBuilt_ = true;
        }

        std::uniform_real_distribution<double> unit(0.0, 1.0);

        // Tier 1: prefer a broken request when there is one.
        int deliveryIndex = -1;
        if (unit(rng) < repairProbability_) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                const int candidate =
                        static_cast<int>(rng() % static_cast<std::uint64_t>(deliveryCount));
                if (!pairIntact(plan, ctx, plan.deliveries[static_cast<std::size_t>(candidate)])) {
                    deliveryIndex = candidate;
                    break;
                }
            }
        }
        if (deliveryIndex < 0) {
            deliveryIndex = static_cast<int>(rng() % static_cast<std::uint64_t>(deliveryCount));
        }

        const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(deliveryIndex)];
        const int pickup = delivery.pickupNodeIndex;
        const int dropoff = delivery.dropoffNodeIndex;
        // The one vehicle the request may ride, and whether it travels with
        // other requests.
        const int bound = boundVehicle(plan, ctx, deliveryIndex);
        const bool grouped = groups_.sizeOf(groups_.leaderOf(deliveryIndex)) > 1;

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
        const bool currentlyPooled =
                ctx.element_list[pickup] == pool && ctx.element_list[dropoff] == pool;

        if (currentlyPooled) {
            // Assignment is the lexicographically important move. Random
            // insertion misses almost every feasible pickup/dropoff slot on
            // large PDPTW routes, so scan the pair slots and take the least
            // added travel among capacity/time-window-feasible candidates.
            // The first request of a group to leave the pool would leave it
            // alone: the request moves route the group whole.
            if (grouped && bound < 0) return false;
            timelines_.rebuild(plan);
            Blinker every;
            RequestInsertion insertion;
            if (bound >= 0) {
                timelines_.of(bound).improve(plan, delivery, every, insertion);
            } else {
                insertion = timelines_.best(plan, delivery, every);
            }
            if (!insertion.found()) return false;
            target = insertion.vehicle;
            greedyPickupPos = insertion.pickupPosition;
            greedyDropoffPos = insertion.dropoffPosition;
        } else if (unit(rng) < assignmentProbability_ && !grouped) {
            target = pool;  // drop the request
        } else {
            // A bound request only moves along its own route, which is also
            // where a repair has to put it back.
            const int required = bound;
            if (ctx.nearby.enabled()) {
                pickupAnchor = required >= 0 ? routedNeighborIn(ctx, plan, pickup, required, rng)
                                             : routedNeighbor(ctx, plan, pickup, pool, rng);
                if (pickupAnchor >= 0) {
                    target = ctx.element_list[pickupAnchor];
                    // A neighbour of the dropoff sitting in the SAME route is
                    // the natural place for the second stop.
                    const int candidate = routedNeighborIn(ctx, plan, dropoff, target, rng);
                    dropoffAnchor = candidate;
                }
            }
            if (target < 0) {
                target = required >= 0 ? required
                                       : static_cast<int>(rng() %
                                                          static_cast<std::uint64_t>(vehicleCount));
            }
        }

        // Save every list we are about to touch, for the undo.
        saved_.clear();
        save(plan, ctx.element_list[pickup]);
        save(plan, ctx.element_list[dropoff]);
        save(plan, target);

        // Detach both stops, then re-insert them together.
        removeNode(ctx, plan, pickup);
        removeNode(ctx, plan, dropoff);

        const std::vector<int>& targetList = Descriptor::list_of(plan, target);
        const int targetSize = static_cast<int>(targetList.size());
        if (target == pool) {
            // Order is meaningless in the pool; append both.
            ctx.editor.begin_list_edit(target);
            Descriptor::insert_element(plan, target, targetSize, pickup);
            Descriptor::insert_element(plan, target, targetSize + 1, dropoff);
            ctx.editor.end_list_edit(target);
        } else {
            // Positions are resolved against the post-detach list, so the
            // anchors are located by value rather than by their stale index.
            int pickupPos = greedyPickupPos >= 0 ? greedyPickupPos
                                                 : positionAfter(targetList, pickupAnchor);
            // Pinned stops stay in front: a slot among them is no slot at all.
            const int firstFree =
                    std::min(static_cast<int>(plan.vehicles[static_cast<std::size_t>(target)]
                                                      .pinnedNodeIndices.size()),
                             targetSize);
            if (pickupPos < 0 || pickupPos < firstFree) {
                pickupPos = firstFree +
                            static_cast<int>(
                                    rng() % static_cast<std::uint64_t>(targetSize - firstFree + 1));
            }
            int dropoffPos = greedyDropoffPos >= 0 ? greedyDropoffPos
                                                   : positionAfter(targetList, dropoffAnchor);
            // The dropoff must land strictly after the pickup for the pair to
            // be legal; an anchor that would put it earlier is discarded.
            if (dropoffPos <= pickupPos) {
                dropoffPos = pickupPos + 1 +
                             static_cast<int>(rng() % static_cast<std::uint64_t>(targetSize -
                                                                                 pickupPos + 1));
            } else if (greedyDropoffPos < 0) {
                // +1 because inserting the pickup first shifts everything
                // at or after pickupPos one place to the right.
                ++dropoffPos;
            }
            ctx.editor.begin_list_edit(target);
            Descriptor::insert_element(plan, target, pickupPos, pickup);
            Descriptor::insert_element(plan, target, dropoffPos, dropoff);
            ctx.editor.end_list_edit(target);
        }

        entities.push_back(pickup);
        entities.push_back(dropoff);
        return true;
    }

    void undo_last(optimaru::OperatorContext<PdpRoutePlan>& ctx) override {
        PdpRoutePlan& plan = ctx.editor.solution();
        for (const auto& [listIndex, contents] : saved_) {
            ctx.editor.begin_list_edit(listIndex);
            Descriptor::list_of(plan, listIndex) = contents;
            ctx.editor.end_list_edit(listIndex);
        }
        saved_.clear();
    }

private:
    // A random nearby stop of `node` that is currently on some vehicle.
    static int routedNeighbor(const optimaru::OperatorContext<PdpRoutePlan>& ctx,
                              const PdpRoutePlan& plan, int node, int pool, std::mt19937_64& rng) {
        const std::vector<int>& neighbors = ctx.nearby.of(node);
        if (neighbors.empty()) return -1;
        const auto start = rng() % static_cast<std::uint64_t>(neighbors.size());
        for (std::size_t i = 0; i < neighbors.size(); ++i) {
            const int candidate = neighbors[(start + i) % neighbors.size()];
            const int list = ctx.element_list[candidate];
            if (list < 0 || list == pool) continue;
            if (candidate == node || candidate == plan.partnerOf(node))
                continue;  // the pair itself is about to be detached
            return candidate;
        }
        return -1;
    }

    // The same, restricted to one route.
    static int routedNeighborIn(const optimaru::OperatorContext<PdpRoutePlan>& ctx,
                                const PdpRoutePlan& plan, int node, int list,
                                std::mt19937_64& rng) {
        const std::vector<int>& neighbors = ctx.nearby.of(node);
        if (neighbors.empty()) return -1;
        const auto start = rng() % static_cast<std::uint64_t>(neighbors.size());
        for (std::size_t i = 0; i < neighbors.size(); ++i) {
            const int candidate = neighbors[(start + i) % neighbors.size()];
            if (ctx.element_list[candidate] != list) continue;
            if (candidate == node || candidate == plan.partnerOf(node)) continue;
            return candidate;
        }
        return -1;
    }

    // Where `anchor` currently sits in `list`, plus one — i.e. the slot right
    // after it. -1 when there is no anchor or it is no longer in the list.
    static int positionAfter(const std::vector<int>& list, int anchor) {
        if (anchor < 0) return -1;
        const auto at = std::find(list.begin(), list.end(), anchor);
        if (at == list.end()) return -1;
        return static_cast<int>(at - list.begin()) + 1;
    }

    void save(const PdpRoutePlan& plan, int listIndex) {
        for (const auto& entry : saved_) {
            if (entry.first == listIndex) return;  // already saved
        }
        saved_.emplace_back(listIndex, Descriptor::list_of(plan, listIndex));
    }

    // Removes a node from whichever list holds it, through the editor so the
    // calculator sees the change.
    static void removeNode(optimaru::OperatorContext<PdpRoutePlan>& ctx, PdpRoutePlan& plan,
                           int node) {
        const int listIndex = ctx.element_list[node];
        if (listIndex < 0) return;
        std::vector<int>& list = Descriptor::list_of(plan, listIndex);
        const auto at = std::find(list.begin(), list.end(), node);
        if (at == list.end()) return;  // already removed (pickup and dropoff share a list)
        ctx.editor.begin_list_edit(listIndex);
        list.erase(at);
        ctx.editor.end_list_edit(listIndex);
    }

    // The vehicle a request has to be on: its required vehicle (its own or
    // its group's), else the one another request of its group rides, else -1.
    int boundVehicle(const PdpRoutePlan& plan, const optimaru::OperatorContext<PdpRoutePlan>& ctx,
                     int deliveryIndex) const {
        const int required = groups_.vehicleOf(deliveryIndex);
        if (required >= 0) return required;
        const int pool = Descriptor::unassigned_list(plan);
        for (int d = groups_.leaderOf(deliveryIndex); d >= 0; d = groups_.nextOf(d)) {
            if (d == deliveryIndex) continue;
            const int list =
                    ctx.element_list[plan.deliveries[static_cast<std::size_t>(d)].pickupNodeIndex];
            if (list >= 0 && list != pool) return list;
        }
        return -1;
    }

    double repairProbability_;
    double assignmentProbability_;
    std::vector<std::pair<int, std::vector<int>>> saved_;
    PlanTimelines timelines_;  // for routing a request that waits in the pool
    RequestGroups groups_;     // which requests travel together
    bool groupsBuilt_ = false;
};

// Places the requests no route holds yet. Routes already in the plan are the
// starting point of the solve and stay as they are. The requests of a group go
// in together, on one vehicle: the one the group is bound to or already rides,
// else the one that takes all of them for the least driving.
void constructPairs(PdpRoutePlan& plan, bool decreasing) {
    std::vector<int> routedOn(plan.nodes.size(), -1);
    for (std::size_t v = 0; v < plan.vehicles.size(); ++v) {
        for (int nodeIndex : plan.vehicles[v].nodeIndices)
            routedOn[static_cast<std::size_t>(nodeIndex)] = static_cast<int>(v);
    }
    plan.unassignedNodeIndices.clear();

    std::vector<int> order;
    order.reserve(plan.deliveries.size());
    for (std::size_t i = 0; i < plan.deliveries.size(); ++i) {
        const std::size_t pickup = static_cast<std::size_t>(plan.deliveries[i].pickupNodeIndex);
        if (routedOn[pickup] < 0) order.push_back(static_cast<int>(i));
    }
    if (decreasing) {
        std::stable_sort(order.begin(), order.end(), [&plan](int a, int b) {
            const Delivery& left = plan.deliveries[a];
            const Delivery& right = plan.deliveries[b];
            const Node& lp = plan.nodes[left.pickupNodeIndex];
            const Node& rp = plan.nodes[right.pickupNodeIndex];
            const Seconds lw = lp.maxEndTime - lp.minStartTime;
            const Seconds rw = rp.maxEndTime - rp.minStartTime;
            if (lw != rw) return lw < rw;
            return left.demand > right.demand;
        });
    }
    RequestGroups groups;
    groups.build(plan);
    // The vehicle each unit has to go to, by leader: the one it is bound to,
    // else the one that already carries most of its requests (the first such
    // when they start on several), else -1.
    std::vector<int> boundOf(plan.deliveries.size(), -1);
    std::vector<std::pair<int, int>> riding;  // (vehicle, requests of the unit on it)
    for (int deliveryIndex : order) {
        const int leader = groups.leaderOf(deliveryIndex);
        int& bound = boundOf[static_cast<std::size_t>(leader)];
        bound = groups.vehicleOf(leader);
        if (bound >= 0 || groups.sizeOf(leader) == 1) continue;
        riding.clear();
        for (int d = leader; d >= 0; d = groups.nextOf(d)) {
            const int on = routedOn[static_cast<std::size_t>(
                    plan.deliveries[static_cast<std::size_t>(d)].pickupNodeIndex)];
            if (on < 0) continue;
            const auto at = std::find_if(riding.begin(), riding.end(),
                                         [on](const auto& entry) { return entry.first == on; });
            if (at == riding.end()) {
                riding.emplace_back(on, 1);
            } else {
                ++at->second;
            }
        }
        int most = 0;
        for (const auto& [vehicle, requests] : riding) {
            if (requests > most) {
                most = requests;
                bound = vehicle;
            }
        }
    }
    // A bound request has one vehicle to go to, so it goes first: a free
    // request placed before it could take the only slot that fits. A group
    // needs one vehicle with room for all of its requests, so it goes before
    // the requests that travel alone.
    const auto rank = [&groups, &boundOf](int deliveryIndex) {
        const int leader = groups.leaderOf(deliveryIndex);
        if (boundOf[static_cast<std::size_t>(leader)] >= 0) return 0;
        return groups.sizeOf(leader) > 1 ? 1 : 2;
    };
    std::stable_sort(order.begin(), order.end(),
                     [&rank](int a, int b) { return rank(a) < rank(b); });

    PlanTimelines timelines;
    timelines.rebuild(plan);
    Blinker every;
    UnitPlanner planner;
    std::vector<char> done(plan.deliveries.size(), 0);  // by unit leader
    std::vector<int> waiting;
    std::vector<int> candidates;
    std::vector<RequestInsertion> steps;
    std::vector<RequestInsertion> trial;
    for (int deliveryIndex : order) {
        const int leader = groups.leaderOf(deliveryIndex);
        if (done[static_cast<std::size_t>(leader)]) continue;  // went in with its group
        done[static_cast<std::size_t>(leader)] = 1;

        // The requests of the unit still to place.
        waiting.clear();
        const int bound = boundOf[static_cast<std::size_t>(leader)];
        for (int d = leader; d >= 0; d = groups.nextOf(d)) {
            if (routedOn[static_cast<std::size_t>(
                        plan.deliveries[static_cast<std::size_t>(d)].pickupNodeIndex)] < 0) {
                waiting.push_back(d);
            }
        }

        timelines.candidateVehicles(plan, bound, candidates);
        steps.clear();
        Seconds least = std::numeric_limits<Seconds>::max();
        for (int v : candidates) {
            trial.clear();
            Seconds added = 0;
            if (planner.plan(plan, timelines.of(v),
                             plan.vehicles[static_cast<std::size_t>(v)].nodeIndices, waiting, every,
                             trial, added) &&
                added < least) {
                least = added;
                steps.swap(trial);
            }
        }
        if (steps.empty()) {
            for (int d : waiting) {
                const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(d)];
                plan.unassignedNodeIndices.push_back(delivery.pickupNodeIndex);
                plan.unassignedNodeIndices.push_back(delivery.dropoffNodeIndex);
            }
            continue;
        }
        const int vehicle = steps.front().vehicle;
        std::vector<int>& route = plan.vehicles[static_cast<std::size_t>(vehicle)].nodeIndices;
        for (std::size_t i = 0; i < waiting.size(); ++i) {
            const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(waiting[i])];
            route.insert(route.begin() + steps[i].pickupPosition, delivery.pickupNodeIndex);
            route.insert(route.begin() + steps[i].dropoffPosition, delivery.dropoffNodeIndex);
        }
        timelines.rebuildRoute(plan, vehicle, route);
    }
}

// Swaps two complete requests between routes. Doing the exchange at request
// granularity preserves both
// same-vehicle and pickup-before-dropoff throughout the candidate move.
class PairExchangeMove final : public optimaru::MoveOperator<PdpRoutePlan> {
public:
    const char* name() const override { return "pairExchange"; }

    bool apply(optimaru::OperatorContext<PdpRoutePlan>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        PdpRoutePlan& plan = ctx.editor.solution();
        if (plan.deliveries.size() < 2) return false;
        if (!groupsBuilt_) {
            groups_.build(plan);
            groupsBuilt_ = true;
        }
        const int pool = Descriptor::unassigned_list(plan);
        for (int attempt = 0; attempt < 12; ++attempt) {
            const int a = static_cast<int>(rng() % plan.deliveries.size());
            const int b = static_cast<int>(rng() % plan.deliveries.size());
            if (a == b) continue;
            const Delivery& da = plan.deliveries[static_cast<std::size_t>(a)];
            const Delivery& db = plan.deliveries[static_cast<std::size_t>(b)];
            // A group changes vehicle as one, in the request moves.
            if (groups_.sizeOf(groups_.leaderOf(a)) > 1 || groups_.sizeOf(groups_.leaderOf(b)) > 1)
                continue;
            const int listA = ctx.element_list[da.pickupNodeIndex];
            const int listB = ctx.element_list[db.pickupNodeIndex];
            if (listA < 0 || listB < 0 || listA == listB || listA == pool || listB == pool ||
                !pairIntact(plan, ctx, da) || !pairIntact(plan, ctx, db))
                continue;

            saved_.clear();
            saved_.emplace_back(listA, Descriptor::list_of(plan, listA));
            saved_.emplace_back(listB, Descriptor::list_of(plan, listB));
            const int pickupPosA = ctx.element_position[da.pickupNodeIndex];
            const int dropoffPosA = ctx.element_position[da.dropoffNodeIndex];
            const int pickupPosB = ctx.element_position[db.pickupNodeIndex];
            const int dropoffPosB = ctx.element_position[db.dropoffNodeIndex];

            erasePair(ctx, plan, listA, da);
            erasePair(ctx, plan, listB, db);
            insertPair(ctx, plan, listA, db, pickupPosA, dropoffPosA);
            insertPair(ctx, plan, listB, da, pickupPosB, dropoffPosB);
            entities.insert(entities.end(), {da.pickupNodeIndex, da.dropoffNodeIndex,
                                             db.pickupNodeIndex, db.dropoffNodeIndex});
            return true;
        }
        return false;
    }

    void undo_last(optimaru::OperatorContext<PdpRoutePlan>& ctx) override {
        PdpRoutePlan& plan = ctx.editor.solution();
        for (const auto& [list, contents] : saved_) {
            ctx.editor.begin_list_edit(list);
            Descriptor::list_of(plan, list) = contents;
            ctx.editor.end_list_edit(list);
        }
        saved_.clear();
    }

private:
    static void erasePair(optimaru::OperatorContext<PdpRoutePlan>& ctx, PdpRoutePlan& plan,
                          int list, const Delivery& delivery) {
        ctx.editor.begin_list_edit(list);
        auto& route = Descriptor::list_of(plan, list);
        route.erase(std::remove(route.begin(), route.end(), delivery.pickupNodeIndex), route.end());
        route.erase(std::remove(route.begin(), route.end(), delivery.dropoffNodeIndex),
                    route.end());
        ctx.editor.end_list_edit(list);
    }

    static void insertPair(optimaru::OperatorContext<PdpRoutePlan>& ctx, PdpRoutePlan& plan,
                           int list, const Delivery& delivery, int pickupPosition,
                           int dropoffPosition) {
        auto& route = Descriptor::list_of(plan, list);
        const int p = std::clamp(pickupPosition, 0, static_cast<int>(route.size()));
        // Preserve roughly the replaced request's span while guaranteeing
        // strict pickup-before-dropoff after the first insertion shifts it.
        const int d = std::clamp(dropoffPosition, p + 1, static_cast<int>(route.size()) + 1);
        ctx.editor.begin_list_edit(list);
        route.insert(route.begin() + p, delivery.pickupNodeIndex);
        route.insert(route.begin() + d, delivery.dropoffNodeIndex);
        ctx.editor.end_list_edit(list);
    }

    std::vector<std::pair<int, std::vector<int>>> saved_;
    RequestGroups groups_;  // which requests travel together
    bool groupsBuilt_ = false;
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
// related ruin, lifted to request granularity. The requests of a group
// (Delivery::groupIndex) are ruined together, so the recreate may take the
// whole group to another vehicle.
class PairRuin final : public optimaru::RuinOperator<PdpRoutePlan> {
public:
    const char* name() const override { return "pairRuin"; }

    std::vector<int> select_ruin_set(optimaru::OperatorContext<PdpRoutePlan>& ctx, int ruinCount,
                                     std::mt19937_64& rng) override {
        const PdpRoutePlan& plan = ctx.editor.solution();
        const int nodeCount = static_cast<int>(plan.nodes.size());
        if (nodeCount == 0) return {};
        if (!groupsBuilt_) {
            groups_.build(plan);
            groupsBuilt_ = true;
        }

        std::vector<char> taken(plan.deliveries.size(), 0);
        std::vector<int> ruined;
        ruined.reserve(static_cast<std::size_t>(ruinCount));

        const int seed = static_cast<int>(rng() % static_cast<std::uint64_t>(nodeCount));
        addRequestOf(plan, seed, taken, ruined, ruinCount);

        if (ctx.nearby.enabled()) {
            for (int neighbor : ctx.nearby.of(seed)) {
                if (static_cast<int>(ruined.size()) >= ruinCount) break;
                addRequestOf(plan, neighbor, taken, ruined, ruinCount);
            }
        }
        // Top up randomly when the neighbour list runs out before the budget.
        for (int attempt = 0;
             attempt < 4 * ruinCount && static_cast<int>(ruined.size()) < ruinCount; ++attempt) {
            const int node = static_cast<int>(rng() % static_cast<std::uint64_t>(nodeCount));
            addRequestOf(plan, node, taken, ruined, ruinCount);
        }
        return ruined;
    }

private:
    // Adds both stops of the request owning `node`, and of the requests that
    // travel with it, unless already taken. All go in together or not at all —
    // a partial add would defeat the point.
    void addRequestOf(const PdpRoutePlan& plan, int node, std::vector<char>& taken,
                      std::vector<int>& ruined, int ruinCount) const {
        const int unit = groups_.leaderOf(plan.nodes[static_cast<std::size_t>(node)].deliveryIndex);
        if (taken[static_cast<std::size_t>(unit)]) return;
        if (static_cast<int>(ruined.size()) + 2 * groups_.sizeOf(unit) > ruinCount) return;
        taken[static_cast<std::size_t>(unit)] = 1;
        for (int d = unit; d >= 0; d = groups_.nextOf(d)) {
            ruined.push_back(plan.deliveries[static_cast<std::size_t>(d)].pickupNodeIndex);
            ruined.push_back(plan.deliveries[static_cast<std::size_t>(d)].dropoffNodeIndex);
        }
    }

    RequestGroups groups_;  // which requests travel together
    bool groupsBuilt_ = false;
};

}  // namespace

namespace {

// A required vehicle is a raw position in plan.vehicles, and the pair move uses
// it as a list index, so a bad one has to stop here.
void checkRequiredVehicles(const PdpRoutePlan& plan) {
    const int vehicleCount = static_cast<int>(plan.vehicles.size());
    for (const Delivery& delivery : plan.deliveries) {
        if (delivery.requiredVehicleIndex < -1 || delivery.requiredVehicleIndex >= vehicleCount) {
            throw std::invalid_argument("request " + delivery.id + ": requiredVehicleIndex " +
                                        std::to_string(delivery.requiredVehicleIndex) +
                                        " is out of range -1.." + std::to_string(vehicleCount - 1));
        }
    }
}

// The requests of a group ride one vehicle, so two of them cannot be bound to
// different ones. A request is bound by its required vehicle, else by the
// vehicle that pins one of its stops. Such a group could never be whole, and
// the result would be hard-infeasible without a word of why.
void checkRequestGroups(const PdpRoutePlan& plan) {
    RequestGroups groups;
    groups.build(plan);
    std::unordered_map<int, std::pair<int, const Delivery*>> boundBy;  // group -> (vehicle, by)
    for (std::size_t d = 0; d < plan.deliveries.size(); ++d) {
        const Delivery& delivery = plan.deliveries[d];
        if (delivery.groupIndex < -1) {
            throw std::invalid_argument("request " + delivery.id + ": groupIndex " +
                                        std::to_string(delivery.groupIndex) + " is below -1");
        }
        if (delivery.groupIndex < 0) continue;
        const int vehicle = delivery.requiredVehicleIndex >= 0
                                    ? delivery.requiredVehicleIndex
                                    : groups.pinnedOn(static_cast<int>(d));
        if (vehicle < 0) continue;
        const auto& first = boundBy.emplace(delivery.groupIndex, std::make_pair(vehicle, &delivery))
                                    .first->second;
        if (first.first != vehicle) {
            throw std::invalid_argument("requests " + first.second->id + " and " + delivery.id +
                                        " share a group but are bound to different vehicles");
        }
    }
}

// Shared engine wiring for both solve() overloads: termination, late
// acceptance, the sequence move mix, the ALNS knobs and the driving-time nearby
// selection (all pure problem-fact lookups, so they apply to the plugin path
// too).
optimaru::Settings buildEngineConfig(const Solver::Config& config, const PdpRoutePlan& plan) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    engineConfig.parallel_starts = 1;
    if (config.on_new_best) {
        auto callback = config.on_new_best;
        engineConfig.on_new_best = [callback](std::int64_t step, const optimaru::Score& score) {
            callback(step, fromEngineScore(score));
        };
    }

    if (config.target_score.medium != std::numeric_limits<std::int64_t>::max())
        engineConfig.limits.target_score = toEngineScoreLimit(config.target_score);

    // Construction order: place a request's pickup right before its dropoff by
    // giving the pair adjacent difficulty, tightest window first. The stops are
    // still placed one at a time (the engine's CH is element-granular), but
    // consecutive placement means the dropoff is inserted while its pickup is
    // the freshest thing in the route.
    if (config.first_fit_decreasing) {
        const PdpRoutePlan* facts = &plan;
        engineConfig.construction.difficulty = [facts](int nodeIndex) {
            const Node& node = facts->nodes[static_cast<std::size_t>(nodeIndex)];
            const Delivery& delivery =
                    facts->deliveries[static_cast<std::size_t>(node.deliveryIndex)];
            const double window =
                    static_cast<double>(std::max<Seconds>(node.maxEndTime - node.minStartTime, 1));
            // Pickups rank marginally above their dropoff so the pair is laid
            // down in the legal order.
            return static_cast<double>(delivery.demand) + 1e6 / window + (node.pickup ? 1e-3 : 0.0);
        };
    }

    auto& acceptor = engineConfig.search.acceptance;
    acceptor.rule = config.rule;
    acceptor.history_length =
            config.history_length != 0
                    ? config.history_length
                    : (plan.nodes.size() > 60
                               ? 5
                               : std::clamp<std::size_t>(4 * plan.nodes.size(), 200, 1600));
    acceptor.rrt_start_gap = config.record_to_record_start_gap;
    acceptor.rrt_end_gap = config.record_to_record_end_gap;

    auto& moves = engineConfig.search.moves;
    moves.relocate_weight = config.relocate_weight;
    moves.list_swap_weight = config.list_swap_weight;
    moves.two_opt_weight = config.two_opt_weight;
    moves.or_opt_weight = config.or_opt_weight;
    moves.tail_swap_weight = config.tail_swap_weight;
    moves.cross_exchange_weight = config.cross_exchange_weight;
    moves.cross_exchange_max_run = config.cross_exchange_max_run;

    engineConfig.search.descent_weight = config.descent_weight;
    engineConfig.search.descent_max_sweeps = config.descent_max_sweeps;
    engineConfig.search.route_elimination_weight = config.route_elimination_weight;
    engineConfig.search.route_elimination_max_size = config.route_elimination_max_size;
    // The ruin & recreate knobs that are this use case's own (settings_from
    // set the shared ones).
    auto& lns = engineConfig.search.ruin_recreate;
    lns.selection_type = config.ruin_recreate_selection_type;
    lns.selection_segment_length = config.ruin_recreate_selection_segment_length;
    lns.selection_alpha = config.ruin_recreate_selection_alpha;
    lns.sisr_ruin = config.sisr_ruin;
    lns.route_ruin = config.route_ruin;
    lns.sisr_max_string_removals = config.sisr_max_string_removals;
    lns.sisr_max_string_size = config.sisr_max_string_size;
    lns.regret_k = config.regret_k;
    lns.blink_rate = config.blink_rate;

    if (config.driving_time_nearby && plan.nodes.size() > 1) {
        auto locationOfNode = std::make_shared<std::vector<int>>();
        locationOfNode->reserve(plan.nodes.size());
        for (const Node& node : plan.nodes) locationOfNode->push_back(node.locationIndex);
        auto matrix = std::make_shared<std::vector<std::vector<Seconds>>>(plan.drivingSeconds);
        engineConfig.search.nearby.distance = [locationOfNode, matrix](int a, int b) {
            return static_cast<double>((*matrix)[static_cast<std::size_t>((*locationOfNode)[a])]
                                                [static_cast<std::size_t>((*locationOfNode)[b])]);
        };
        engineConfig.search.nearby.size = std::max(
                1,
                std::min(config.driving_time_nearby_size, static_cast<int>(plan.nodes.size()) - 1));
        engineConfig.search.nearby.probability = config.driving_time_nearby_probability;
    }

    return engineConfig;
}

// The request-level operators both solve() overloads register. They read the
// plan (which request a stop belongs to, the windows, the vehicle limits),
// never the calculator, so a caller's constraint assembly keeps them.
void addRequestOperators(optimaru::Solver<PdpRoutePlan>& engine, const Solver::Config& config,
                         const PdpRoutePlan& plan) {
    if (config.pair_moves) {
        engine.add_move_operator(std::make_unique<PairRelocateMove>(
                config.pair_repair_probability, config.pair_assignment_probability));
    }
    if (config.pair_exchange_moves &&
        static_cast<int>(plan.nodes.size()) >= config.pair_exchange_min_nodes) {
        engine.add_move_operator(std::make_unique<PairExchangeMove>());
    }
    RequestMoveSettings moves;
    moves.ruinRecreate = config.request_ruin_recreate;
    moves.ruinMaxRequests = config.request_ruin_max_requests;
    moves.regretK = config.regret_k;
    moves.blinkRate = config.blink_rate;
    moves.bundleMoves = config.bundle_moves;
    moves.bundleMaxRequests = config.bundle_max_requests;
    for (auto& move : makeRequestMoves(moves)) engine.add_move_operator(std::move(move));
    if (config.pair_ruin) engine.add_ruin_operator(std::make_unique<PairRuin>());
}

// The domain Score of a plugin calculator, read from its three accumulators
// (level_scores(), before the medium fold) rather than decoded from the folded
// engine soft, which would file a soft total of 10^9 or more under medium.
Score levelScoreOf(const optimaru::DecomposedObjective<PdpRoutePlan>& calculator) {
    const auto levels = calculator.level_scores();
    return {static_cast<std::int64_t>(levels.hard),
            optimaru::round_score_level(levels.medium, "medium"),
            optimaru::round_score_level(levels.soft, "soft")};
}

}  // namespace

void Solver::solve(PdpRoutePlan& plan) {
    checkRequiredVehicles(plan);
    checkRequestGroups(plan);
    // Empty pool so the construction heuristic places every stop itself.
    if (config_.pair_construction_heuristic && !plan.vehicles.empty())
        constructPairs(plan, config_.first_fit_decreasing);
    else
        plan.unassignedNodeIndices.clear();

    if (plan.vehicles.empty() || plan.nodes.empty()) {
        ScoreCalculator::score(plan);
        return;
    }
    optimaru::check_config(config_);

    // Parallel starts of this solver's own search: the request-level operators
    // are registered per engine, so the engine's multi-start would run without
    // them. Each start is this whole solve again, on its copy, with one start
    // (so it takes the single-start path below).
    if (config_.parallel_solver_count > 1) {
        const auto solveOne = [](const Config& config, PdpRoutePlan& candidate, int) {
            Solver(config).solve(candidate);
        };
        optimaru::solve_parallel_starts(config_, plan, solveOne);
        return;
    }

    optimaru::Settings engineConfig = buildEngineConfig(config_, plan);
    PdpIncrementalScoreCalculator calculator;
    optimaru::Solver<PdpRoutePlan> engine(std::move(engineConfig));
    if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
    addRequestOperators(engine, config_, plan);
    engine.solve(plan, calculator);

    plan.score = ScoreCalculator::score(plan);
}

void Solver::solve(PdpRoutePlan& plan, optimaru::DecomposedObjective<PdpRoutePlan>& calculator) {
    checkRequiredVehicles(plan);
    checkRequestGroups(plan);
    optimaru::check_config(config_);
    if (config_.pair_construction_heuristic && !plan.vehicles.empty())
        constructPairs(plan, config_.first_fit_decreasing);
    if (!plan.nodes.empty() && !plan.vehicles.empty()) {
        optimaru::Settings engineConfig = buildEngineConfig(config_, plan);
        if (config_.on_new_best) {
            // The engine reports a new best right after the working solution
            // became it, so the calculator's accumulators hold exactly that
            // solution: report its levels the same way as the final
            // plan.score below, not the decoded engine fold.
            auto callback = config_.on_new_best;
            engineConfig.on_new_best = [callback, &calculator](std::int64_t step,
                                                               const optimaru::Score&) {
                callback(step, levelScoreOf(calculator));
            };
        }
        // The request-level operators only read the plan (which request a
        // node belongs to, the windows, the vehicle limits), never the
        // calculator, so the plugin path keeps them — they are what makes a
        // PDP search work.
        optimaru::Solver<PdpRoutePlan> engine(std::move(engineConfig));
        if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
        addRequestOperators(engine, config_, plan);
        engine.solve(plan, calculator);
    }

    // Refresh the node derived fields (vehicleIndex / arrivalTime) on the
    // best solution — the plugin components derive their own chain and never
    // write these — then score it with the caller's constraint assembly,
    // leaving the calculator primed on it so constraint_totals() explains
    // exactly this score.
    ScoreCalculator::updateShadowVariables(plan);
    calculator.rebuild(plan);
    plan.score = levelScoreOf(calculator);
}

}  // namespace pdp
