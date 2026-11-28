#ifndef CL_CONSTRUCT_CONSTRUCTION_HPP
#define CL_CONSTRUCT_CONSTRUCTION_HPP

#include <vector>

#include "cl/model/domain.hpp"

namespace cl {

// Where a new item goes in a loading order that is to stay sorted by
// unloading order: before the first item that leaves earlier than it (what
// leaves later is loaded first, so it ends up further from the door), at the
// end when there is none or the item has no unloadOrder.
int insertPosition(const ContainerLoadingProblem& problem, const std::vector<int>& list, int item);

// The order a container's items are loaded in when a list is built from
// scratch: what leaves last first (an item without an unloading order with
// the last); then the items that carry any load before those with a
// maxLoadOnTop, so that those end up on top; then largest first, by volume
// and by base area. True when item `a` is loaded before item `b`.
bool loadsBefore(const ContainerLoadingProblem& problem, int a, int b);

// ---------------------------------------------------------------------------
// First fit decreasing, by decode
// ---------------------------------------------------------------------------
// Puts every item that is in no container's list into one, and leaves the
// lists it finds (a warm start) as they are. The items go in this order: with
// unloading orders, what leaves last first; the items with a maxLoadOnTop
// after all the others; then group by group, the group with the most volume
// first (an item without a group stands for itself); inside a group largest
// first, by volume, then by base area. Each goes into
// the first container that takes it: one whose weight limit holds and whose
// decode, with the new item added, leaves no more items unplaced than before,
// no more weight on an item than it carries and no more items in the way of an
// earlier stop. Containers are
// tried in this order: those
// that already hold an item of the item's group, the other containers in use,
// then the empty ones, cheapest per volume first.
//
// An item no container takes goes to the container with the most free volume,
// where the decoder leaves it unplaced; the search then looks for a place.
//
// Deterministic: the same problem gives the same lists. The problem must be
// prepared (ScoreCalculator::prepare).
void constructLoading(ContainerLoadingProblem& problem);

}  // namespace cl

#endif  // CL_CONSTRUCT_CONSTRUCTION_HPP
