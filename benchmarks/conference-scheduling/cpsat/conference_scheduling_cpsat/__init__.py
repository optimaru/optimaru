"""CP-SAT backend for :mod:`conference_scheduling`."""

from .solve import catalog, solve

__all__ = ["catalog", "solve"]
