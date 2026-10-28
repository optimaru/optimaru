#ifndef VRP_CONSTRAINTS_ROUTE_SUMS_HPP
#define VRP_CONSTRAINTS_ROUTE_SUMS_HPP

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "vrp/constraints/common.hpp"

namespace vrp {
namespace detail {

// ---------------------------------------------------------------------------
// RouteSumsContext -- per-list prefix sums behind the piecewise previews
// ---------------------------------------------------------------------------
// The engine previews a move by describing each candidate route as pieces of
// the routes as they are now (optimaru::ListPiece) and asking every component
// what it would cost. A component whose cost sums per-visit quantities along
// the route (load, service, driving between neighbours) answers from these
// prefix sums in O(pieces) instead of writing the route out and walking it;
// see UnitConstraint::piecewiseCost. The sums are the same ones the
// monolithic VehicleRoutingIncrementalScoreCalculator keeps, held once per
// assembly: the components share this context through the engine's
// shared-context mechanism, and the calculator tells it once per list change
// through onListChanged(), however many components read it. A changed list is
// only marked then and rebuilt on the first preview that reads it, so phases
// that never preview (construction, a repair's own insertions) pay nothing.
//
// Index k of every vector holds the total over the first k visits of the
// list, so a run [b, e) is entry e minus entry b. Driving is kept as the
// list's internal legs only (cumDist[k] = drive into visit k-1 from visit
// k-2, zero for k <= 1), so the internal distance of [b, e) is cumDist[e] -
// cumDist[b + 1]; cumRevDist is the same for the reversed traversal. The leg
// into a piece's first visit and out of its last are looked up per piece.
struct RouteSumsContext final : optimaru::SharedConstraintContext {
    static constexpr const char* kKey = "vrp.route-sums.v1";

    struct ListSums {
        std::vector<std::int64_t> cumLoad;     // demand
        std::vector<std::int64_t> cumService;  // service seconds
        std::vector<std::int64_t> cumPenalty;  // unassigned penalty
        std::vector<std::int64_t> cumDist;     // internal legs, forward
        std::vector<std::int64_t> cumRevDist;  // internal legs, reversed
        // Earliest deadline over any run [b, e) in O(1): minEnd[j][i] is the
        // minimum over the 2^j visits from i (a sparse table, O(n log n),
        // built on the first deadline query after a change: an assembly
        // without the lateness component, or a plan with waiting, never asks).
        std::vector<Seconds> maxEnd;  // per visit, for the table
        std::vector<std::vector<Seconds>> minEnd;  // levels; storage kept across rebuilds
        bool minEndValid = false;
    };

    // What a candidate route made of pieces adds up to, home leg excluded.
    struct PieceWalk {
        std::int64_t load = 0;
        std::int64_t service = 0;
        std::int64_t drive = 0;  // home -> first visit ... -> last visit
        Seconds minEnd = std::numeric_limits<Seconds>::max();
        int lastLocation = -1;
        bool any = false;
    };

    // The plan this solve scores. Called from every sharing component's
    // reset, unit and global alike, in whatever order the calculator resets
    // them; the first call after the calculator's beginReset() marks every
    // list for rebuild (a reset follows every rejected compound move, so the
    // tables keep their storage), later ones are no-ops.
    void prepare(const VehicleRoutePlan& plan) {
        if (plan_ == &plan && preparedGeneration_ == generation) return;
        plan_ = &plan;
        preparedGeneration_ = generation;
        lists_.resize(plan.vehicles.size() + 1);  // keeps the vectors' capacity across resets
        dirty_.assign(plan.vehicles.size() + 1, 1);
        Seconds earliestDeparture = std::numeric_limits<Seconds>::max();
        for (const Vehicle& vehicle : plan.vehicles) {
            earliestDeparture = std::min(earliestDeparture, vehicle.departureTime);
        }
        // No visit's window opens after any vehicle leaves: arrival is always
        // the service start, so finish times follow from the sums alone.
        noWaiting_ = true;
        for (const Visit& visit : plan.visits) {
            noWaiting_ = noWaiting_ && visit.minStartTime <= earliestDeparture;
        }
    }

    void onListChanged(int list) override {
        if (list >= 0 && static_cast<std::size_t>(list) < dirty_.size()) {
            dirty_[static_cast<std::size_t>(list)] = 1;
        }
    }

    bool noWaiting() const { return noWaiting_; }

    // Demand / unassigned penalty of one piece.
    std::int64_t loadOf(const optimaru::ListPiece& piece) const {
        if (piece.size() == 1) return visitOf(piece).demand;
        const ListSums& s = sums(piece.list);
        return s.cumLoad[static_cast<std::size_t>(piece.end)]
               - s.cumLoad[static_cast<std::size_t>(piece.begin)];
    }
    std::int64_t penaltyOf(const optimaru::ListPiece& piece) const {
        if (piece.size() == 1) return visitOf(piece).unassignedPenalty();
        const ListSums& s = sums(piece.list);
        return s.cumPenalty[static_cast<std::size_t>(piece.end)]
               - s.cumPenalty[static_cast<std::size_t>(piece.begin)];
    }

    // The route `vehicleIndex` would drive holding `pieces` in order: load,
    // service, driving from home through the last visit and, when `withMinEnd`,
    // the earliest deadline among its visits. O(pieces), except that the
    // the earliest deadline is one sparse-table lookup per piece; a component
    // that does not need it asks without and skips that work.
    PieceWalk walk(int vehicleIndex, const optimaru::ListPiece* pieces, int count,
                   bool withMinEnd) const {
        PieceWalk w;
        const Vehicle& vehicle = plan_->vehicles[static_cast<std::size_t>(vehicleIndex)];
        int previousLocation = vehicle.homeLocationIndex;
        for (int i = 0; i < count; ++i) {
            const optimaru::ListPiece& piece = pieces[i];
            if (piece.size() <= 0) continue;
            if (piece.size() == 1) {
                // One visit, detached or not: read it, and leave its list's
                // sums alone (a relocation out of a large pool would otherwise
                // rebuild the pool's sums for nothing).
                const Visit& visit = visitOf(piece);
                w.load += visit.demand;
                w.service += visit.serviceDuration;
                if (withMinEnd) w.minEnd = std::min(w.minEnd, visit.maxEndTime);
                w.drive += plan_->drivingTime(previousLocation, visit.locationIndex);
                previousLocation = visit.locationIndex;
                w.any = true;
                continue;
            }
            ListSums& s = sums(piece.list);
            const std::vector<int>& elements = elementsOf(piece.list);
            const std::size_t b = static_cast<std::size_t>(piece.begin);
            const std::size_t e = static_cast<std::size_t>(piece.end);
            w.load += s.cumLoad[e] - s.cumLoad[b];
            w.service += s.cumService[e] - s.cumService[b];
            if (withMinEnd) w.minEnd = std::min(w.minEnd, minEndOf(s, b, e));
            const int first = elements[piece.reversed ? e - 1 : b];
            const int last = elements[piece.reversed ? b : e - 1];
            w.drive += plan_->drivingTime(
                    previousLocation, plan_->visits[static_cast<std::size_t>(first)].locationIndex);
            w.drive += piece.reversed ? s.cumRevDist[e] - s.cumRevDist[b + 1]
                                      : s.cumDist[e] - s.cumDist[b + 1];
            previousLocation = plan_->visits[static_cast<std::size_t>(last)].locationIndex;
            w.any = true;
        }
        w.lastLocation = previousLocation;
        return w;
    }

private:
    // The one visit of a single-element piece, detached or in a list.
    const Visit& visitOf(const optimaru::ListPiece& piece) const {
        const int visit = piece.detached() ? piece.begin : elementsOf(piece.list)[static_cast<std::size_t>(piece.begin)];
        return plan_->visits[static_cast<std::size_t>(visit)];
    }
    const std::vector<int>& elementsOf(int list) const {
        return static_cast<std::size_t>(list) >= plan_->vehicles.size()
                       ? plan_->unassignedVisitIndices
                       : plan_->vehicles[static_cast<std::size_t>(list)].visitIndices;
    }

    // The sums of `list`, rebuilt first if the list changed since last read.
    ListSums& sums(int list) const {
        if (dirty_[static_cast<std::size_t>(list)]) rebuild(list);
        return lists_[static_cast<std::size_t>(list)];
    }

    void rebuild(int list) const {
        dirty_[static_cast<std::size_t>(list)] = 0;
        const std::vector<int>& elements = elementsOf(list);
        ListSums& s = lists_[static_cast<std::size_t>(list)];
        const std::size_t n = elements.size();
        s.cumLoad.assign(n + 1, 0);
        s.cumPenalty.assign(n + 1, 0);
        // The pool gets every sum too: a run of pool visits moved into a route
        // (a tail swap or CROSS against the pool, an Or-opt out of it) is a
        // pool piece inside a vehicle run, read for service, legs and deadlines.
        s.cumService.assign(n + 1, 0);
        s.cumDist.assign(n + 1, 0);
        s.cumRevDist.assign(n + 1, 0);
        s.maxEnd.resize(n);
        s.minEndValid = false;
        int previousLocation = -1;
        for (std::size_t k = 0; k < n; ++k) {
            const Visit& visit = plan_->visits[static_cast<std::size_t>(elements[k])];
            s.cumLoad[k + 1] = s.cumLoad[k] + visit.demand;
            s.cumService[k + 1] = s.cumService[k] + visit.serviceDuration;
            s.cumPenalty[k + 1] = s.cumPenalty[k] + visit.unassignedPenalty();
            s.cumDist[k + 1] = s.cumDist[k] + (k == 0 ? 0 : plan_->drivingTime(previousLocation,
                                                                                visit.locationIndex));
            s.cumRevDist[k + 1] =
                    s.cumRevDist[k]
                    + (k == 0 ? 0 : plan_->drivingTime(visit.locationIndex, previousLocation));
            s.maxEnd[k] = visit.maxEndTime;
            previousLocation = visit.locationIndex;
        }
    }

    static void buildMinEnd(ListSums& s) {
        const std::size_t n = s.maxEnd.size();
        std::size_t levels = 1;
        for (std::size_t span = 2; span <= n; span *= 2) ++levels;
        s.minEnd.resize(levels);
        s.minEnd[0].assign(s.maxEnd.begin(), s.maxEnd.end());
        std::size_t span = 2;
        for (std::size_t j = 1; j < levels; ++j, span *= 2) {
            const std::vector<Seconds>& previous = s.minEnd[j - 1];
            std::vector<Seconds>& level = s.minEnd[j];
            level.resize(n - span + 1);
            for (std::size_t i = 0; i + span <= n; ++i) {
                level[i] = std::min(previous[i], previous[i + span / 2]);
            }
        }
        s.minEndValid = true;
    }

    // Earliest deadline over [begin, end) of a list, end > begin: the two
    // largest power-of-two windows that cover the run.
    static Seconds minEndOf(ListSums& s, std::size_t begin, std::size_t end) {
        if (!s.minEndValid) buildMinEnd(s);
        const std::size_t length = end - begin;
        std::size_t level = 0;
        while ((std::size_t{2} << level) <= length) ++level;
        const std::vector<Seconds>& table = s.minEnd[level];
        return std::min(table[begin], table[end - (std::size_t{1} << level)]);
    }

    const VehicleRoutePlan* plan_ = nullptr;
    std::uint64_t preparedGeneration_ = 0;
    mutable std::vector<ListSums> lists_;
    mutable std::vector<char> dirty_;  // per list: changed since its sums were built
    bool noWaiting_ = false;
};

// A component that reads the shared sums: the shared-context plumbing once,
// over either constraint base, so each component is its cost() and its
// piecewise answer. Both bases prepare the context themselves at reset: a
// unit component in onReset(), a global one in reset() before handing over
// to resetWithSums().
template <typename Base>
struct WithRouteSums : Base {
    using Base::Base;

    const char* sharedContextKey() const override { return RouteSumsContext::kKey; }
    std::unique_ptr<optimaru::SharedConstraintContext> createSharedContext() const override {
        return std::make_unique<RouteSumsContext>();
    }
    void bindSharedContext(optimaru::SharedConstraintContext& context) override {
        sums_ = static_cast<RouteSumsContext*>(&context);
    }
    bool supportsPiecewisePreview() const override { return true; }

protected:
    RouteSumsContext* sums_ = nullptr;
};

struct SummedUnitConstraint : WithRouteSums<VrpUnitConstraint> {
    using WithRouteSums<VrpUnitConstraint>::WithRouteSums;
    void onReset(const VehicleRoutePlan& plan) override { this->sums_->prepare(plan); }
};
struct SummedGlobalConstraint : WithRouteSums<VrpGlobalConstraint> {
    using WithRouteSums<VrpGlobalConstraint>::WithRouteSums;
    void reset(const VehicleRoutePlan& plan,
               const std::vector<std::vector<int>>& membersPerUnit) final {
        this->sums_->prepare(plan);
        resetWithSums(plan, membersPerUnit);
    }
    // reset() with the shared sums already prepared.
    virtual void resetWithSums(const VehicleRoutePlan& plan,
                               const std::vector<std::vector<int>>& membersPerUnit) = 0;
};

}  // namespace detail
}  // namespace vrp

#endif  // VRP_CONSTRAINTS_ROUTE_SUMS_HPP
