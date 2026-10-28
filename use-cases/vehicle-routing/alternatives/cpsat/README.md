# Vehicle routing CP-SAT

Multiple-circuit CP-SAT backend using the canonical `RoutePlanRequest` and
`RouteSolution` contract. It models capacity overflow, time windows, optional
visits, service times, and total driving time lexicographically. Routes are
open by default; a vehicle with `return_to_depot=True` also drives (and is
timed) back to its depot.
