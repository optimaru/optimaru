#ifndef PDP_INCREMENTAL_CALCULATOR_HPP
#define PDP_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "pdp/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace pdp
{

// ---------------------------------------------------------------------------
// PdpIncrementalScoreCalculator
// ---------------------------------------------------------------------------
// The PDP constraints split into two groups with very different locality:
//
//  - Per-route terms (capacity peak, lateness, driving time) decompose exactly
//    like vehicle-routing's: one walk of a touched route re-derives all three.
//
//  - Coupling terms (paired / same-vehicle / pickup-before-dropoff) are NOT
//    per-route: they read both nodes of a request, which may sit in different
//    lists. They are maintained per *request* instead, in pairPenalty_, and
//    only the requests touching a changed list are recomputed.
//
// The bookkeeping that makes the second group work: beforeListChanged snapshots
// the list's old membership, and afterListChanged compares it with the new one
// to (a) mark every node that left as unassigned, (b) reindex every node that
// stayed or arrived, and (c) recompute the coupling penalty of every request
// on either side. A relocate touches two lists and therefore costs
// O(route) + O(requests in those routes), not O(instance).
//
// Transient states are expected and handled: between the remove hook pair and
// the insert hook pair a node is genuinely unassigned, and during an LNS ruin
// many nodes stay unassigned across several score evaluations. Both read as
// "half-assigned request", which is exactly what the reference implementation
// reports for the same state — so FULL_ASSERT agrees at every step.
class PdpIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<PdpRoutePlan>
{
public:
    void resetWorkingSolution(PdpRoutePlan &workingSolution) override;
    void beforeListChanged(int listIndex) override;
    void afterListChanged(int listIndex) override;
    optimaru::HardSoftScore calculateScore() override;

    // The HardMediumSoft view of the current state.
    Score currentScore() const;

private:
    struct RouteTerms
    {
        std::int64_t capacityExcess
            = 0;  // hard: summed over-capacity along the route
        std::int64_t lateness = 0;  // hard: whole minutes past maxEndTime
        // soft: seconds, home -> stops (-> home if returnToDepot)
        std::int64_t driving = 0;
    };

    RouteTerms evaluateRoute(int vehicleIndex) const;

    // The coupling penalty of one request from the current node index, plus
    // its medium-level contribution when the request is fully unrouted.
    std::int64_t couplingPenalty(int deliveryIndex) const;
    std::int64_t unassignedMinutes(int deliveryIndex) const;

    // Marks a request as needing recomputation, retracting its current
    // contribution once (repeated marks within one hook pair are no-ops).
    void markDelivery(int deliveryIndex);
    void flushDeliveries();

    PdpRoutePlan *plan_ = nullptr;
    int vehicleCount_ = 0;

    std::vector<RouteTerms> terms_;  // per vehicle

    // Live node index: which list each node is in, and where. This is the
    // TRUE list index — the unassigned pool (vehicleCount_) included — so that
    // afterListChanged can check ownership before releasing a node. -1 means
    // the node is in no list at all (transient, during an LNS ruin).
    std::vector<int> nodeList_;
    std::vector<int> nodePos_;

    // The vehicle serving a node, or -1 when it is pooled or in no list.
    int routeOf(int nodeIndex) const
    {
        const int list = nodeList_[static_cast<std::size_t>(nodeIndex)];
        return (list >= 0 && list < vehicleCount_) ? list : -1;
    }

    // Per-request maintained contributions.
    std::vector<std::int64_t> pairPenalty_;
    std::vector<std::int64_t> unassignedPenalty_;

    // Requests retracted during the current hook pair, awaiting recompute.
    std::vector<int> pendingDeliveries_;
    std::vector<char> pendingMark_;

    // Membership of the list currently between beforeListChanged and
    // afterListChanged, so departures can be detected.
    std::vector<int> oldMembers_;

    std::int64_t capacityTotal_ = 0;
    std::int64_t latenessTotal_ = 0;
    std::int64_t drivingTotal_ = 0;
    std::int64_t couplingTotal_ = 0;
    std::int64_t unassignedTotal_ = 0;
};

}  // namespace pdp

#endif  // PDP_INCREMENTAL_CALCULATOR_HPP
