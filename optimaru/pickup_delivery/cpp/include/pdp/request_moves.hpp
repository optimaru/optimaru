#ifndef PDP_REQUEST_MOVES_HPP
#define PDP_REQUEST_MOVES_HPP

#include <memory>
#include <vector>

#include "pdp/domain.hpp"
#include "pdp/request_insertion.hpp"
#include "optimaru/search/operators.hpp"

namespace pdp {

// ---------------------------------------------------------------------------
// Request-level move operators
// ---------------------------------------------------------------------------
// Moves whose unit is the request, not the stop: a pickup and its dropoff
// leave a route together and come back together, each at the cheapest feasible
// slot pair (request_insertion.hpp). No candidate ever holds a split or
// half-routed request, so the acceptor is offered feasible plans to compare
// instead of broken ones to reject.
//
//   requestRuinRecreate  Takes a few requests out and puts them back: large
//                        neighbourhood search (Shaw 1998; Ropke & Pisinger,
//                        "An Adaptive Large Neighborhood Search Heuristic for
//                        the Pickup and Delivery Problem with Time Windows",
//                        2006) at request granularity. One application picks
//                        how many (1 .. ruinMaxRequests) and which: at random,
//                        related ones (the requests of nearby stops), the ones
//                        whose removal saves the most driving, the requests of
//                        one route, or the requests of adjacent stretches of
//                        nearby routes. They are reinserted cheapest-first in
//                        random order or by regret-k, with blinking. Requests
//                        waiting in the unassigned pool join the reinsertion,
//                        which is how they get routed. Taking one request is
//                        the pair relocate with the best slot.
//
//   bundleRelocate       Moves a bundle — up to bundleMaxRequests requests
//                        whose pickups follow each other on one route — to one
//                        other route, each request at its cheapest slot there.
//                        It carries a cluster of orders across in one step,
//                        where moving them one by one would pass through plans
//                        that are worse or infeasible.
//
//   bundleExchange       Swaps a bundle of one route with a bundle of a nearby
//                        route, the two sized independently.
//
// Requests the input has put in one group (Delivery::groupIndex) are one unit
// in all three: they are taken out together and go back on one vehicle, each at
// its cheapest slot pair once the earlier ones are in, or stay out together
// when no vehicle has room for all of them (RequestGroups in
// request_insertion.hpp).
//
// The operators only read the plan (the stops, the windows, the vehicle
// limits, the groups), never the objective, so they serve the default
// calculator and a caller's constraint assembly alike.
struct RequestMoveSettings {
    bool ruinRecreate = true;
    // The most requests one requestRuinRecreate takes out (at least 1, and never
    // more than two fifths of the plan's requests).
    int ruinMaxRequests = 20;
    // Reinsertion by regret-k on half of the applications; below 2, always
    // cheapest-first in random order.
    int regretK = 3;
    // The chance a reinsertion skips a candidate slot. 0 disables blinking.
    double blinkRate = 0.01;

    bool bundleMoves = true;
    // The most requests in one bundle (at least 1).
    int bundleMaxRequests = 3;
};

// The operators the settings turn on, to register with
// optimaru::Solver::add_move_operator. They share their working state, so they
// belong to one engine and one solve; `groups` is built from that solve's plan.
std::vector<std::unique_ptr<optimaru::MoveOperator<PdpRoutePlan>>> makeRequestMoves(
        const RequestMoveSettings& settings, std::shared_ptr<const RequestGroups> groups);

}  // namespace pdp

#endif  // PDP_REQUEST_MOVES_HPP
