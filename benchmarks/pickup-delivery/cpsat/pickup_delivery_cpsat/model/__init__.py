"""The CP-SAT model of pickup and delivery the benchmark compares the engine with.

external-solver v0.3.0's ``external_solver.pickup_delivery`` (github.com/optimaru/external-solver,
Apache-2.0), kept here and ported to the optimaru 0.6 input (depots, ``start_depot`` / ``end_depot``,
travel profiles), since that library is frozen at the 0.5 input. Its plugin registry and errors
still come from the pinned ``external_solver``.
"""

from .solve import catalog, solve_request

solve = solve_request

__all__ = ["catalog", "solve", "solve_request"]
