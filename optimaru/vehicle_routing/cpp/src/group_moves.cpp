#include "vrp/group_moves.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/portable_random.hpp"

namespace vrp {

namespace {

using Descriptor = optimaru::SolutionTraits<VehicleRoutePlan>;
using Context = optimaru::OperatorContext<VehicleRoutePlan>;
using optimaru::random_index;

// A visit taken out of its route and not placed yet.
constexpr int kDetached = -2;
// Groups of the unassigned pool one reinsertion tries to route.
constexpr int kWaitingPerRepair = 2;

// Whether two vehicles serve any visit identically while they are idle: the
// same limits, and the same driving times between their home and every visit.
// (Two vehicles of one depot usually have a location entry each.)
bool sameService(const VehicleRoutePlan& plan, const Vehicle& a, const Vehicle& b) {
    if (a.capacity != b.capacity || a.departureTime != b.departureTime ||
        a.maxTravelTime != b.maxTravelTime || a.maxRouteDuration != b.maxRouteDuration ||
        a.maxEndTime != b.maxEndTime || a.maxStops != b.maxStops ||
        a.minLoadRatio != b.minLoadRatio || a.fixedCost != b.fixedCost ||
        a.maxRouteRadiusMeters != b.maxRouteRadiusMeters || a.returnToDepot != b.returnToDepot) {
        return false;
    }
    const auto homeA = static_cast<std::size_t>(a.homeLocationIndex);
    const auto homeB = static_cast<std::size_t>(b.homeLocationIndex);
    if (homeA == homeB) return true;
    if (homeA < plan.locations.size() && homeB < plan.locations.size() &&
        (plan.locations[homeA].latitude != plan.locations[homeB].latitude ||
         plan.locations[homeA].longitude != plan.locations[homeB].longitude)) {
        return false;
    }
    for (const Visit& visit : plan.visits) {
        const auto stop = static_cast<std::size_t>(visit.locationIndex);
        if (plan.drivingSeconds[homeA][stop] != plan.drivingSeconds[homeB][stop] ||
            plan.drivingSeconds[stop][homeA] != plan.drivingSeconds[stop][homeB]) {
            return false;
        }
    }
    return true;
}

// The vehicles worth offering a unit to: every vehicle with a route and, of
// the idle vehicles that would serve it identically, only the first.
class Fleet {
public:
    void classify(const VehicleRoutePlan& plan) {
        classOf_.assign(plan.vehicles.size(), -1);
        std::vector<int> representatives;
        for (std::size_t v = 0; v < plan.vehicles.size(); ++v) {
            for (std::size_t c = 0; c < representatives.size(); ++c) {
                if (sameService(plan, plan.vehicles[static_cast<std::size_t>(representatives[c])],
                                plan.vehicles[v])) {
                    classOf_[v] = static_cast<int>(c);
                    break;
                }
            }
            if (classOf_[v] < 0) {
                classOf_[v] = static_cast<int>(representatives.size());
                representatives.push_back(static_cast<int>(v));
            }
        }
        seen_.assign(representatives.size(), 0);
    }

    template <typename IsIdle>
    void candidates(IsIdle&& idle, std::vector<int>& out) {
        out.clear();
        std::fill(seen_.begin(), seen_.end(), 0);
        for (std::size_t v = 0; v < classOf_.size(); ++v) {
            if (idle(static_cast<int>(v))) {
                char& seen = seen_[static_cast<std::size_t>(classOf_[v])];
                if (seen) continue;
                seen = 1;
            }
            out.push_back(static_cast<int>(v));
        }
    }

private:
    std::vector<int> classOf_;
    std::vector<char> seen_;
};

// The cheapest way to put `visits` on one of `candidates`, each a route of
// `routes` with its timeline in `timelines`. Leaves `steps` empty when no
// vehicle takes all of them.
void bestPlan(const VehicleRoutePlan& plan, const VisitGroups& groups, UnitPlanner& planner,
              const std::vector<RouteTimeline>& timelines,
              const std::vector<std::vector<int>>& routes, const std::vector<int>& candidates,
              const std::vector<int>& visits, std::vector<UnitStep>& steps,
              std::vector<UnitStep>& trial) {
    steps.clear();
    Seconds least = std::numeric_limits<Seconds>::max();
    for (int v : candidates) {
        trial.clear();
        Seconds added = 0;
        if (planner.plan(plan, groups, timelines[static_cast<std::size_t>(v)],
                         routes[static_cast<std::size_t>(v)], visits, trial, added) &&
            added < least) {
            least = added;
            steps.swap(trial);
        }
    }
}

// ---------------------------------------------------------------------------
// GroupWorkspace
// ---------------------------------------------------------------------------
// Working copies of the routes with their timelines, and the plan-then-commit
// protocol: a move takes units out and puts them back on the copies, where
// every candidate slot is judged from the timelines, and nothing reaches the
// objective until commit() writes each changed route through the editor, once.
// undo() writes the old routes back.
//
// The copies survive between moves and are compared with the plan when the
// next one begins, so whatever else changed the plan in between (the engine's
// own moves, a rejected move's undo) is picked up there.
class GroupWorkspace {
public:
    GroupWorkspace(const GroupMoveSettings& settings, std::shared_ptr<const VisitGroups> groups)
        : settings_(settings), groups_(std::move(groups)) {}

    const GroupMoveSettings& settings() const { return settings_; }
    const VisitGroups& groups() const { return *groups_; }
    int vehicleCount() const { return vehicleCount_; }

    void begin(Context& ctx) {
        ctx_ = &ctx;
        plan_ = &ctx.editor.solution();
        const VehicleRoutePlan& plan = *plan_;
        vehicleCount_ = static_cast<int>(plan.vehicles.size());
        const auto vehicles = static_cast<std::size_t>(vehicleCount_);
        if (routes_.size() != vehicles) {
            routes_.resize(vehicles);
            timelines_.resize(vehicles);
            for (std::size_t v = 0; v < vehicles; ++v) {
                routes_[v] = plan.vehicles[v].visitIndices;
                timelines_[v].rebuild(plan, static_cast<int>(v), routes_[v]);
            }
            fleet_.classify(plan);
            dirty_.assign(vehicles, 0);
            stale_.assign(vehicles, 0);
            taken_.assign(plan.visits.size(), 0);
            dirtyList_.clear();
            staleList_.clear();
        } else {
            for (int v : dirtyList_) dirty_[static_cast<std::size_t>(v)] = 0;
            dirtyList_.clear();
            for (std::size_t v = 0; v < vehicles; ++v) {
                if (routes_[v] != plan.vehicles[v].visitIndices) {
                    routes_[v] = plan.vehicles[v].visitIndices;
                    markStale(static_cast<int>(v));
                }
            }
        }
        routeOf_.assign(ctx.element_list.begin(), ctx.element_list.end());
        for (int unit : pending_) taken_[static_cast<std::size_t>(unit)] = 0;
        pending_.clear();
        pendingVisits_ = 0;
        moved_.clear();
        saved_.clear();
    }

    // The list a visit was in when the move began.
    int listOf(int visitIndex) const {
        return ctx_->element_list[static_cast<std::size_t>(visitIndex)];
    }
    bool onVehicle(int list) const { return list >= 0 && list < vehicleCount_; }
    // The visits pending() holds.
    int pendingVisits() const { return pendingVisits_; }
    bool nothingPending() const { return pending_.empty(); }

    // --- choosing what to take out ------------------------------------------

    // The visit's unit joins pending(), once.
    bool take(int visitIndex) {
        const int unit = groups_->leaderOf(visitIndex);
        char& taken = taken_[static_cast<std::size_t>(unit)];
        if (taken) return false;
        taken = 1;
        pending_.push_back(unit);
        pendingVisits_ += groups_->sizeOf(unit);
        return true;
    }

    // The units of the routed stops nearest to the visits of `seed`'s unit,
    // groups and single visits alike, until `extra` more visits are pending.
    void takeNear(int seed, int extra, std::mt19937_64& rng) {
        if (!ctx_->nearby.enabled()) return takeGroups(extra, rng);
        const int limit = pendingVisits_ + extra;
        for (int visit = seed; visit >= 0 && pendingVisits_ < limit;
             visit = groups_->nextOf(visit)) {
            for (int neighbor : ctx_->nearby.of(visit)) {
                if (pendingVisits_ >= limit) break;
                if (!onVehicle(listOf(neighbor))) continue;
                if (pendingVisits_ + groups_->sizeOf(groups_->leaderOf(neighbor)) > limit) continue;
                take(neighbor);
            }
        }
    }

    // Other groups, picked at random, until `extra` more visits are pending.
    void takeGroups(int extra, std::mt19937_64& rng) {
        const std::vector<int>& leaders = groups_->groups();
        const int limit = pendingVisits_ + extra;
        for (int attempt = 0; attempt < 8 && pendingVisits_ < limit; ++attempt) {
            const int leader = leaders[static_cast<std::size_t>(
                    random_index(rng, static_cast<int>(leaders.size())))];
            if (pendingVisits_ + groups_->sizeOf(leader) > limit) continue;
            take(leader);
        }
    }

    // Up to `limit` groups that wait whole in the unassigned pool.
    void takeWaitingGroups(int limit, std::mt19937_64& rng) {
        const std::vector<int>& pool = plan_->unassignedVisitIndices;
        if (pool.empty()) return;
        for (int attempt = 0; attempt < 2 * limit && limit > 0; ++attempt) {
            const int unit = groups_->leaderOf(pool[static_cast<std::size_t>(
                    random_index(rng, static_cast<int>(pool.size())))]);
            if (groups_->sizeOf(unit) == 1 || taken_[static_cast<std::size_t>(unit)]) continue;
            bool waits = true;
            for (int visit = unit; visit >= 0 && waits; visit = groups_->nextOf(visit)) {
                waits = !onVehicle(listOf(visit));
            }
            if (waits && take(unit)) --limit;
        }
    }

    // --- taking out and putting back ------------------------------------------

    void detachPending() {
        for (int unit : pending_) {
            for (int visit = unit; visit >= 0; visit = groups_->nextOf(visit)) detach(visit);
        }
    }

    // Puts every pending unit back at its cheapest place, the larger units
    // first (they need the most room on one vehicle), the rest in random
    // order. A unit no vehicle takes whole ends the move in the pool.
    void reinsert(std::mt19937_64& rng) {
        std::shuffle(pending_.begin(), pending_.end(), rng);
        std::stable_sort(pending_.begin(), pending_.end(),
                         [this](int a, int b) { return groups_->sizeOf(a) > groups_->sizeOf(b); });
        for (int unit : pending_) {
            refreshTimelines();
            fleet_.candidates(
                    [this](int v) { return routes_[static_cast<std::size_t>(v)].empty(); },
                    candidates_);
            members_.clear();
            for (int visit = unit; visit >= 0; visit = groups_->nextOf(visit))
                members_.push_back(visit);
            bestPlan(*plan_, *groups_, planner_, timelines_, routes_, candidates_, members_, steps_,
                     trial_);
            if (steps_.empty()) {
                for (int visit : members_)
                    routeOf_[static_cast<std::size_t>(visit)] = vehicleCount_;
                continue;
            }
            for (const UnitStep& step : steps_) {
                std::vector<int>& route = routes_[static_cast<std::size_t>(step.slot.vehicle)];
                route.insert(route.begin() + step.slot.position, step.visit);
                routeOf_[static_cast<std::size_t>(step.visit)] = step.slot.vehicle;
            }
            touch(steps_.front().slot.vehicle);
        }
    }

    // --- commit / undo --------------------------------------------------------

    // Writes the changed routes (and the pool) through the editor. Returns
    // false, with the plan untouched, when the move changed nothing.
    bool commit(Context& ctx, std::vector<int>& entities) {
        VehicleRoutePlan& plan = ctx.editor.solution();
        const int pool = vehicleCount_;
        bool poolChanged = false;
        for (int visit : moved_) {
            int& list = routeOf_[static_cast<std::size_t>(visit)];
            if (list == kDetached) list = pool;  // never placed: it waits
            if ((listOf(visit) == pool) != (list == pool)) poolChanged = true;
        }
        bool changed = poolChanged;
        for (int v : dirtyList_) {
            if (routes_[static_cast<std::size_t>(v)] !=
                plan.vehicles[static_cast<std::size_t>(v)].visitIndices) {
                changed = true;
                break;
            }
        }
        if (!changed) return false;

        for (int v : dirtyList_) {
            std::vector<int>& route = plan.vehicles[static_cast<std::size_t>(v)].visitIndices;
            if (routes_[static_cast<std::size_t>(v)] == route) continue;
            ctx.editor.begin_list_edit(v);
            saved_.emplace_back(v, std::move(route));
            route = routes_[static_cast<std::size_t>(v)];
            ctx.editor.end_list_edit(v);
        }
        if (poolChanged) {
            std::vector<int>& waiting = plan.unassignedVisitIndices;
            ctx.editor.begin_list_edit(pool);
            saved_.emplace_back(pool, waiting);
            waiting.erase(std::remove_if(waiting.begin(), waiting.end(),
                                         [&](int visit) {
                                             return routeOf_[static_cast<std::size_t>(visit)] !=
                                                    pool;
                                         }),
                          waiting.end());
            for (int visit : moved_) {
                if (routeOf_[static_cast<std::size_t>(visit)] == pool && listOf(visit) != pool)
                    waiting.push_back(visit);
            }
            ctx.editor.end_list_edit(pool);
        }
        entities.insert(entities.end(), moved_.begin(), moved_.end());
        return true;
    }

    void undo(Context& ctx) {
        VehicleRoutePlan& plan = ctx.editor.solution();
        for (auto& [list, contents] : saved_) {
            ctx.editor.begin_list_edit(list);
            Descriptor::list_of(plan, list) = std::move(contents);
            ctx.editor.end_list_edit(list);
        }
        saved_.clear();
    }

private:
    void detach(int visit) {
        int& list = routeOf_[static_cast<std::size_t>(visit)];
        if (onVehicle(list)) {
            std::vector<int>& route = routes_[static_cast<std::size_t>(list)];
            const auto at = std::find(route.begin(), route.end(), visit);
            if (at != route.end()) route.erase(at);
            touch(list);
        }
        list = kDetached;
        moved_.push_back(visit);
    }

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
            timelines_[static_cast<std::size_t>(v)].rebuild(*plan_, v,
                                                            routes_[static_cast<std::size_t>(v)]);
            stale_[static_cast<std::size_t>(v)] = 0;
        }
        staleList_.clear();
    }

    GroupMoveSettings settings_;
    std::shared_ptr<const VisitGroups> groups_;
    Context* ctx_ = nullptr;
    const VehicleRoutePlan* plan_ = nullptr;
    int vehicleCount_ = 0;

    std::vector<std::vector<int>> routes_;  // working copy, per vehicle
    std::vector<RouteTimeline> timelines_;  // of the working copies
    std::vector<char> stale_;               // timeline behind its working route
    std::vector<int> staleList_;
    std::vector<int> routeOf_;  // visit -> list on the working copies
    std::vector<char> dirty_;   // working route changed by this move
    std::vector<int> dirtyList_;
    Fleet fleet_;

    std::vector<int> pending_;  // units (by leader) this move takes out and puts back
    int pendingVisits_ = 0;     // visits in those units
    std::vector<char> taken_;   // unit is in pending_
    std::vector<int> moved_;    // visits detached by this move
    std::vector<std::pair<int, std::vector<int>>> saved_;  // lists as they were, for undo

    // Scratch.
    UnitPlanner planner_;
    std::vector<int> candidates_;
    std::vector<int> members_;
    std::vector<UnitStep> steps_;
    std::vector<UnitStep> trial_;
};

// ---------------------------------------------------------------------------
// GroupRuinRecreateMove
// ---------------------------------------------------------------------------
class GroupRuinRecreateMove final : public optimaru::MoveOperator<VehicleRoutePlan> {
public:
    explicit GroupRuinRecreateMove(std::shared_ptr<GroupWorkspace> workspace)
        : workspace_(std::move(workspace)) {}

    const char* name() const override { return "groupRuinRecreate"; }

    bool apply(Context& ctx, std::mt19937_64& rng, std::vector<int>& entities) override {
        GroupWorkspace& work = *workspace_;
        work.begin(ctx);
        const std::vector<int>& leaders = work.groups().groups();
        if (leaders.empty() || work.vehicleCount() == 0) return false;

        const int seed = leaders[static_cast<std::size_t>(
                random_index(rng, static_cast<int>(leaders.size())))];
        work.take(seed);
        // What leaves with it: nothing, the units around it, or other groups.
        const int extra = random_index(rng, std::max(0, work.settings().ruinMaxVisits) + 1);
        switch (random_index(rng, 3)) {
            case 0:
                break;
            case 1:
                work.takeNear(seed, extra, rng);
                break;
            default:
                work.takeGroups(extra, rng);
                break;
        }
        work.takeWaitingGroups(kWaitingPerRepair, rng);
        work.detachPending();
        work.reinsert(rng);
        return work.commit(ctx, entities);
    }

    void undo_last(Context& ctx) override { workspace_->undo(ctx); }

private:
    std::shared_ptr<GroupWorkspace> workspace_;
};

}  // namespace

std::vector<std::unique_ptr<optimaru::MoveOperator<VehicleRoutePlan>>> makeGroupMoves(
        const GroupMoveSettings& settings, std::shared_ptr<const VisitGroups> groups) {
    std::vector<std::unique_ptr<optimaru::MoveOperator<VehicleRoutePlan>>> moves;
    if (!settings.ruinRecreate || !groups->any()) return moves;
    auto workspace = std::make_shared<GroupWorkspace>(settings, std::move(groups));
    moves.push_back(std::make_unique<GroupRuinRecreateMove>(std::move(workspace)));
    return moves;
}

void constructGroups(VehicleRoutePlan& plan, const VisitGroups& groups) {
    if (!groups.any()) return;
    const std::size_t vehicles = plan.vehicles.size();
    std::vector<int> routedOn(plan.visits.size(), -1);
    std::vector<std::vector<int>> routes(vehicles);
    std::vector<RouteTimeline> timelines(vehicles);
    for (std::size_t v = 0; v < vehicles; ++v) {
        routes[v] = plan.vehicles[v].visitIndices;
        for (int visit : routes[v]) routedOn[static_cast<std::size_t>(visit)] = static_cast<int>(v);
        timelines[v].rebuild(plan, static_cast<int>(v), routes[v]);
    }

    // The larger groups first: they need the most room on one vehicle. Among
    // equals, the one whose tightest window is narrowest.
    std::vector<int> order = groups.groups();
    const auto tightest = [&](int leader) {
        Seconds narrowest = std::numeric_limits<Seconds>::max();
        for (int visit = leader; visit >= 0; visit = groups.nextOf(visit)) {
            const Visit& v = plan.visits[static_cast<std::size_t>(visit)];
            narrowest = std::min(narrowest, v.maxEndTime - v.minStartTime);
        }
        return narrowest;
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (groups.sizeOf(a) != groups.sizeOf(b)) return groups.sizeOf(a) > groups.sizeOf(b);
        return tightest(a) < tightest(b);
    });

    Fleet fleet;
    fleet.classify(plan);
    UnitPlanner planner;
    std::vector<int> waiting;
    std::vector<int> candidates;
    std::vector<std::pair<int, int>> riding;  // (vehicle, visits of the group on it)
    std::vector<UnitStep> steps;
    std::vector<UnitStep> trial;
    std::vector<char> parked(plan.visits.size(), 0);
    for (int visit : plan.unassignedVisitIndices) parked[static_cast<std::size_t>(visit)] = 1;
    for (int leader : order) {
        // The visits still to place, and the vehicle that carries most of the
        // others (the first such when they start on several). A visit the
        // caller parked in the pool stays there, as it does for the engine's
        // construction, and the rest of its group that is on no route waits
        // with it: the group operator routes the group whole.
        waiting.clear();
        riding.clear();
        bool parkedByCaller = false;
        for (int visit = leader; visit >= 0; visit = groups.nextOf(visit)) {
            const int on = routedOn[static_cast<std::size_t>(visit)];
            if (on < 0) {
                if (parked[static_cast<std::size_t>(visit)]) {
                    parkedByCaller = true;
                } else {
                    waiting.push_back(visit);
                }
                continue;
            }
            const auto at = std::find_if(riding.begin(), riding.end(),
                                         [on](const auto& entry) { return entry.first == on; });
            if (at == riding.end()) {
                riding.emplace_back(on, 1);
            } else {
                ++at->second;
            }
        }
        if (waiting.empty()) continue;
        int bound = -1;
        int most = 0;
        for (const auto& [vehicle, visits] : riding) {
            if (visits > most) {
                most = visits;
                bound = vehicle;
            }
        }
        if (parkedByCaller) {
            candidates.clear();
        } else if (bound >= 0) {
            candidates.assign(1, bound);
        } else {
            fleet.candidates(
                    [&routes](int v) { return routes[static_cast<std::size_t>(v)].empty(); },
                    candidates);
        }
        bestPlan(plan, groups, planner, timelines, routes, candidates, waiting, steps, trial);
        if (steps.empty()) {
            // No vehicle takes them: they wait together in the pool, where
            // construction leaves them alone. A group with visits already
            // on a route was only offered that vehicle, so it starts split
            // when the rest does not fit there, and the group operator
            // moves it whole.
            for (int visit : waiting) {
                if (!parked[static_cast<std::size_t>(visit)]) {
                    parked[static_cast<std::size_t>(visit)] = 1;
                    plan.unassignedVisitIndices.push_back(visit);
                }
            }
            continue;
        }
        const int vehicle = steps.front().slot.vehicle;
        std::vector<int>& route = routes[static_cast<std::size_t>(vehicle)];
        for (const UnitStep& step : steps) {
            route.insert(route.begin() + step.slot.position, step.visit);
            routedOn[static_cast<std::size_t>(step.visit)] = vehicle;
        }
        timelines[static_cast<std::size_t>(vehicle)].rebuild(plan, vehicle, route);
    }
    for (std::size_t v = 0; v < vehicles; ++v) plan.vehicles[v].visitIndices = routes[v];
    // A visit that went onto a route no longer waits.
    auto& pool = plan.unassignedVisitIndices;
    pool.erase(std::remove_if(pool.begin(), pool.end(),
                              [&routedOn](int visit) {
                                  return routedOn[static_cast<std::size_t>(visit)] >= 0;
                              }),
               pool.end());
}

}  // namespace vrp
