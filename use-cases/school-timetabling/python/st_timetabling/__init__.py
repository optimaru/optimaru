"""School timetabling — Python wrapper of the C++ solver on the optimaru engine.

The default entry point is the canonical full-parity pipeline:
`convert_problem` moves API_SPEC.md's `TimetableProblem` JSON into the
canonical C++ domain (`_core.canon`, every hard and soft constraint of the
CP-SAT reference), local search solves it, and `convert_solution` turns the
result back into a canonical `Timetable` JSON. `solve` bundles all of it::

    import st_timetabling as st

    result = st.solve(problem_dict, time_limit_seconds=10.0, random_seed=42)
    print(result["status"], result["penalties"], result["notes"])

The quickstart domain (7 teacher/class/room terms, slotRoom composite
encoding) remains available through `mode="quickstart"` and
`st_timetabling.quickstart` (an approximate conversion; its limits are in the
quickstart module docstring). The top-level classes `Timeslot`/`Room`/`Lesson`/
`Timetable`/`Solver`/`SolverConfig` belong to the quickstart domain; the
canonical domain classes live in the `canon` submodule (`canon.Lesson`,
`canon.Timetable`, `canon.Solver`, `canon.SolverConfig`,
`canon.ObjectiveWeights`, ...). The constraint catalog is the canonical one:
`list_constraints()`, `constraint_info()` (default level / weight per
constraint, read off the C++ components) and `constraint_costs(table)` (the raw
per-constraint costs of a `canon.Timetable`).

The cost function defaults to the monolithic incremental calculator (fast
path); `constraints=` selects a plugin assembly instead (see `constraints.py`)::

    from st_timetabling.constraints import Assembly

    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap_cost", weight=40).drop("teacher_workday_cost"),
    )

Pydantic mirrors (for serialization) live in `st_timetabling.models` (optional dependency).
"""

import copy
import time

from . import quickstart
from ._core import (
    ConstraintInfo,
    Lesson,
    Room,
    Score,
    Solver,
    SolverConfig,
    Timeslot,
    Timetable,
    calculate_score,
    canon,
    constraint_info,
)
from .constraints import (
    UNASSIGNED_LESSONS_SOFT_WEIGHT,
    Assembly,
    AvoidedPeriodCost,
    BlockAdjacencyCost,
    ClassConflictCost,
    ClassGapCost,
    Constraint,
    CourseMaxPeriodsPerDayCost,
    DisallowedSlotCost,
    FixedSlotsCost,
    NonPreferredPeriodCost,
    ParallelGroupsCost,
    RoomCapacityCost,
    TeacherConflictCost,
    TeacherFreeDayCost,
    TeacherGapCost,
    TeacherMaxConsecutivePeriodsCost,
    TeacherMaxPeriodsCost,
    TeacherWorkdayCost,
    UnassignedLessonsCost,
    get_constraints,
    list_constraints,
    spec_named,
)
from .convert import (
    ConvertedProblem,
    OccupancyGroup,
    convert_problem,
    convert_solution,
    inject_solution,
)

#: The raw per-constraint costs of a `canon.Timetable` (the canonical domain):
#: ``{name: {costs, weight, exponent, hard, global, level, reward,
#: level_flippable, name, label}}``, in list_constraints() order.
constraint_costs = canon.constraint_costs

__all__ = [
    "UNASSIGNED_LESSONS_SOFT_WEIGHT",
    "Assembly",
    "AvoidedPeriodCost",
    "BlockAdjacencyCost",
    "ClassConflictCost",
    "ClassGapCost",
    "Constraint",
    "ConstraintInfo",
    "ConvertedProblem",
    "CourseMaxPeriodsPerDayCost",
    "DisallowedSlotCost",
    "FixedSlotsCost",
    "Lesson",
    "NonPreferredPeriodCost",
    "OccupancyGroup",
    "ParallelGroupsCost",
    "Room",
    "RoomCapacityCost",
    "Score",
    "Solver",
    "SolverConfig",
    "TeacherConflictCost",
    "TeacherFreeDayCost",
    "TeacherGapCost",
    "TeacherMaxConsecutivePeriodsCost",
    "TeacherMaxPeriodsCost",
    "TeacherWorkdayCost",
    "Timeslot",
    "Timetable",
    "UnassignedLessonsCost",
    "calculate_score",
    "canon",
    "constraint_costs",
    "constraint_info",
    "convert_problem",
    "convert_solution",
    "get_constraints",
    "inject_solution",
    "list_constraints",
    "quickstart",
    "solve",
]

# The soft constraint names == the fast path's ObjectiveWeights fields (the
# objective_value rescoring weights): the soft rows of the catalog.
_SOFT_NAMES = tuple(row.name for row in constraint_info() if row.default_level == "soft")
_DEFAULT_WEIGHTS = {row.name: row.default_weight for row in constraint_info()}


def _weights_of(weights: "canon.ObjectiveWeights") -> dict[str, float]:
    return {name: getattr(weights, name) for name in _SOFT_NAMES}


def _picks_of(constraints) -> list[Constraint]:
    """`solve(constraints=...)`'s argument as a list of specs.

    An `Assembly` is taken as it is (its add() replaces a name it already
    holds); any other iterable of specs and/or names goes through
    `get_constraints`, so an empty list is a ValueError and a constraint
    named several times is one instance per pick — the same contract as the
    C++ `applyConstraints`."""
    if isinstance(constraints, Assembly):
        return list(constraints)
    return get_constraints([spec_named(item) if isinstance(item, str) else item for item in constraints])


def _assemble_plugin(constraints):
    """Assembly of the plugin path: (DecomposedCalculator, ObjectiveWeights for
    rescoring, unassigned-penalty weight).

    Each pick's ``weight=None`` is its component's default weight (the C++
    kDefaultWeight, `constraint_info()`), the same as on the fast path. The
    rescoring weights (objective_value) mirror the assembly: a picked soft
    constraint takes its effective weight, a soft constraint left out of the
    selection 0. A soft constraint picked several times takes the sum of its
    instances' effective weights, which is exactly their linear contribution
    (``w1 * c + w2 * c``). objective_value stays that linear weighted sum of the
    raw penalties: like a pick's exponent, an offset / clip shapes the search
    score only; `constraints.score(canon.constraint_costs(table, picks), picks)`
    is the exact score of the selection. They are plain numbers — no integer
    requirement.

    The third value is a float only for an assembly that demotes every
    instance of `unassigned_lessons_cost` to soft (the sum of their effective
    weights) or leaves it out altogether (0.0), and None while one instance
    stays hard — it tells `convert_solution` whether unassigned lessons may
    remain in the result and what each one costs.
    """
    picks = _picks_of(constraints)
    calculator = canon.DecomposedCalculator(picks)  # the C++ knob checks
    report = dict.fromkeys(_SOFT_NAMES, 0.0)  # soft terms missing from the selection are 0
    unassigned_weight: float | None = 0.0  # without the constraint, unassigned lessons are free
    for spec in picks:
        weight = _DEFAULT_WEIGHTS[spec.name] if spec.weight is None else float(spec.weight)
        if spec.name in report:
            report[spec.name] += weight  # a repeated soft pick: the instances add up
        if spec.name == UnassignedLessonsCost.name:
            # Hard means unassigned lessons cannot occur (the engine does not enable the moves
            # either); one hard instance is enough.
            if spec.level != "soft":
                unassigned_weight = None
            elif unassigned_weight is not None:
                unassigned_weight += weight
    return calculator, canon.ObjectiveWeights(**report), unassigned_weight


def _resolve_config(config, *, canonical: bool):
    """Converts the config argument into the core SolverConfig of the mode."""
    if config is None:
        return canon.SolverConfig() if canonical else SolverConfig()
    if canonical:
        if isinstance(config, canon.SolverConfig):
            return copy.copy(config)  # solve()'s overrides must not leak into it
        if hasattr(config, "to_canon"):
            return config.to_canon()
        raise ValueError("a canonical-mode config must be a canon.SolverConfig or an object with to_canon()")
    if isinstance(config, SolverConfig):
        return copy.copy(config)  # solve()'s overrides must not leak into it
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
    initial_solution: dict | None = None,
) -> dict:
    """High-level API: solves a canonical `TimetableProblem` dict and returns a
    canonical `Timetable` dict.

    ``mode="canonical"`` (default) honours every hard and soft constraint of
    the CP-SAT reference through the st::canon domain.
    ``mode="quickstart"`` is the approximate quickstart-domain conversion
    (unsupported constraints are reported in notes).

    ``initial_solution`` is where the search starts: a `Timetable` dict, as a
    solve returned it. Problems here are dicts, so this keyword takes the place
    of the `with_initial_solution` function of the other use cases, under the
    same contract. Lessons are matched by course id, and neither dict is
    changed. Periods the solution leaves out are placed by construction, and a
    solution holding every period is the exact starting point. `fixed_slots`
    are problem facts: they are kept, and a course's other slots must fit its
    free periods, so a complete solution that moves a fixed lesson is rejected. room_id is
    ignored, as it is recomputed from the result. Raises ValueError on an
    unknown course, a slot outside the calendar, a course given twice at one
    time, more slots than a course has free periods, or parallel sections at
    different times (see `inject_solution`). A time a course may not use is
    accepted and scores as a hard violation, so an INFEASIBLE result can be
    resumed.

    ``config`` becomes the base configuration: pass the mode's `_core`
    SolverConfig as is, or an object with `to_canon()`/`to_core()`
    (`models.SolveConfig`). None means the engine defaults (30 s budget).
    Explicit keyword arguments override config.

    ``weights`` is a dict of soft weights for the default fast path, keyed by
    the soft constraint names (a subset of teacher_gap_cost /
    teacher_workday_cost / teacher_free_day_cost / class_gap_cost /
    avoided_period_cost / non_preferred_period_cost; omitted keys keep the
    component defaults 20/3/30/10/5/1). It becomes ``config.weights``
    (`canon.ObjectiveWeights`, which rejects a bool, a non-finite or a negative
    weight). Canonical mode only. Next to ``constraints`` it must equal the
    defaults, like ``config.weights`` — a selection carries its own weights.

    ``constraints`` is a plugin constraint selection (canonical mode only; see
    the `constraints` module): an `Assembly`, or an iterable of specs and/or
    names (an empty list raises ValueError; a constraint named several times
    is one instance per pick, each with its own knobs).
    ``None`` (default) keeps the monolithic incremental calculator's fast path.
    When given, the search runs on the assembled `canon.DecomposedCalculator`:
    a pick's ``weight=None`` is the component default (never ``weights`` /
    ``config.weights``, which must then stay at the defaults), and the search
    is single-start (parallel_solver_count ignored). The response's penalties
    are always the canonical rescore (raw soft counts, independent of the
    selection), and objective_value uses the selection's effective soft
    weights (a picked soft constraint its weight, the sum of them for one
    picked several times, one left out 0; exponent / offset / clip shape the
    search score only). status also
    follows the canonical hard rescore (e.g. a selection that demotes a hard
    constraint to soft and then violates it is INFEASIBLE).

    Only a selection that demotes `unassigned_lessons_cost` to soft accepts
    unassigned lessons as a **result** (partial timetabling, the CP-SAT
    reference's relaxed weekly-period counts): the search gains unassign
    moves, the unfilled periods are reported as
    `penalties["unassigned_lessons_cost"]` instead of a hard violation, and
    status comes from the hard rescore without that term. One unassigned
    period costs that component's weight — give it
    `UNASSIGNED_LESSONS_SOFT_WEIGHT` (10000), above the sum of the other soft
    terms, so every period that can be filled is filled.

    ``stop_at_first_feasible`` stops as soon as hard 0 is reached
    (best_score_limit=(0, -inf)). Do not combine it with a selection relaxing
    `unassigned_lessons_cost` — a solution that drops every lesson is hard 0
    too and would stop there.

    The returned dict follows API_SPEC.md's response shape (status/lessons/
    objective_value/best_objective_bound/wall_time_seconds/penalties/notes).
    In canonical mode penalties are the raw soft counts keyed by the soft
    constraint names and objective_value the weighted penalty sum — the same
    unit as the cpsat backend.
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
        cfg.best_score_limit = (0, float("-inf"))
    if on_new_best is not None:
        cfg.on_new_best = on_new_best
    if weights is not None:
        unknown = sorted(set(weights) - set(_SOFT_NAMES))
        if unknown:
            raise ValueError(f"unknown soft weight(s) {unknown}; the weights are keyed by {list(_SOFT_NAMES)}")
        cfg.weights = canon.ObjectiveWeights(**weights)
    # weights= / config.weights at the defaults (e.g. a full default dict) are harmless next to a
    # selection; anything else would be silently ignored, so it raises.
    if constraints is not None and canonical and _weights_of(cfg.weights) != _weights_of(canon.ObjectiveWeights()):
        raise ValueError(
            "weights / config.weights apply to the default fast path only; with constraints=, give each pick its weight"
        )

    if initial_solution is not None and not (isinstance(initial_solution, dict) and "lessons" in initial_solution):
        raise ValueError("initial_solution has no lessons")

    if canonical:
        converted = convert_problem(problem)
        if initial_solution is not None:
            inject_solution(converted, initial_solution["lessons"])
        unassigned_weight = None
        if constraints is None:
            # Default: the monolithic incremental calculator's fast path.
            report_weights = cfg.weights
            start = time.perf_counter()
            canon.Solver(cfg).solve(converted.table)
            wall = time.perf_counter() - start
        else:
            # Plugin path: search with the assembled calculator; the rescoring
            # weights (convert_solution's objective_value) mirror the selection.
            calculator, report_weights, unassigned_weight = _assemble_plugin(constraints)
            start = time.perf_counter()
            canon.Solver(cfg).solve(converted.table, calculator)
            wall = time.perf_counter() - start
        return convert_solution(
            converted, wall_time_seconds=wall, unassigned_weight=unassigned_weight, weights=report_weights
        )

    converted = quickstart.convert_problem(problem)
    if initial_solution is not None:
        quickstart.inject_solution(converted, initial_solution["lessons"])
    start = time.perf_counter()
    Solver(cfg).solve(converted.table)
    wall = time.perf_counter() - start
    return quickstart.convert_solution(converted, wall_time_seconds=wall)
