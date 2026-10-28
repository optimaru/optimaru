"""Bed allocation — Python wrapper of the C++ solver on the optimaru engine.

`Department`/`Room`/`Bed`/`Stay`/`BedPlan`/`Score`/`Solver`/`SolverConfig` and
`calculate_score` come from the compiled C++ core (`_core`). `Stay.bed` is the
planning variable::

    -1                          uninitialized (no score contribution)
    0..bed_count()-1            real bed index
    bed_count() (no_bed_value)  "no bed" — the quickstart's null bed
                                (allowsUnassigned; penalized at the medium level)

Score convention (HardMediumSoftScore, as in the Java quickstart): hard is bed/gender/
age/required-equipment violations (0 == feasible), medium is the nights of unassigned
stays, soft is preference penalties (room size/specialty/preferred equipment).
Lexicographic comparison (hard first). Internally it maps onto HardSoftScore(int64, double)
as `engine.soft = medium * 10**9 + soft` (level order preserved).

The high-level entry point is `solve`::

    import bed_allocation as ba
    from bed_allocation.demo import generate_demo_data

    plan = generate_demo_data()                     # models.BedPlanModel
    result = ba.solve(plan, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_medium, result.score_soft)

The pydantic mirror (for serialization) is in `bed_allocation.models` (optional dependency).
"""

from ._core import (
    Bed,
    BedPlan,
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
    list_constraints,
)

__all__ = [
    "Bed",
    "BedPlan",
    "Department",
    "Gender",
    "GenderLimitation",
    "Room",
    "Score",
    "Solver",
    "SolverConfig",
    "Stay",
    "calculate_score",
    "solve",
    "list_constraints",
    "constraint_costs",
]


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
    plan,
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
    """High-level API that solves a bed allocation problem and returns it in the same shape.

    ``plan`` is a `_core.BedPlan` (solved in place and returned) or a
    `models.BedPlanModel` (converted to the engine shape, solved, and returned as a new
    `BedPlanModel`; needs pydantic).

    ``config`` becomes the base settings (`SolverConfig`; None = engine default
    30 s budget). Explicit keyword arguments override config.
    ``stop_at_first_feasible`` stops as soon as hard 0 is reached.
    ``on_new_best`` is called with ``(step, Score)`` on every new best.

    ``constraints`` picks the cost function directly
    (see `bed_allocation.constraints`). Only the listed constraints are scored;
    None uses all twelve default constraints::

        from bed_allocation.constraints import get_constraints
        ba.solve(plan, constraints=get_constraints())
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

    def _solve(core_plan):
        if constraints is None:
            Solver(cfg).solve(core_plan)
        else:
            Solver(cfg).solve_with_constraints(core_plan, constraints)

    if isinstance(plan, BedPlan):
        _solve(plan)
        return plan

    # pydantic model path (lazy import: pydantic is an optional dependency).
    from .models import BedPlanModel

    if not isinstance(plan, BedPlanModel):
        raise TypeError(f"plan must be a BedPlan or a models.BedPlanModel: {type(plan)!r}")
    core_plan = plan.to_core()
    _solve(core_plan)
    return BedPlanModel.from_core(core_plan)
