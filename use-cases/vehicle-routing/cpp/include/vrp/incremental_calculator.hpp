#ifndef VRP_INCREMENTAL_CALCULATOR_HPP
#define VRP_INCREMENTAL_CALCULATOR_HPP

#include <cassert>
#include <cstdint>
#include <vector>

#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"
#include "vrp/domain.hpp"

namespace vrp
{

// ---------------------------------------------------------------------------
// VehicleRoutingIncrementalScoreCalculator
// ---------------------------------------------------------------------------
// The vehicle-routing implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing Java's
// IncrementalScoreCalculator instead of a ConstraintProvider.
//
// Every constraint in VehicleRoutingConstraintProvider decomposes per route,
// which is what makes this cheap: a move touches at most two lists, so a step
// costs O(route length) instead of O(instance).
//
//   hard   vehicleCapacity                  -> overload_[v]
//   hard   serviceFinishedAfterMaxEndTime   -> lateness_[v]  (whole minutes)
//   soft   minimizeTravelTime               -> driving_[v]   (seconds)
//   medium maximizeVisitsAssigned           -> unassignedMinutes_
//
// The first three are re-derived for a touched vehicle list in one walk
// (arrival times propagate along the route, so all three terms fall out of the
// same pass). The medium level belongs to the virtual unassigned-pool list —
// its contribution is just the service minutes of whatever sits in it.
//
// beforeListChanged retracts the list's contribution while it still holds the
// OLD contents; afterListChanged inserts the NEW contents. The calculator
// reads the route lists directly and never the shadow fields (vehicleIndex /
// arrivalTime), so it stays correct while the engine mutates lists mid-move;
// the shadows are refreshed by ScoreCalculator afterwards. The full-recompute
// reference is ScoreCalculator::calculateScore, checked in the tests and under
// EnvironmentMode::FULL_ASSERT.
//
// NOTE (measured 2026-08-18, do not redo without new evidence): a
// suffix-recompute cache (per-vehicle walk states + route copy, resume the
// walk at the first position that differs from the cached contents) was
// implemented, proven bit-exact, and REJECTED — at this domain's route
// lengths (5-25 stops on CVRPLIB Set X) the compare/copy/state-write overhead
// exceeds the partial-walk savings: interleaved 6-seed A/B at 10s measured
// means +3.1pp (X-n491) / +1.1pp (X-n701) WORSE and 0 paired wins, with
// X-n289 bit-identical on all 6 seeds.
class VehicleRoutingIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<VehicleRoutePlan>
{
public:
    void resetWorkingSolution(VehicleRoutePlan &workingSolution) override;
    void beforeListChanged(int listIndex) override;
    void afterListChanged(int listIndex) override;
    optimaru::HardSoftScore calculateScore() override;

    // Move previews (see IncrementalScoreCalculator). A candidate route is a
    // handful of pieces of existing lists. On a plan where no visit ever
    // waits for its window to open, every piece's load, service and driving
    // total is a difference of per-list prefix sums kept up to date in
    // afterListChanged(), so a preview is O(1); lateness is the one term that
    // is not a prefix sum, so the sums answer only when no visit can be late
    // on the candidate route (its finish time stays under the earliest
    // deadline of the lists it draws from). Otherwise, and on a plan with
    // waiting (time windows that open after departure, where arrival times
    // depend on the order), the candidate route is written out and walked —
    // O(route length), still one walk against the several the
    // apply/score/undo cycle costs. Every preview is served.
    //
    // Scope: this calculator and its previews cover the quickstart's four
    // terms (capacity, visit deadlines, unassigned visits, driving time).
    // The optional vehicle route policies (maxTravelTime, maxRouteDuration,
    // Vehicle::maxEndTime, maxStops, minLoadRatio, fixedCost,
    // maxRouteRadiusMeters) are scored by the decomposed constraint plugins,
    // whose calculator previews a move by re-costing the candidate routes per
    // component (O(route) rather than O(1), always served). Adding a term
    // here means adding it to evaluateElements, previewRuns / previewInsert
    // (falling back to the walk when it is not a prefix sum), the plugin and
    // the CP-SAT reference together.
    bool previewSegmentMove(int fromList, int start, int len, int toList, int insertAt,
                            optimaru::HardSoftScore &out) override;
    bool previewSwap(int l1, int p1, int l2, int p2, optimaru::HardSoftScore &out) override;
    bool previewReverse(int list, int first, int last,
                        optimaru::HardSoftScore &out) override;
    bool previewTailSwap(int l1, int c1, int l2, int c2,
                         optimaru::HardSoftScore &out) override;
    bool previewExchange(int l1, int p1, int len1, int l2, int p2, int len2,
                         optimaru::HardSoftScore &out) override;
    bool previewInsert(int list, int pos, int element, optimaru::HardSoftScore &out) override;
    // Every kind above is served once the working solution is set.
    bool supportsPreviews() const override { return previewsEnabled(); }

    // The HardMediumSoft view of the current state, for callers that want the
    // three levels without decoding the packed engine score.
    Score currentScore() const;

private:
    // A contiguous run [begin, end) of one list, traversed forward or reversed.
    // (A detached element never appears as a piece: previewInsert scores it
    // as a direct delta instead.)
    struct Piece
    {
        int list;
        int begin;
        int end;
        bool reversed = false;
    };

    // Per-list prefix sums over its current elements. Index k holds the total
    // over the first k elements, so a run [i, j) is entry j minus entry i.
    struct ListCache
    {
        std::vector<std::int64_t> cumLoad;     // demand
        std::vector<std::int64_t> cumService;  // service seconds
        std::vector<std::int64_t> cumPenalty;  // unassigned penalty (pool term)
        // Driving along the list: cumDist[k] = drive into element k-1 (from
        // its predecessor, or from home for k == 1), so the internal distance
        // of [i, j) is cumDist[j] - cumDist[i + 1]. cumRevDist is the same
        // for the reversed traversal: element k-1 -> element k-2.
        std::vector<std::int64_t> cumDist;
        std::vector<std::int64_t> cumRevDist;
        // Earliest deadline over the first k elements (prefixMinEnd[k]) and
        // over the elements from k on (suffixMinEnd[k]); entry n / entry 0 is
        // the list-wide minimum. A middle piece is scanned.
        std::vector<Seconds> prefixMinEnd;
        std::vector<Seconds> suffixMinEnd;
        // Lateness of the list as a vehicle route, valid while noWaiting_
        // (arrival times are then prefix sums too).
        std::int64_t lateness = 0;
    };

    // One list's proposed content as a short run of pieces. Fixed capacity
    // keeps a preview free of heap allocation: no move here needs more than
    // five pieces (an intra-route swap) and a split insert adds two.
    struct Run
    {
        static constexpr int kCapacity = 7;
        int list = -1;
        int count = 0;
        Piece pieces[kCapacity];

        void add(const Piece &piece)
        {
            assert(count < kCapacity);
            pieces[count++] = piece;
        }
        // Insert `piece` so that it starts at element `index` of the run,
        // splitting the piece that position falls in.
        void insertAt(int index, const Piece &piece);
    };

    void rebuildCache(int listIndex);
    bool previewRuns(const Run *runs, int runCount, optimaru::HardSoftScore &out) const;
    // The exact O(route) path: materialize and walk. Used on a plan with
    // waiting and whenever the cached sums cannot rule lateness out.
    bool previewRunsByWalk(const Run *runs, int runCount, optimaru::HardSoftScore &out) const;
    bool previewInsertByWalk(int list, int pos, int element, optimaru::HardSoftScore &out) const;
    bool previewsEnabled() const { return plan_ != nullptr; }
    // Earliest deadline among the elements [begin, end) of a cached list.
    Seconds minEndOf(const ListCache &cache, const std::vector<int> &elements, std::size_t begin,
                     std::size_t end) const;
    const std::vector<int> &elementsOf(int list) const
    {
        return list >= vehicleCount_
                   ? plan_->unassignedVisitIndices
                   : plan_->vehicles[static_cast<std::size_t>(list)].visitIndices;
    }
    int listSize(int list) const { return static_cast<int>(elementsOf(list).size()); }

    // Per-vehicle terms, all derived in one walk of the route.
    struct RouteTerms
    {
        std::int64_t overload = 0;  // hard: max(0, demand - capacity)
        std::int64_t lateness = 0;  // hard: whole minutes past maxEndTime
        std::int64_t driving = 0;   // soft: seconds, home -> visits (-> home if returnToDepot)
    };

    RouteTerms evaluateRoute(int vehicleIndex) const;
    // The same walk over an arbitrary visit sequence (a previewed route).
    RouteTerms evaluateElements(int vehicleIndex, const std::vector<int> &elements) const;

    RouteTerms termsFromCache(int vehicleIndex) const;
    std::int64_t unassignedMinutes() const;

    // Ceil of a positive second-count to whole minutes, matching
    // Visit.roundDurationToNextOrEqualMinutes.
    static std::int64_t ceilToMinutes(Seconds seconds)
    {
        return seconds <= 0 ? 0 : (seconds + 59) / 60;
    }

    VehicleRoutePlan *plan_ = nullptr;
    int vehicleCount_ = 0;

    std::vector<RouteTerms> terms_;  // per vehicle
    std::vector<ListCache> caches_;  // per list (vehicles, then the pool)
    // No visit's window opens after any vehicle leaves, so arrival is always
    // the service start and finish times follow from distances alone. Only
    // then are the prefix-sum caches built and the O(1) previews served.
    bool noWaiting_ = false;
    mutable std::vector<int> walkScratch_;  // waiting-plan previews: the candidate route

    // Running totals, all >= 0 (they are penalties; the score negates them).
    std::int64_t overloadTotal_ = 0;
    std::int64_t latenessTotal_ = 0;
    std::int64_t drivingTotal_ = 0;
    std::int64_t unassignedTotal_ = 0;
};

}  // namespace vrp

#endif  // VRP_INCREMENTAL_CALCULATOR_HPP
