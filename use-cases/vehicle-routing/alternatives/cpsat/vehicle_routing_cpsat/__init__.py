"""CP-SAT backend for :mod:`vrp_routing`."""

from .solve import catalog, solve_request

solve = solve_request

__all__ = ["catalog", "solve", "solve_request"]
