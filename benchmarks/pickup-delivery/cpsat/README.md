# Pickup and delivery CP-SAT

Pickup/dropoff-coupled optional circuits with same-vehicle assignment,
pickup-first precedence, running capacity, time windows, optional requests,
and travel minimization. Routes are open by default; a vehicle with
`return_to_depot=True` also drives (and is timed) back to its home. The package
shares `pdp_routing.models` with the C++ backend.
