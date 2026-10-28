"""School timetabling — Python wrapper of the C++ solver on the optimaru engine.

The default entry point is the canonical full-parity pipeline:
`convert_problem` moves API_SPEC.md's `TimetableProblem` JSON into the
canonical C++ domain (`_core.canon`, every constraint H1–H10 / S1–S6 of the
CP-SAT reference), local search solves it, and `convert_solution` turns the
result back into a canonical `Timetable` JSON. `solve` bundles all of it::

    import st_timetabling as st

    result = st.solve(problem_dict, time_limit_seconds=10.0, random_seed=42)
    print(result["status"], result["penalties"], result["notes"])

The quickstart domain (6 teacher/class/room clash constraints, slotRoom
composite encoding) remains available through `mode="quickstart"` and
`st_timetabling.quickstart` (an approximate conversion; its limits are in the
quickstart module docstring). The top-level classes `Timeslot`/`Room`/`Lesson`/
`Timetable`/`Solver`/`SolverConfig` belong to the quickstart domain; the
canonical domain classes live in the `canon` submodule (`canon.Lesson`,
`canon.Timetable`, `canon.Solver`, `canon.SolverConfig`,
`canon.ObjectiveWeights`, ...).

The cost function defaults to the monolithic incremental calculator (fast
path); `constraints=` selects a plugin assembly instead (see `constraints.py`)::

    from st_timetabling.constraints import Assembly

    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap", weight=40).drop("teacher_workday"),
    )

Pydantic mirrors (for serialization) live in `st_timetabling.models` (optional dependency).
"""

import time

from . import quickstart
from ._core import (
    Lesson,
    Room,
    Score,
    Solver,
    SolverConfig,
    Timeslot,
    Timetable,
    calculate_score,
    canon,
)
from .constraints import (
    Assembly,
    AvoidedPeriod,
    BlockAdjacency,
    ClassConflict,
    ClassGap,
    Constraint,
    CourseDailyLimit,
    NonPreferredPeriod,
    RoomCapacity,
    SlotNotAllowed,
    TeacherConflict,
    TeacherConsecutiveLimit,
    TeacherFreeDay,
    TeacherGap,
    TeacherLoadLimit,
    TeacherWorkday,
    UnassignedLesson,
    get_constraints,
    list_constraints,
)
from .convert import (
    ConvertedProblem,
    OccupancyGroup,
    convert_problem,
    convert_solution,
    inject_solution,
)

__all__ = [
    "Assembly",
    "AvoidedPeriod",
    "BlockAdjacency",
    "ClassConflict",
    "ClassGap",
    "Constraint",
    "ConvertedProblem",
    "CourseDailyLimit",
    "Lesson",
    "NonPreferredPeriod",
    "OccupancyGroup",
    "Room",
    "RoomCapacity",
    "Score",
    "SlotNotAllowed",
    "Solver",
    "SolverConfig",
    "TeacherConflict",
    "TeacherConsecutiveLimit",
    "TeacherFreeDay",
    "TeacherGap",
    "TeacherLoadLimit",
    "TeacherWorkday",
    "Timeslot",
    "Timetable",
    "UnassignedLesson",
    "calculate_score",
    "canon",
    "convert_problem",
    "convert_solution",
    "inject_solution",
    "get_constraints",
    "list_constraints",
    "quickstart",
    "solve",
]

# S1–S6 constraint names == ObjectiveWeights field names (used to fold the rescoring weights).
_SOFT_NAMES = (
    "teacher_gap",
    "teacher_workday",
    "teacher_free_day",
    "class_gap",
    "avoided_period",
    "non_preferred_period",
)


def _assemble_plugin(constraints, base_weights):
    """Assembly of the plugin path: (DecomposedCalculator, ObjectiveWeights for
    rescoring, unassigned-penalty weight).

    S components without an explicit weight follow ``base_weights`` (=
    config.weights with the `weights` argument applied) — the same rule as the
    monolithic path. The rescoring weights mirror the assembly: an overridden S
    weight takes that value, an S term dropped from the assembly becomes 0.
    `ObjectiveWeights` holds integers, so S weights must be integral (else
    ValueError).

    The third value is a float only for an assembly that demotes H1
    (`unassigned_lesson`) to soft (that component's effective weight; 0.0 when
    it is dropped altogether) and None while it stays hard — it hands the caller
    (`convert_solution`) whether unassigned lessons can remain in the result and
    what each one costs.
    """
    assembly = constraints if isinstance(constraints, Assembly) else Assembly(constraints)
    calculator = canon.DecomposedCalculator()
    report = dict.fromkeys(_SOFT_NAMES, 0)  # S terms missing from the assembly are 0 in rescoring too
    unassigned_weight: float | None = 0.0  # without H1 in the assembly, unassigned lessons are free
    for spec in assembly:
        weight = spec.weight
        if spec.name in report:
            if weight is None:
                weight = getattr(base_weights, spec.name)
            if float(weight) != int(weight) or weight < 0:
                raise ValueError(f"the weight of {spec.name} must be a non-negative integer (rescoring weight): {weight!r}")
            report[spec.name] = int(weight)
        if spec.name == UnassignedLesson.name:
            # Hard means unassigned lessons cannot occur (the engine does not enable the moves either).
            unassigned_weight = None if spec.hard is not False else float(1.0 if weight is None else weight)
        calculator.add(spec.name, weight=weight, hard=spec.hard, exponent=spec.exponent)
    return calculator, canon.ObjectiveWeights(**report), unassigned_weight


def _resolve_config(config, *, canonical: bool):
    """Converts the config argument into the core SolverConfig of the mode."""
    if config is None:
        return canon.SolverConfig() if canonical else SolverConfig()
    if canonical:
        if isinstance(config, canon.SolverConfig):
            return config
        if hasattr(config, "to_canon"):
            return config.to_canon()
        raise ValueError("a canonical-mode config must be a canon.SolverConfig or an object with to_canon()")
    if isinstance(config, SolverConfig):
        return config
    if hasattr(config, "to_core"):
        return config.to_core()
    raise ValueError("a quickstart-mode config must be a SolverConfig or an object with to_core()")


def solve(
    problem: dict,
    *,
    mode: str = "canonical",
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    weights: dict | None = None,
    constraints=None,
    on_new_best=None,
    config=None,
) -> dict:
    """High-level API: solves a canonical `TimetableProblem` dict and returns a
    canonical `Timetable` dict.

    ``mode="canonical"`` (default) honours every hard H1–H10 / soft S1–S6
    constraint of the CP-SAT reference through the st::canon domain.
    ``mode="quickstart"`` is the approximate quickstart-domain conversion
    (unsupported constraints are reported in notes).

    ``config`` becomes the base configuration: pass the mode's `_core`
    SolverConfig as is, or an object with `to_canon()`/`to_core()`
    (`models.SolveConfig`). None means the engine defaults (30 s budget).
    Explicit keyword arguments override config.

    ``weights`` is a dict of canonical soft weights (a subset of teacher_gap /
    teacher_workday / teacher_free_day / class_gap / avoided_period /
    non_preferred_period; omitted keys take the cpsat defaults). Canonical mode
    only.

    ``constraints`` is a plugin constraint assembly (canonical mode only; see
    the `constraints` module): an `Assembly` or an iterable of specs/names.
    ``None`` (default) keeps the monolithic incremental calculator's fast path —
    the default path's behaviour is unchanged. When given, the search runs on
    the assembled `canon.DecomposedCalculator`: S components without an explicit
    weight follow ``weights``, and the search is single-start
    (parallel_solver_count ignored). The response's penalties are always the
    canonical rescore (raw S1–S6 counts, independent of the assembly), and
    objective_value uses the effective S weights of the assembly (an override
    takes its value, a dropped term 0) — hence S weights must be non-negative
    integers. status also follows the canonical hard rescore (e.g. an assembly
    that demotes a hard constraint to soft and then violates it is UNKNOWN).

    Only an assembly that demotes H1 (`unassigned_lesson`) to soft accepts
    unassigned lessons as a **result** (the same contract as cpsat's
    `WeeklyPeriodCounts(hard=false)`, i.e. partial timetabling): the search gains
    unassign moves, unfilled periods are reported as
    `penalties["weekly_period_counts"]` instead of a hard violation, and status
    comes from the hard rescore without that term. One unassigned period costs
    that component's weight — keep it above the sum of the other soft terms
    (cpsat default 10000) so every period that can be filled is filled.

    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -1e18)). Do not combine it with an assembly relaxing
    H1 — a solution that drops every lesson is hard 0 too and would stop there.

    The returned dict follows API_SPEC.md's response shape (status/lessons/
    objective_value/best_objective_bound/wall_time_seconds/penalties/notes).
    In canonical mode penalties are the raw S1–S6 counts and objective_value the
    weighted penalty sum — the same unit as the cpsat backend.
    """
    if mode not in ("canonical", "quickstart"):
        raise ValueError(f"unknown mode: {mode!r} (canonical | quickstart)")
    canonical = mode == "canonical"
    if weights is not None and not canonical:
        raise ValueError("weights are supported in canonical mode only")
    if constraints is not None and not canonical:
        raise ValueError("constraints are supported in canonical mode only")

    cfg = _resolve_config(config, canonical=canonical)
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        cfg.best_score_limit = (0, -1e18)
    if on_new_best is not None:
        cfg.on_new_best = on_new_best
    if weights is not None:
        cfg.weights = canon.ObjectiveWeights(**weights)

    if canonical:
        converted = convert_problem(problem)
        unassigned_weight = None
        if constraints is None:
            # Default: the monolithic incremental calculator's fast path (unchanged).
            start = time.perf_counter()
            canon.Solver(cfg).solve(converted.table)
            wall = time.perf_counter() - start
        else:
            # Plugin path: search with the assembled calculator; the rescoring
            # weights (convert_solution's objective_value) mirror the assembly.
            calculator, report_weights, unassigned_weight = _assemble_plugin(constraints, cfg.weights)
            start = time.perf_counter()
            canon.Solver(cfg).solve(converted.table, calculator)
            wall = time.perf_counter() - start
            converted.table.weights = report_weights
        return convert_solution(converted, wall_time_seconds=wall, unassigned_weight=unassigned_weight)

    converted = quickstart.convert_problem(problem)
    start = time.perf_counter()
    Solver(cfg).solve(converted.table)
    wall = time.perf_counter() - start
    return quickstart.convert_solution(converted, wall_time_seconds=wall)
