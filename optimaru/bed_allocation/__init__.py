"""Bed allocation — Python wrapper of the C++ solver on the optimaru engine.

`Department`/`Room`/`Bed`/`Stay`/`BedPlan`/`Score`/`Solver`/`SolverConfig` and
`calculate_score` come from the compiled C++ core (`_core`). `Stay.bed` is the
decision variable::

    -1                          uninitialized (no score contribution)
    0..bed_count()-1            real bed index
    bed_count() (no_bed_value)  "no bed" — the stay is left unassigned
                                (penalized at the medium level)

Score convention (three levels, hard / medium / soft): hard is bed/gender/
age/required-equipment violations (0 == feasible), medium is the nights of unassigned
stays (`unassigned_stays_cost`), soft is preference penalties (room size/specialty/
preferred equipment). Lexicographic comparison (hard first). Internally it maps onto
Score(int64, double) as `engine.soft = medium * 10**9 + soft` (the domain's medium
fold; level order preserved).

The constraints and their defaults (level, weight in units of that level) live in the C++
components; `constraint_info()` reports them and `list_constraints()` lists the names::

    {row.name: (row.default_level, row.default_weight) for row in ba.constraint_info()}
    # {'bed_conflict_cost': ('hard', 1000.0), ..., 'unassigned_stays_cost': ('medium', 1.0), ...}

The high-level entry point is `solve`::

    import optimaru.bed_allocation as ba
    from optimaru.bed_allocation.demo import generate_demo_data

    plan = generate_demo_data()                     # models.BedPlanModel
    result = ba.solve(plan, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_medium, result.score_soft)

The pydantic mirror (for serialization) is in `optimaru.bed_allocation.models`.
"""

from typing import TypeVar

from .._solve import solve_in_place, solver_config
from ._core import (
    Bed,
    BedPlan,
    ConstraintInfo,
    Department,
    Gender,
    GenderLimitation,
    Room,
    Score,
    Solver,
    SolverConfig,
    Stay,
    calculate_score,
    constraint_costs,
    constraint_info,
    list_constraints,
)
from .models import BedPlanModel, solve_request, with_initial_solution

__all__ = [
    "Bed",
    "BedPlan",
    "ConstraintInfo",
    "Department",
    "Gender",
    "GenderLimitation",
    "Room",
    "Score",
    "Solver",
    "SolverConfig",
    "Stay",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "list_constraints",
    "solve",
    "solve_request",
    "with_initial_solution",
]


def _build_config(**settings) -> SolverConfig:
    """`solve()`'s `config` and keyword overrides as the `SolverConfig` of the solve."""
    # Stop as soon as hard 0 is reached: -(2**63) on medium/soft is "no floor", so the
    # engine compares hard only and no soft total, however low, misses the stop.
    return solver_config(SolverConfig, first_feasible_limit=(0, -(2**63), -(2**63)), **settings)


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


def solve(
    plan: _Problem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config=None,
    constraints=None,
) -> _Problem:
    """High-level API that solves a bed allocation problem and returns it in the same shape.

    ``plan`` is a `_core.BedPlan` (solved in place and returned) or a
    `models.BedPlanModel` (converted to the engine shape, solved, and returned as a new
    model, the one passed in left as it was).

    ``config`` becomes the base settings (`SolverConfig`; None = engine default
    30 s budget). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached.
    ``on_new_best`` is called with ``(step, Score)`` on every new best.

    ``constraints`` picks the cost function directly
    (see `optimaru.bed_allocation.constraints`). Only the listed constraints are scored;
    None uses all twelve default constraints::

        from optimaru.bed_allocation.constraints import UnassignedStaysCost, get_constraints
        ba.solve(plan, constraints=get_constraints())
        # unassigned nights as soft instead of medium:
        others = [spec for spec in get_constraints() if spec.name != "unassigned_stays_cost"]
        ba.solve(plan, constraints=[*others, UnassignedStaysCost(level="soft")])

    The result's score then reports each term on the level the selection gave it, and so
    does every score passed to ``on_new_best``.
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
    if isinstance(plan, BedPlan):
        solve_in_place(Solver(cfg), plan, constraints)
        return plan
    if not isinstance(plan, BedPlanModel):
        raise TypeError(f"plan must be a BedPlan or a models.BedPlanModel: {type(plan)!r}")
    core_plan = plan.to_core()
    solve_in_place(Solver(cfg), core_plan, constraints)
    return BedPlanModel.from_core(core_plan)
