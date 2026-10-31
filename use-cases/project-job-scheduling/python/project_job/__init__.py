"""Project job scheduling (multi-project RCPSP) — Python wrapper of the C++
solver on the optimaru engine.

`Project`/`Resource`/`ResourceRequirement`/`ExecutionMode`/`Job`/`Allocation`/
`ProjectJobSchedule`/`JobType`/`Score`/`Solver`/`SolverConfig` and
`calculate_score` come from the compiled C++ core (`_core`).

The Java quickstart's two planning variables (executionMode, delay∈[0, 500)) are
ported as one composite-encoded BASIC variable `Allocation.mode_delay`::

    -1                                  uninitialized (no resource use;
                                        dates pass through with delay 0/duration 0)
    delay * max_mode_count + mode_slot  (real mode index = mode_slot %
                                        the job's mode count)

Java's shadow variable chain (predecessorsDoneDate → startDate → endDate) is
reproduced as derived state: `calculate_score` / `Solver.solve` recompute it in
topological order and fill `Allocation.start_date`/`end_date`. SOURCE/SINK
allocations are always pinned (exactly Java's @PlanningPin + setPinned), and if
uninitialized before solving they are fixed to (first mode, delay 0).

Score convention (HardMediumSoftScore, as in the Java quickstart), at the default weights:
  hard   1. non_renewable_resource_capacity_cost  use per non-renewable resource above
                                                  capacity, over the whole horizon
  hard   2. renewable_resource_capacity_cost      use per renewable resource x day above
                                                  capacity
  medium 3. project_delay_cost                    per-project delay (sink end - critical
                                                  path end, positive only), in days
  soft   4. makespan_cost                         the latest sink end date
(0 hard == feasible). Lexicographic comparison (hard first). Internally it maps onto
HardSoftScore(int64, double) with the domain's medium fold
`engine.soft = medium * 10**9 + soft` (level order preserved).

The constraints and their defaults (level, weight in units of that level) live in the C++
components; `constraint_info()` reports them, `list_constraints()` lists the names and
`constraint_costs()` gives the raw per-constraint costs of a schedule::

    {row.name: (row.default_level, row.default_weight) for row in pjs.constraint_info()}
    # {'non_renewable_resource_capacity_cost': ('hard', 1.0), ...,
    #  'project_delay_cost': ('medium', 1.0), 'makespan_cost': ('soft', 1.0)}

The high-level entry point is `solve`::

    import project_job as pj
    from project_job.demo import generate_demo_data

    schedule = generate_demo_data()                 # models.ProjectJobScheduleModel
    result = pj.solve(schedule, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_medium, result.score_soft)

The pydantic mirror (for serialization) is in `project_job.models` (optional dependency).
"""

from ._core import (
    Allocation,
    ConstraintInfo,
    ExecutionMode,
    Job,
    JobType,
    Project,
    ProjectJobSchedule,
    Resource,
    ResourceRequirement,
    Score,
    Solver,
    SolverConfig,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
)

__all__ = [
    "Allocation",
    "ConstraintInfo",
    "ExecutionMode",
    "Job",
    "JobType",
    "Project",
    "ProjectJobSchedule",
    "Resource",
    "ResourceRequirement",
    "Score",
    "Solver",
    "SolverConfig",
    "calculate_score",
    "solve",
    "list_constraints",
    "constraint_costs",
    "constraint_info",
    "with_initial_solution",
]


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    See `models.with_initial_solution`. Lazy import: pydantic is an optional dependency.
    """
    from . import models

    return models.with_initial_solution(problem, solution)


def _build_config(
    *,
    time_limit_seconds: float | None,
    random_seed: int | None,
    parallel_solver_count: int | None,
    stop_at_first_feasible: bool,
    full_assert: bool,
    on_new_best,
    config,
) -> SolverConfig:
    cfg = config if config is not None else SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        # Stop as soon as hard 0 is reached (medium/soft have an effectively -inf floor).
        cfg.best_score_limit = (0, -(2**62), -(2**62))
    if full_assert:
        cfg.full_assert = True
    if on_new_best is not None:
        cfg.on_new_best = on_new_best
    return cfg


def solve(
    schedule,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config=None,
    constraints=None,
):
    """High-level API that solves a project job scheduling problem and returns it in the same shape.

    ``schedule`` is a `_core.ProjectJobSchedule` (solved in place and
    returned) or a `models.ProjectJobScheduleModel` (converted to the engine shape,
    solved, and returned as a new `ProjectJobScheduleModel`; needs pydantic).

    ``config`` becomes the base settings (`SolverConfig`; None = engine default
    30 s budget). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached.
    ``on_new_best`` is called with ``(step, Score)`` on every new best.

    ``constraints`` picks the cost function directly
    (see `project_job.constraints`). Only the listed constraints are scored; None
    uses all four default constraints::

        from project_job.constraints import ProjectDelayCost, get_constraints
        pjs.solve(schedule, constraints=get_constraints())
        # project delay as soft days instead of medium:
        others = [spec for spec in get_constraints() if spec.name != "project_delay_cost"]
        pjs.solve(schedule, constraints=[*others, ProjectDelayCost(hard=False)])

    The result's score then reports each term on the level the selection gave it.
    """
    cfg = _build_config(
        time_limit_seconds=time_limit_seconds,
        random_seed=random_seed,
        parallel_solver_count=parallel_solver_count,
        stop_at_first_feasible=stop_at_first_feasible,
        full_assert=full_assert,
        on_new_best=on_new_best,
        config=config,
    )

    def _solve(core_schedule):
        if constraints is None:
            Solver(cfg).solve(core_schedule)
        else:
            Solver(cfg).solve_with_constraints(core_schedule, constraints)

    if isinstance(schedule, ProjectJobSchedule):
        _solve(schedule)
        return schedule

    # pydantic model path (lazy import: pydantic is an optional dependency).
    from .models import ProjectJobScheduleModel

    if not isinstance(schedule, ProjectJobScheduleModel):
        raise TypeError(
            f"schedule must be a ProjectJobSchedule or a models.ProjectJobScheduleModel: {type(schedule)!r}"
        )
    core_schedule = schedule.to_core()
    _solve(core_schedule)
    return ProjectJobScheduleModel.from_core(core_schedule)
