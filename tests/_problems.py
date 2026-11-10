"""One small problem per use case, for the contract tests that solve (tests/test_solve_contract.py).

Each entry is a problem the use case's own tests already solve: its demo generator, or the
builder in its test module. A new use case adds its line here.
"""

import importlib


def _call(module, function, *args, **kwargs):
    """A builder that imports `module` only when called: a narrowed build has one use case."""
    if module.startswith("."):
        module = __package__ + module
    return lambda: getattr(importlib.import_module(module), function)(*args, **kwargs)


def _core(build):
    return lambda: build().to_core()


SMALL_PROBLEMS = {
    "assembly_line_balancing": _call(".assembly_line_balancing.test_solve", "salbp"),
    "bed_allocation": _core(_call("optimaru.bed_allocation.demo", "generate_demo_data")),
    "bin_packing": _call(".bin_packing.test_solve", "web_demo"),
    "conference_scheduling": _call("optimaru.conference_scheduling", "demo_conference"),
    "employee_scheduling": _call(
        "optimaru.employee_scheduling", "make_random_roster", days=8, shifts_per_day=6, employees=9, seed=23
    ),
    "facility_location": _call(".facility_location.test_solve", "demo_problem"),
    "flight_crew_scheduling": _call(".flight_crew_scheduling.test_solve", "_mini_problem"),
    "food_packaging": _call("optimaru.food_packaging", "generate_demo_data", line_count=2, job_count=8),
    "knapsack": _call(".knapsack.test_solve", "demo"),
    "maintenance_scheduling": _call("optimaru.maintenance_scheduling", "generate_demo_data", "SMALL", 18659),
    "max_cut": _call(".max_cut.test_solve", "demo"),
    "meeting_scheduling": _call("optimaru.meeting_scheduling", "demo_data"),
    "order_picking": _call("optimaru.order_picking", "build_demo_data"),
    "pickup_delivery": _call(
        "optimaru.pickup_delivery", "build_random_instance", request_count=8, vehicle_count=3, seed=7
    ),
    "project_job_scheduling": _core(_call("optimaru.project_job_scheduling.demo", "generate_demo_data")),
    "quadratic_assignment": _call(".quadratic_assignment.test_solve", "demo"),
    "school_timetabling": _call(".school_timetabling.conftest", "demo_problem"),
    "semiconductor_scheduling": _call(
        ".semiconductor_scheduling.test_semiconductor_scheduling", "_fab_problem", with_assignments=False
    ),
    "sports_league_scheduling": _call("optimaru.sports_league_scheduling", "demo_schedule"),
    "task_assigning": _core(_call("optimaru.task_assigning.demo", "generate_demo_data")),
    "tournament_scheduling": _core(_call("optimaru.tournament_scheduling.models", "demo_data")),
    "vehicle_routing": _call(".vehicle_routing.test_routing", "small_plan"),
}

_LEVELS = ("hard", "medium", "soft", "soft0", "soft1", "soft2")


def small_problem(use_case: str):
    assert use_case in SMALL_PROBLEMS, f"add a small problem for {use_case} to tests/_problems.py"
    return SMALL_PROBLEMS[use_case]()


def score_of(solved) -> tuple:
    """The score of what solve() returned, as a tuple that orders as the score does."""
    score = getattr(solved, "score", None)
    if score is not None and hasattr(score, "hard"):
        return tuple(getattr(score, level) for level in _LEVELS if hasattr(score, level))
    levels = tuple(getattr(solved, f"score_{level}") for level in _LEVELS if hasattr(solved, f"score_{level}"))
    if not levels:
        raise TypeError(f"{type(solved).__name__} carries no score")
    return levels
