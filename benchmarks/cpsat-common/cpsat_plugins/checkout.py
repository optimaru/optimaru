"""Run external-solver's CP-SAT models against the engine in this checkout.

The CP-SAT models live in external-solver (github.com/optimaru/external-solver)
and import the published layout, ``optimaru.<subpackage>``. A benchmark installs
the use case from ``use-cases/<slug>/python`` instead, under its own name
(``knapsack``, ``pdp_routing``, ...), so that every run measures the engine in
this checkout. ``use_checkout_engine`` registers that package and all of its
submodules under the ``optimaru.<subpackage>`` names before external-solver is
imported. Both solvers then build, select and score with the same module
objects: the same constraint classes, models and ``constraint_costs``.

The use-case packages import their own modules relatively, so they do not
depend on the name they are loaded under.
"""

from __future__ import annotations

import importlib
import pkgutil
import sys
import types

#: Repo-local import name -> ``optimaru`` subpackage (as in the root setup.py).
SUBPACKAGES = {
    "assembly_line_balancing": "assembly_line_balancing",
    "bed_allocation": "bed_allocation",
    "bin_packing": "bin_packing",
    "conference_scheduling": "conference_scheduling",
    "es_scheduling": "employee_scheduling",
    "facility_location": "facility_location",
    "flight_crew": "flight_crew_scheduling",
    "food_packaging": "food_packaging",
    "knapsack": "knapsack",
    "maintenance_scheduling": "maintenance_scheduling",
    "max_cut": "max_cut",
    "meeting_scheduling": "meeting_scheduling",
    "order_picking": "order_picking",
    "pdp_routing": "pickup_delivery",
    "project_job": "project_job_scheduling",
    "quadratic_assignment": "quadratic_assignment",
    "semiconductor_scheduling": "semiconductor_scheduling",
    "sports_league": "sports_league_scheduling",
    "task_assigning": "task_assigning",
    "tournament_scheduling": "tournament_scheduling",
}


def use_checkout_engine(package: str) -> None:
    """Make ``optimaru.<subpackage>`` the checkout's ``package``. Idempotent.

    Raises RuntimeError if ``optimaru.<subpackage>`` was already imported from
    somewhere else, since external-solver would then score with another engine.
    """
    subpackage = SUBPACKAGES[package]
    target = f"optimaru.{subpackage}"
    module = importlib.import_module(package)
    current = sys.modules.get(target)
    if current is module:
        return
    if current is not None:
        raise RuntimeError(
            f"{target} is already imported from {getattr(current, '__file__', current)!r}; "
            f"call use_checkout_engine({package!r}) before importing external_solver"
        )
    parent = sys.modules.get("optimaru")
    if parent is None:
        # The published package is not installed here (the benchmarks drop it), and
        # nothing else from it is needed: only the subpackages registered below.
        parent = types.ModuleType("optimaru")
        parent.__path__ = []
        sys.modules["optimaru"] = parent
    for info in pkgutil.walk_packages(module.__path__, prefix=f"{package}."):
        importlib.import_module(info.name)
    for name, loaded in list(sys.modules.items()):
        if name == package or name.startswith(f"{package}."):
            sys.modules[target + name[len(package) :]] = loaded
    setattr(parent, subpackage, module)
