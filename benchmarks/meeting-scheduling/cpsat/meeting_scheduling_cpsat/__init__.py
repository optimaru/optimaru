"""CP-SAT backend for :mod:`meeting_scheduling`."""

from .solve import catalog, solve

__all__ = ["catalog", "solve"]
