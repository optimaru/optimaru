#ifndef VRP_GROUP_MOVES_HPP
#define VRP_GROUP_MOVES_HPP

#include <memory>
#include <vector>

#include "optimaru/search/operators.hpp"
#include "vrp/domain.hpp"
#include "vrp/visit_insertion.hpp"

namespace vrp {

// ---------------------------------------------------------------------------
// Group move operators
// ---------------------------------------------------------------------------
// The engine's moves carry one visit, or a run of one route, at a time. The
// visits of a group (Visit::groupIndex) ride one vehicle together, so moving
// one of them to another vehicle splits the group, and every plan on the way
// to having moved all of them is infeasible. These operators move a group as
// one unit instead: its visits leave their routes together and go back on one
// vehicle, each at its cheapest feasible slot (visit_insertion.hpp) in an
// order that respects Visit::afterVisitIndices, or stay out together when no
// vehicle has room for all of them. No candidate ever holds a split group.
//
//   groupRuinRecreate  Takes one group out, alone or with a few more units
//                      (the groups and single visits of nearby stops, or
//                      other groups picked at random), and puts them back,
//                      the larger units first. Alone, it is the relocation of
//                      a group to its best vehicle, which also mends a group
//                      that is split; with others, it lets groups trade places
//                      and makes room for a group on a full vehicle. Groups
//                      waiting whole in the unassigned pool join the
//                      reinsertion, which is how they get routed.
//
// The operators only read the plan (the visits, the windows, the vehicle
// limits, the groups), never the objective, so they serve the default
// calculator and a caller's constraint assembly alike. They are registered
// only when the plan has a group of two or more visits.
struct GroupMoveSettings {
    bool ruinRecreate = true;
    // The most visits one groupRuinRecreate takes out beside its first group
    // (at least 0).
    int ruinMaxVisits = 30;
};

// The operators the settings turn on, to register with
// optimaru::Solver::add_move_operator. `groups` is built from the plan of the
// solve they are registered for.
std::vector<std::unique_ptr<optimaru::MoveOperator<VehicleRoutePlan>>> makeGroupMoves(
        const GroupMoveSettings& settings, std::shared_ptr<const VisitGroups> groups);

// Places the groups no route holds whole yet, each on the one vehicle that
// takes its waiting visits for the least driving: the vehicle that already
// carries most of the group when some of it is routed, any vehicle otherwise.
// A group no vehicle takes is parked in the unassigned pool, whole, and so is
// what is on no route of a group the caller has parked a visit of. Visits in
// no group are left to the engine's construction.
void constructGroups(VehicleRoutePlan& plan, const VisitGroups& groups);

}  // namespace vrp

#endif  // VRP_GROUP_MOVES_HPP
