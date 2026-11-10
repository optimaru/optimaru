#include "pdp/request_moves.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "pdp/request_insertion.hpp"
#include "optimaru/model/solution_traits.hpp"
#include "optimaru/portable_random.hpp"

namespace pdp {

namespace {

using Descriptor = optimaru::SolutionTraits<PdpRoutePlan>;
using Context = optimaru::OperatorContext<PdpRoutePlan>;
using optimaru::random_index;

// A stop taken out of its route and not placed yet.
constexpr int kDetached = -2;
// Up to this many candidate vehicles a request is offered to all of them;
// above, only to the idle ones and the routes that hold one of its nearby stops.
constexpr std::size_t kOfferEveryVehicleUpTo = 32;
// Requests of the unassigned pool one reinsertion tries to route.
constexpr int kPooledPerRepair = 2;
// Up to this many requests the worst removal prices all of them; above, a
// sample.
constexpr int kPriceEveryRequestUpTo = 256;

double unitDraw(std::mt19937_64& rng) { return static_cast<double>(rng() >> 11) * 0x1.0p-53; }

// ---------------------------------------------------------------------------
// RequestWorkspace
// ---------------------------------------------------------------------------
// What the request moves share: working copies of the routes with their
// timelines, and the plan-then-commit protocol.
//
// A move works on the copies only. Taking requests out and putting them back is
// plain vector work there, and every candidate slot is judged from the
// timelines, so nothing reaches the objective until commit() writes each
// changed route through the editor, once. undo() writes the old routes back.
//
// The copies survive between moves and are compared with the plan when the
// next one begins, so whatever else changed the plan in between (the engine's
// own moves, a kick, a rejected move's undo) is picked up there.
class RequestWorkspace {
public:
    explicit RequestWorkspace(const RequestMoveSettings& settings) : settings_(settings) {}

    const RequestMoveSettings& settings() const { return settings_; }
    const PdpRoutePlan& plan() const { return *plan_; }
    int vehicleCount() const { return vehicleCount_; }
    int deliveryCount() const { return static_cast<int>(plan_->deliveries.size()); }

    // --- one move ----------------------------------------------------------

    void begin(Context& ctx) {
        ctx_ = &ctx;
        plan_ = &ctx.editor.solution();
        const PdpRoutePlan& plan = *plan_;
        vehicleCount_ = static_cast<int>(plan.vehicles.size());
        const auto vehicles = static_cast<std::size_t>(vehicleCount_);
        if (routes_.size() != vehicles) {
            routes_.resize(vehicles);
            for (std::size_t v = 0; v < vehicles; ++v) routes_[v] = plan.vehicles[v].nodeIndices;
            timelines_.rebuild(plan);
            dirty_.assign(vehicles, 0);
            stale_.assign(vehicles, 0);
            nearStamp_.assign(vehicles, 0);
            taken_.assign(plan.deliveries.size(), 0);
            pinned_.assign(plan.deliveries.size(), 0);
            for (const Vehicle& vehicle : plan.vehicles) {
                for (int nodeIndex : vehicle.pinnedNodeIndices) {
                    pinned_[static_cast<std::size_t>(
                            plan.nodes[static_cast<std::size_t>(nodeIndex)].deliveryIndex)] = 1;
                }
            }
            dirtyList_.clear();
            staleList_.clear();
        } else {
            for (int v : dirtyList_) dirty_[static_cast<std::size_t>(v)] = 0;
            dirtyList_.clear();
            for (std::size_t v = 0; v < vehicles; ++v) {
                if (routes_[v] != plan.vehicles[v].nodeIndices) {
                    routes_[v] = plan.vehicles[v].nodeIndices;
                    markStale(static_cast<int>(v));
                }
            }
        }
        routeOf_.assign(ctx.element_list.begin(), ctx.element_list.end());
        for (int deliveryIndex : pending_) taken_[static_cast<std::size_t>(deliveryIndex)] = 0;
        pending_.clear();
        moved_.clear();
        saved_.clear();
    }

    // Where a request is. Read from the engine's element index, so it holds
    // until the move's first detach().
    enum class Where { Routed, Pooled, Broken };

    Where where(int deliveryIndex) const {
        const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
        const int pickupList =
                ctx_->element_list[static_cast<std::size_t>(delivery.pickupNodeIndex)];
        const int dropoffList =
                ctx_->element_list[static_cast<std::size_t>(delivery.dropoffNodeIndex)];
        if (pickupList == vehicleCount_ && dropoffList == vehicleCount_) return Where::Pooled;
        if (pickupList == dropoffList && pickupList >= 0 && pickupList < vehicleCount_ &&
            positionOf(delivery.pickupNodeIndex) < positionOf(delivery.dropoffNodeIndex)) {
            return Where::Routed;
        }
        return Where::Broken;
    }

    // The vehicle and the slot of a stop when the move began.
    int listOf(int nodeIndex) const {
        return ctx_->element_list[static_cast<std::size_t>(nodeIndex)];
    }
    int positionOf(int nodeIndex) const {
        return ctx_->element_position[static_cast<std::size_t>(nodeIndex)];
    }
    bool onVehicle(int list) const { return list >= 0 && list < vehicleCount_; }
    // A request with a pinned stop stays where it is: its vehicle's route must
    // start with that stop, so the request cannot be taken out and put back.
    bool movable(int deliveryIndex) const {
        return !pinned_[static_cast<std::size_t>(deliveryIndex)];
    }

    // --- changing the working routes -----------------------------------------

    void detach(int deliveryIndex) {
        const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
        for (int nodeIndex : {delivery.pickupNodeIndex, delivery.dropoffNodeIndex}) {
            int& list = routeOf_[static_cast<std::size_t>(nodeIndex)];
            if (onVehicle(list)) {
                std::vector<int>& route = routes_[static_cast<std::size_t>(list)];
                const auto at = std::find(route.begin(), route.end(), nodeIndex);
                if (at != route.end()) route.erase(at);
                touch(list);
            }
            list = kDetached;
        }
        moved_.push_back(deliveryIndex);
    }

    void place(int deliveryIndex, const RequestInsertion& insertion) {
        const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
        std::vector<int>& route = routes_[static_cast<std::size_t>(insertion.vehicle)];
        route.insert(route.begin() + insertion.pickupPosition, delivery.pickupNodeIndex);
        route.insert(route.begin() + insertion.dropoffPosition, delivery.dropoffNodeIndex);
        routeOf_[static_cast<std::size_t>(delivery.pickupNodeIndex)] = insertion.vehicle;
        routeOf_[static_cast<std::size_t>(delivery.dropoffNodeIndex)] = insertion.vehicle;
        touch(insertion.vehicle);
    }

    // No route takes the request: it ends the move in the unassigned pool.
    void leaveOut(int deliveryIndex) {
        const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
        routeOf_[static_cast<std::size_t>(delivery.pickupNodeIndex)] = vehicleCount_;
        routeOf_[static_cast<std::size_t>(delivery.dropoffNodeIndex)] = vehicleCount_;
    }

    // --- where a detached request fits -------------------------------------

    RequestInsertion bestOn(int deliveryIndex, int vehicle, Blinker& blinker) {
        refreshTimelines();
        RequestInsertion best;
        const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
        if (delivery.requiredVehicleIndex >= 0 && delivery.requiredVehicleIndex != vehicle) {
            return best;
        }
        timelines_.of(vehicle).improve(*plan_, delivery, blinker, best);
        return best;
    }

    // The vehicles a detached request is offered to. On a large fleet the
    // offer is narrowed to the idle vehicles and the routes that hold one of
    // the request's nearby stops; returns whether it was.
    bool candidateVehicles(int deliveryIndex, std::vector<int>& out) {
        refreshTimelines();
        const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
        timelines_.candidateVehicles(*plan_, delivery, out);
        if (out.size() <= kOfferEveryVehicleUpTo || !ctx_->nearby.enabled()) return false;
        const std::size_t offered = out.size();
        ++nearEpoch_;
        for (int nodeIndex : {delivery.pickupNodeIndex, delivery.dropoffNodeIndex}) {
            for (int neighbor : ctx_->nearby.of(nodeIndex)) {
                const int list = routeOf_[static_cast<std::size_t>(neighbor)];
                if (onVehicle(list)) nearStamp_[static_cast<std::size_t>(list)] = nearEpoch_;
            }
        }
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](int v) {
                                     return timelines_.of(v).size() > 0 &&
                                            nearStamp_[static_cast<std::size_t>(v)] != nearEpoch_;
                                 }),
                  out.end());
        return out.size() < offered;
    }

    // The cheapest slot among the vehicles the request is offered to. When
    // the narrowed offer has no room, the rest of the fleet is asked before
    // the request is given up: a request left out for want of looking would
    // cost the whole move.
    RequestInsertion bestAnywhere(int deliveryIndex, Blinker& blinker) {
        const bool narrowed = candidateVehicles(deliveryIndex, candidateScratch_);
        RequestInsertion best;
        const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
        for (int v : candidateScratch_) timelines_.of(v).improve(*plan_, delivery, blinker, best);
        if (!best.found() && narrowed) best = bestInFleet(deliveryIndex);
        return best;
    }

    // The cheapest slot over every vehicle that may take the request, with no
    // blinking.
    RequestInsertion bestInFleet(int deliveryIndex) {
        refreshTimelines();
        const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
        timelines_.candidateVehicles(*plan_, delivery, candidateScratch_);
        Blinker every;
        RequestInsertion best;
        for (int v : candidateScratch_) timelines_.of(v).improve(*plan_, delivery, every, best);
        return best;
    }

    // --- choosing what to take out ------------------------------------------
    // Each fills pending() with up to `count` requests that are on a route (or
    // broken across lists). Read the plan as it was when the move began.

    const std::vector<int>& pending() const { return pending_; }

    bool take(int deliveryIndex) {
        char& taken = taken_[static_cast<std::size_t>(deliveryIndex)];
        if (taken || !movable(deliveryIndex) || where(deliveryIndex) == Where::Pooled) {
            return false;
        }
        taken = 1;
        pending_.push_back(deliveryIndex);
        return true;
    }

    bool takeRandom(std::mt19937_64& rng) {
        for (int attempt = 0; attempt < 16; ++attempt) {
            if (take(random_index(rng, deliveryCount()))) return true;
        }
        return false;
    }

    // A request that is whole on one route and free to leave it, or -1.
    int randomRouted(std::mt19937_64& rng) const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const int deliveryIndex = random_index(rng, deliveryCount());
            if (movable(deliveryIndex) && where(deliveryIndex) == Where::Routed) {
                return deliveryIndex;
            }
        }
        return -1;
    }

    void takeAtRandom(int count, std::mt19937_64& rng) {
        for (int attempt = 0; attempt < 4 * count + 8 && pendingCount() < count; ++attempt) {
            take(random_index(rng, deliveryCount()));
        }
    }

    // Shaw's related removal: grow the set from a random request through the
    // requests of the stops nearest to one already in it.
    void takeRelated(int count, std::mt19937_64& rng) {
        if (!ctx_->nearby.enabled()) return takeAtRandom(count, rng);
        if (!takeRandom(rng)) return;
        while (pendingCount() < count) {
            const Delivery& from = plan_->deliveries[static_cast<std::size_t>(
                    pending_[static_cast<std::size_t>(random_index(rng, pendingCount()))])];
            const std::vector<int>& nearPickup = ctx_->nearby.of(from.pickupNodeIndex);
            const std::vector<int>& nearDropoff = ctx_->nearby.of(from.dropoffNodeIndex);
            const std::size_t reach = std::max(nearPickup.size(), nearDropoff.size());
            bool added = false;
            for (std::size_t n = 0; n < reach && !added; ++n) {
                for (const std::vector<int>* list : {&nearPickup, &nearDropoff}) {
                    if (n >= list->size()) continue;
                    // Not always the nearest: the same seed should not take the
                    // same set every time.
                    if (random_index(rng, 4) == 0) continue;
                    const int other =
                            plan_->nodes[static_cast<std::size_t>((*list)[n])].deliveryIndex;
                    if (take(other)) {
                        added = true;
                        break;
                    }
                }
            }
            if (!added && !takeRandom(rng)) return;
        }
    }

    // Worst removal: the requests whose stops cost the most driving where they
    // are, picked with Ropke & Pisinger's bias towards the top of the ranking.
    void takeWorst(int count, std::mt19937_64& rng) {
        refreshTimelines();
        gains_.clear();
        const auto price = [&](int deliveryIndex) {
            if (where(deliveryIndex) != Where::Routed) return;
            const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
            const Seconds gain = timelines_.of(listOf(delivery.pickupNodeIndex))
                                         .removalGain(*plan_, positionOf(delivery.pickupNodeIndex),
                                                      positionOf(delivery.dropoffNodeIndex));
            gains_.emplace_back(gain, deliveryIndex);
        };
        if (deliveryCount() <= kPriceEveryRequestUpTo) {
            for (int d = 0; d < deliveryCount(); ++d) price(d);
        } else {
            for (int sample = 0; sample < 8 * count; ++sample) {
                price(random_index(rng, deliveryCount()));
            }
        }
        std::sort(gains_.begin(), gains_.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        while (pendingCount() < count && !gains_.empty()) {
            const double biased = std::pow(unitDraw(rng), 3.0);
            const auto index =
                    static_cast<std::size_t>(biased * static_cast<double>(gains_.size()));
            take(gains_[index].second);
            gains_.erase(gains_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    // The requests of one route, from a random stop on (the whole route when
    // it has no more than `count`).
    void takeRoute(int count, std::mt19937_64& rng) {
        const int seed = randomRouted(rng);
        if (seed < 0) return takeAtRandom(count, rng);
        const int vehicle =
                listOf(plan_->deliveries[static_cast<std::size_t>(seed)].pickupNodeIndex);
        const std::vector<int>& route =
                plan_->vehicles[static_cast<std::size_t>(vehicle)].nodeIndices;
        const int size = static_cast<int>(route.size());
        const int start = random_index(rng, size);
        for (int step = 0; step < size && pendingCount() < count; ++step) {
            take(plan_->nodes[static_cast<std::size_t>(
                                      route[static_cast<std::size_t>((start + step) % size)])]
                         .deliveryIndex);
        }
    }

    // String removal: the requests with a stop in one stretch of a route, then
    // in a stretch of the routes of nearby stops.
    void takeStrings(int count, std::mt19937_64& rng) {
        const int seed = randomRouted(rng);
        if (seed < 0) return takeAtRandom(count, rng);
        const int seedNode = plan_->deliveries[static_cast<std::size_t>(seed)].pickupNodeIndex;
        ++nearEpoch_;
        takeStretch(seedNode, count, rng);
        if (!ctx_->nearby.enabled()) return;
        for (int neighbor : ctx_->nearby.of(seedNode)) {
            if (pendingCount() >= count) break;
            const int list = listOf(neighbor);
            if (!onVehicle(list) || nearStamp_[static_cast<std::size_t>(list)] == nearEpoch_) {
                continue;
            }
            takeStretch(neighbor, count, rng);
        }
    }

    // Adds up to `limit` requests waiting in the unassigned pool to pending().
    void takePooled(int limit, std::mt19937_64& rng) {
        const std::vector<int>& pool = plan_->unassignedNodeIndices;
        if (pool.empty()) return;
        for (int attempt = 0; attempt < 2 * limit && limit > 0; ++attempt) {
            const int nodeIndex = pool[static_cast<std::size_t>(
                    random_index(rng, static_cast<int>(pool.size())))];
            const int deliveryIndex =
                    plan_->nodes[static_cast<std::size_t>(nodeIndex)].deliveryIndex;
            char& taken = taken_[static_cast<std::size_t>(deliveryIndex)];
            if (taken || where(deliveryIndex) != Where::Pooled) continue;
            taken = 1;
            pending_.push_back(deliveryIndex);
            --limit;
        }
    }

    // The requests whose pickups follow each other on `seed`'s route, `seed`
    // first: a bundle of up to `size`.
    void bundleFrom(int seed, int size, std::vector<int>& bundle) const {
        bundle.clear();
        const Delivery& first = plan_->deliveries[static_cast<std::size_t>(seed)];
        const std::vector<int>& route =
                plan_->vehicles[static_cast<std::size_t>(listOf(first.pickupNodeIndex))]
                        .nodeIndices;
        for (std::size_t p = static_cast<std::size_t>(positionOf(first.pickupNodeIndex));
             p < route.size() && static_cast<int>(bundle.size()) < size; ++p) {
            const Node& node = plan_->nodes[static_cast<std::size_t>(route[p])];
            if (node.pickup && movable(node.deliveryIndex) &&
                where(node.deliveryIndex) == Where::Routed) {
                bundle.push_back(node.deliveryIndex);
            }
        }
    }

    // A stop near one of the request's two that rides another vehicle than
    // `from`, or -1.
    int neighborElsewhere(int deliveryIndex, int from, std::mt19937_64& rng) const {
        if (!ctx_->nearby.enabled()) return -1;
        const Delivery& delivery = plan_->deliveries[static_cast<std::size_t>(deliveryIndex)];
        const bool pickupFirst = (rng() & 1U) == 0;
        for (int side = 0; side < 2; ++side) {
            const std::vector<int>& neighbors =
                    ctx_->nearby.of((side == 0) == pickupFirst ? delivery.pickupNodeIndex
                                                               : delivery.dropoffNodeIndex);
            if (neighbors.empty()) continue;
            const std::size_t start =
                    static_cast<std::size_t>(random_index(rng, static_cast<int>(neighbors.size())));
            for (std::size_t n = 0; n < neighbors.size(); ++n) {
                const int neighbor = neighbors[(start + n) % neighbors.size()];
                const int list = listOf(neighbor);
                if (onVehicle(list) && list != from) return neighbor;
            }
        }
        return -1;
    }

    // --- putting pending() back -----------------------------------------------

    void detachPending() {
        for (int deliveryIndex : pending_) detach(deliveryIndex);
    }

    void shufflePending(std::mt19937_64& rng) {
        std::shuffle(pending_.begin(), pending_.end(), rng);
    }

    // Cheapest slot first, in the order given.
    void reinsertInOrder(Blinker& blinker) {
        for (int deliveryIndex : pending_) {
            const RequestInsertion insertion = bestAnywhere(deliveryIndex, blinker);
            if (insertion.found()) {
                place(deliveryIndex, insertion);
            } else {
                leaveOut(deliveryIndex);
            }
        }
    }

    // Regret-k: place next the request that loses the most if it has to settle
    // for its k-th best route, a request with fewer than k routes first.
    void reinsertByRegret(int k, Blinker& blinker) {
        const std::size_t count = pending_.size();
        const auto vehicles = static_cast<std::size_t>(vehicleCount_);
        options_.assign(count * vehicles, RequestInsertion{});
        offered_.assign(count * vehicles, 0);
        placed_.assign(count, 0);
        narrowed_.assign(count, 0);
        for (std::size_t i = 0; i < count; ++i) offer(i, blinker);

        for (std::size_t round = 0; round < count; ++round) {
            std::size_t choice = count;
            int choiceRoutes = 0;
            Seconds choiceRegret = 0;
            Seconds choiceBest = 0;
            int choiceVehicle = -1;
            for (std::size_t i = 0; i < count; ++i) {
                if (placed_[i]) continue;
                // The k cheapest routes of request i.
                cheapest_.clear();
                int bestVehicle = -1;
                for (std::size_t v = 0; v < vehicles; ++v) {
                    const RequestInsertion& option = options_[i * vehicles + v];
                    if (!option.found()) continue;
                    if (bestVehicle < 0 ||
                        option.addedTravel <
                                options_[i * vehicles + static_cast<std::size_t>(bestVehicle)]
                                        .addedTravel) {
                        bestVehicle = static_cast<int>(v);
                    }
                    const auto at = std::upper_bound(cheapest_.begin(), cheapest_.end(),
                                                     option.addedTravel);
                    if (at != cheapest_.end() || static_cast<int>(cheapest_.size()) < k) {
                        cheapest_.insert(at, option.addedTravel);
                        if (static_cast<int>(cheapest_.size()) > k) cheapest_.pop_back();
                    }
                }
                if (bestVehicle < 0) {
                    // No offered route has room. A narrowed offer asks the rest
                    // of the fleet; what it finds is the request's one option.
                    const RequestInsertion last =
                            narrowed_[i] ? bestInFleet(pending_[i]) : RequestInsertion{};
                    if (!last.found()) {
                        // Routes only fill up during a repair: it will not fit
                        // later either.
                        leaveOut(pending_[i]);
                        placed_[i] = 1;
                        continue;
                    }
                    const std::size_t cell = i * vehicles + static_cast<std::size_t>(last.vehicle);
                    options_[cell] = last;
                    offered_[cell] = 1;
                    bestVehicle = last.vehicle;
                    cheapest_.assign(1, last.addedTravel);
                }
                const int routes = static_cast<int>(cheapest_.size());
                Seconds regret = 0;
                for (Seconds cost : cheapest_) regret += cost - cheapest_.front();
                const bool moreUrgent =
                        choice == count || routes < choiceRoutes ||
                        (routes == choiceRoutes &&
                         (regret > choiceRegret ||
                          (regret == choiceRegret && cheapest_.front() < choiceBest)));
                if (moreUrgent) {
                    choice = i;
                    choiceRoutes = routes;
                    choiceRegret = regret;
                    choiceBest = cheapest_.front();
                    choiceVehicle = bestVehicle;
                }
            }
            if (choice == count) return;

            refreshTimelines();
            const bool wasIdle = timelines_.of(choiceVehicle).size() == 0;
            place(pending_[choice],
                  options_[choice * vehicles + static_cast<std::size_t>(choiceVehicle)]);
            placed_[choice] = 1;
            for (std::size_t i = 0; i < count; ++i) {
                if (placed_[i]) continue;
                const std::size_t cell = i * vehicles + static_cast<std::size_t>(choiceVehicle);
                if (offered_[cell]) options_[cell] = bestOn(pending_[i], choiceVehicle, blinker);
                // Another idle vehicle now stands in for the one just used.
                if (wasIdle) offer(i, blinker);
            }
        }
    }

    // --- commit / undo --------------------------------------------------------

    // Writes the changed routes (and the pool) through the editor. Returns
    // false, with the plan untouched, when the move changed nothing.
    bool commit(Context& ctx, std::vector<int>& entities) {
        PdpRoutePlan& plan = ctx.editor.solution();
        const int pool = vehicleCount_;
        bool poolChanged = false;
        for (int deliveryIndex : moved_) {
            const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(deliveryIndex)];
            for (int nodeIndex : {delivery.pickupNodeIndex, delivery.dropoffNodeIndex}) {
                int& list = routeOf_[static_cast<std::size_t>(nodeIndex)];
                if (list == kDetached) list = pool;  // never placed: it waits
                if ((listOf(nodeIndex) == pool) != (list == pool)) poolChanged = true;
            }
        }
        bool changed = poolChanged;
        for (int v : dirtyList_) {
            if (routes_[static_cast<std::size_t>(v)] !=
                plan.vehicles[static_cast<std::size_t>(v)].nodeIndices) {
                changed = true;
                break;
            }
        }
        if (!changed) return false;

        for (int v : dirtyList_) {
            std::vector<int>& route = plan.vehicles[static_cast<std::size_t>(v)].nodeIndices;
            if (routes_[static_cast<std::size_t>(v)] == route) continue;
            ctx.editor.begin_list_edit(v);
            saved_.emplace_back(v, std::move(route));
            route = routes_[static_cast<std::size_t>(v)];
            ctx.editor.end_list_edit(v);
        }
        if (poolChanged) {
            std::vector<int>& waiting = plan.unassignedNodeIndices;
            ctx.editor.begin_list_edit(pool);
            saved_.emplace_back(pool, waiting);
            waiting.erase(std::remove_if(waiting.begin(), waiting.end(),
                                         [&](int nodeIndex) {
                                             return routeOf_[static_cast<std::size_t>(nodeIndex)] !=
                                                    pool;
                                         }),
                          waiting.end());
            for (int deliveryIndex : moved_) {
                const Delivery& delivery = plan.deliveries[static_cast<std::size_t>(deliveryIndex)];
                for (int nodeIndex : {delivery.pickupNodeIndex, delivery.dropoffNodeIndex}) {
                    if (routeOf_[static_cast<std::size_t>(nodeIndex)] == pool &&
                        listOf(nodeIndex) != pool) {
                        waiting.push_back(nodeIndex);
                    }
                }
            }
            ctx.editor.end_list_edit(pool);
        }
        // One entity per moved request (its pickup) for the acceptor's tabu
        // memory: the two stops of a request never move apart here.
        for (int deliveryIndex : moved_) {
            entities.push_back(
                    plan.deliveries[static_cast<std::size_t>(deliveryIndex)].pickupNodeIndex);
        }
        return true;
    }

    void undo(Context& ctx) {
        PdpRoutePlan& plan = ctx.editor.solution();
        for (auto& [list, contents] : saved_) {
            ctx.editor.begin_list_edit(list);
            Descriptor::list_of(plan, list) = std::move(contents);
            ctx.editor.end_list_edit(list);
        }
        saved_.clear();
    }

private:
    int pendingCount() const { return static_cast<int>(pending_.size()); }

    void markStale(int vehicle) {
        char& stale = stale_[static_cast<std::size_t>(vehicle)];
        if (stale) return;
        stale = 1;
        staleList_.push_back(vehicle);
    }

    void touch(int vehicle) {
        char& dirty = dirty_[static_cast<std::size_t>(vehicle)];
        if (!dirty) {
            dirty = 1;
            dirtyList_.push_back(vehicle);
        }
        markStale(vehicle);
    }

    void refreshTimelines() {
        for (int v : staleList_) {
            timelines_.rebuildRoute(*plan_, v, routes_[static_cast<std::size_t>(v)]);
            stale_[static_cast<std::size_t>(v)] = 0;
        }
        staleList_.clear();
    }

    // The requests with a stop in a random stretch of the route around
    // `nodeIndex`, until pending() holds `count`.
    void takeStretch(int nodeIndex, int count, std::mt19937_64& rng) {
        if (pendingCount() >= count) return;  // nothing left to take
        const int list = listOf(nodeIndex);
        nearStamp_[static_cast<std::size_t>(list)] = nearEpoch_;
        const std::vector<int>& route = plan_->vehicles[static_cast<std::size_t>(list)].nodeIndices;
        const int size = static_cast<int>(route.size());
        const int length = 1 + random_index(rng, std::min(size, 2 * (count - pendingCount())));
        const int start =
                std::clamp(positionOf(nodeIndex) - random_index(rng, length), 0, size - length);
        for (int p = start; p < start + length && pendingCount() < count; ++p) {
            take(plan_->nodes[static_cast<std::size_t>(route[static_cast<std::size_t>(p)])]
                         .deliveryIndex);
        }
    }

    // (Re)computes request i's option on every vehicle it is offered to.
    void offer(std::size_t i, Blinker& blinker) {
        const auto vehicles = static_cast<std::size_t>(vehicleCount_);
        narrowed_[i] = candidateVehicles(pending_[i], candidateScratch_) ? 1 : 0;
        for (int v : candidateScratch_) {
            const std::size_t cell = i * vehicles + static_cast<std::size_t>(v);
            if (offered_[cell]) continue;
            offered_[cell] = 1;
            options_[cell] = bestOn(pending_[i], v, blinker);
        }
    }

    RequestMoveSettings settings_;
    Context* ctx_ = nullptr;
    const PdpRoutePlan* plan_ = nullptr;
    int vehicleCount_ = 0;

    std::vector<std::vector<int>> routes_;  // working copy, per vehicle
    PlanTimelines timelines_;               // of the working copies
    std::vector<char> stale_;               // timeline behind its working route
    std::vector<int> staleList_;
    std::vector<int> routeOf_;  // stop -> list on the working copies
    std::vector<char> dirty_;   // working route changed by this move
    std::vector<int> dirtyList_;

    std::vector<int> pending_;  // requests this move takes out and reinserts
    std::vector<char> taken_;   // request is in pending_
    std::vector<char> pinned_;  // request has a pinned stop
    std::vector<int> moved_;    // requests detached by this move
    std::vector<std::pair<int, std::vector<int>>> saved_;  // lists as they were, for undo

    // Scratch.
    std::vector<int> candidateScratch_;
    std::vector<std::uint32_t> nearStamp_;
    std::uint32_t nearEpoch_ = 0;
    std::vector<std::pair<Seconds, int>> gains_;
    std::vector<RequestInsertion> options_;
    std::vector<char> offered_;
    std::vector<char> placed_;
    std::vector<char> narrowed_;  // request's offer left part of the fleet out
    std::vector<Seconds> cheapest_;
};

// ---------------------------------------------------------------------------
// RequestRuinRecreateMove
// ---------------------------------------------------------------------------
class RequestRuinRecreateMove final : public optimaru::MoveOperator<PdpRoutePlan> {
public:
    explicit RequestRuinRecreateMove(std::shared_ptr<RequestWorkspace> workspace)
        : workspace_(std::move(workspace)) {}

    const char* name() const override { return "requestRuinRecreate"; }

    bool apply(Context& ctx, std::mt19937_64& rng, std::vector<int>& entities) override {
        RequestWorkspace& work = *workspace_;
        work.begin(ctx);
        if (work.deliveryCount() == 0 || work.vehicleCount() == 0) return false;
        const RequestMoveSettings& settings = work.settings();

        const int most = std::clamp(settings.ruinMaxRequests, 1,
                                    std::max(1, (2 * work.deliveryCount()) / 5));
        const int count = 1 + random_index(rng, most);
        switch (random_index(rng, 5)) {
            case 0:
                work.takeAtRandom(count, rng);
                break;
            case 1:
                work.takeRelated(count, rng);
                break;
            case 2:
                work.takeWorst(count, rng);
                break;
            case 3:
                work.takeRoute(count, rng);
                break;
            default:
                work.takeStrings(count, rng);
                break;
        }
        work.takePooled(kPooledPerRepair, rng);
        if (work.pending().empty()) return false;
        work.detachPending();

        Blinker blinker(settings.blinkRate, rng);
        if (settings.regretK >= 2 && work.pending().size() > 1 && (rng() & 1U) == 0) {
            work.reinsertByRegret(settings.regretK, blinker);
        } else {
            work.shufflePending(rng);
            work.reinsertInOrder(blinker);
        }
        return work.commit(ctx, entities);
    }

    void undo_last(Context& ctx) override { workspace_->undo(ctx); }

private:
    std::shared_ptr<RequestWorkspace> workspace_;
};

// ---------------------------------------------------------------------------
// BundleRelocateMove
// ---------------------------------------------------------------------------
class BundleRelocateMove final : public optimaru::MoveOperator<PdpRoutePlan> {
public:
    explicit BundleRelocateMove(std::shared_ptr<RequestWorkspace> workspace)
        : workspace_(std::move(workspace)) {}

    const char* name() const override { return "bundleRelocate"; }

    bool apply(Context& ctx, std::mt19937_64& rng, std::vector<int>& entities) override {
        RequestWorkspace& work = *workspace_;
        work.begin(ctx);
        if (work.vehicleCount() < 2 || work.deliveryCount() == 0) return false;
        const int seed = work.randomRouted(rng);
        if (seed < 0) return false;
        const PdpRoutePlan& plan = work.plan();
        const int from =
                work.listOf(plan.deliveries[static_cast<std::size_t>(seed)].pickupNodeIndex);

        // Mostly to the route of a nearby stop; now and then to any other
        // vehicle, an idle one included, which is how a route gets split.
        int to = -1;
        if (random_index(rng, 8) != 0) {
            const int neighbor = work.neighborElsewhere(seed, from, rng);
            if (neighbor >= 0) to = work.listOf(neighbor);
        }
        if (to < 0) {
            to = random_index(rng, work.vehicleCount() - 1);
            if (to >= from) ++to;
        }

        work.bundleFrom(seed, 1 + random_index(rng, std::max(1, work.settings().bundleMaxRequests)),
                        bundle_);
        for (int deliveryIndex : bundle_) work.detach(deliveryIndex);
        Blinker every;
        for (int deliveryIndex : bundle_) {
            const RequestInsertion insertion = work.bestOn(deliveryIndex, to, every);
            if (!insertion.found()) return false;  // the plan is untouched
            work.place(deliveryIndex, insertion);
        }
        return work.commit(ctx, entities);
    }

    void undo_last(Context& ctx) override { workspace_->undo(ctx); }

private:
    std::shared_ptr<RequestWorkspace> workspace_;
    std::vector<int> bundle_;
};

// ---------------------------------------------------------------------------
// BundleExchangeMove
// ---------------------------------------------------------------------------
class BundleExchangeMove final : public optimaru::MoveOperator<PdpRoutePlan> {
public:
    explicit BundleExchangeMove(std::shared_ptr<RequestWorkspace> workspace)
        : workspace_(std::move(workspace)) {}

    const char* name() const override { return "bundleExchange"; }

    bool apply(Context& ctx, std::mt19937_64& rng, std::vector<int>& entities) override {
        RequestWorkspace& work = *workspace_;
        work.begin(ctx);
        if (work.vehicleCount() < 2 || work.deliveryCount() < 2) return false;
        const int seed = work.randomRouted(rng);
        if (seed < 0) return false;
        const PdpRoutePlan& plan = work.plan();
        const int from =
                work.listOf(plan.deliveries[static_cast<std::size_t>(seed)].pickupNodeIndex);
        const int neighbor = work.neighborElsewhere(seed, from, rng);
        if (neighbor < 0) return false;
        const int partner = plan.nodes[static_cast<std::size_t>(neighbor)].deliveryIndex;
        if (!work.movable(partner) || work.where(partner) != RequestWorkspace::Where::Routed) {
            return false;
        }
        const int to = work.listOf(neighbor);

        const int most = std::max(1, work.settings().bundleMaxRequests);
        work.bundleFrom(seed, 1 + random_index(rng, most), outgoing_);
        work.bundleFrom(partner, 1 + random_index(rng, most), incoming_);
        for (int deliveryIndex : outgoing_) work.detach(deliveryIndex);
        for (int deliveryIndex : incoming_) work.detach(deliveryIndex);
        Blinker every;
        for (int deliveryIndex : outgoing_) {
            const RequestInsertion insertion = work.bestOn(deliveryIndex, to, every);
            if (!insertion.found()) return false;  // the plan is untouched
            work.place(deliveryIndex, insertion);
        }
        for (int deliveryIndex : incoming_) {
            const RequestInsertion insertion = work.bestOn(deliveryIndex, from, every);
            if (!insertion.found()) return false;
            work.place(deliveryIndex, insertion);
        }
        return work.commit(ctx, entities);
    }

    void undo_last(Context& ctx) override { workspace_->undo(ctx); }

private:
    std::shared_ptr<RequestWorkspace> workspace_;
    std::vector<int> outgoing_;
    std::vector<int> incoming_;
};

}  // namespace

std::vector<std::unique_ptr<optimaru::MoveOperator<PdpRoutePlan>>> makeRequestMoves(
        const RequestMoveSettings& settings) {
    std::vector<std::unique_ptr<optimaru::MoveOperator<PdpRoutePlan>>> moves;
    if (!settings.ruinRecreate && !settings.bundleMoves) return moves;
    auto workspace = std::make_shared<RequestWorkspace>(settings);
    if (settings.ruinRecreate) {
        moves.push_back(std::make_unique<RequestRuinRecreateMove>(workspace));
    }
    if (settings.bundleMoves) {
        moves.push_back(std::make_unique<BundleRelocateMove>(workspace));
        moves.push_back(std::make_unique<BundleExchangeMove>(workspace));
    }
    return moves;
}

}  // namespace pdp
