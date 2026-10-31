"""optimaru — metaheuristic optimization engine with ready-to-use models.

Each use case is a subpackage, loaded on first access so that importing
``optimaru`` does not load all compiled extensions::

    from optimaru import vehicle_routing
    import optimaru.school_timetabling as st

See https://optimaru.com and https://github.com/optimaru/optimaru.
"""

import importlib
from importlib.metadata import PackageNotFoundError, version

try:
    __version__ = version("optimaru")
except PackageNotFoundError:  # running from a source tree
    __version__ = "0.0.0"

__all__ = [
    "assembly_line_balancing",
    "bed_allocation",
    "bin_packing",
    "conference_scheduling",
    "employee_scheduling",
    "facility_location",
    "flight_crew_scheduling",
    "food_packaging",
    "knapsack",
    "maintenance_scheduling",
    "max_cut",
    "meeting_scheduling",
    "order_picking",
    "pickup_delivery",
    "project_job_scheduling",
    "quadratic_assignment",
    "school_timetabling",
    "semiconductor_scheduling",
    "sports_league_scheduling",
    "task_assigning",
    "tournament_scheduling",
    "vehicle_routing",
]


def __getattr__(name):
    if name in __all__:
        module = importlib.import_module(f"{__name__}.{name}")
        globals()[name] = module
        return module
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


def __dir__():
    return sorted(set(globals()) | set(__all__))


def _import_all():
    """Import every use case (used by the wheel smoke test)."""
    for name in __all__:
        importlib.import_module(f"{__name__}.{name}")
