"""Semiconductor fab scheduling — Python wrapper of the C++ solver on the optimaru engine.

Lots follow re-entrant routes over alternative tools (batch tools included), with
sequence-dependent setups, inter-tool transport, queue-time windows, maintenance windows,
auxiliary resources and pinned operations. The search state is one machine choice and one
priority rank per operation, folded into one assignment-shape planning value per operation row
(``rank * K + slot``); the C++ serial schedule-generation decoder turns it into the
schedule (``assignments``).

Score, at the default weights: hard = the sum of the 14 hard constraints,
soft = preference + minimum batch fill + load imbalance + 10 x weighted tardiness + cycle
time + makespan (see `semiconductor_scheduling.constraints`; ``constraint_info()`` lists
every default level and weight).

Basic usage::

    import semiconductor_scheduling as fab
    from semiconductor_scheduling.models import SemiconductorScheduleModel

    problem = SemiconductorScheduleModel.model_validate(data)
    score = fab.solve(problem, time_limit_seconds=5.0, random_seed=42)   # writes into `problem`
    print(score, problem.makespan, problem.assignments[0])

``solve`` and ``calculate_score`` take the Pydantic model (written back in place) or the
engine object ``_core.SemiconductorSchedule`` (``problem.to_core()``; solved in place).
"""

from __future__ import annotations

import copy

from . import _core
from ._core import (
    AcceptorType,
    Assignment,
    AuxiliaryResource,
    ConstraintInfo,
    Lot,
    Machine,
    MachineOption,
    Operation,
    Score,
    SemiconductorSchedule,
    SetupTime,
    Solver,
    SolverConfig,
    TimeWindow,
    TransportTime,
    constraint_info,
    list_constraints,
)

__all__ = [
    "AcceptorType",
    "Assignment",
    "AuxiliaryResource",
    "ConstraintInfo",
    "Lot",
    "Machine",
    "MachineOption",
    "Operation",
    "Score",
    "SemiconductorSchedule",
    "SetupTime",
    "Solver",
    "SolverConfig",
    "TimeWindow",
    "TransportTime",
    "calculate_score",
    "constraint_costs",
    "constraint_info",
    "constraint_items",
    "list_constraints",
    "solve",
    "with_initial_solution",
]


def _core_of(problem):
    """(engine object, the model to write back into or None)."""
    if isinstance(problem, _core.SemiconductorSchedule):
        return problem, None
    return problem.to_core(), problem


def _write_scores(model, core) -> None:
    scored = core.scored
    model.makespan = core.makespan if scored else None
    model.total_weighted_tardiness = core.total_weighted_tardiness if scored else None
    model.total_cycle_time = core.total_cycle_time if scored else None
    model.score_hard = core.score.hard if scored else None
    model.score_soft = core.score.soft if scored else None


def _assignment_models(core, assignments):
    from .models import AssignmentModel

    machines = core.machines
    lots = core.lots
    return [
        AssignmentModel(
            lot_id=lots[item.lot].id,
            operation_id=lots[item.lot].operations[item.operation].id,
            machine_id=machines[item.machine].id,
            start=item.start,
            end=item.end,
            batch_id=item.batch_id,
        )
        for item in assignments
    ]


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    See `models.with_initial_solution`.
    """
    from . import models

    return models.with_initial_solution(problem, solution)


def solve(
    problem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> Score:
    """Solve `problem` and return the final `Score` (also a (hard, soft) pair).

    A `models.SemiconductorScheduleModel` is converted, solved and written back INTO THE
    SAME MODEL: assignments (with batch ids), makespan, total_weighted_tardiness,
    total_cycle_time, score_hard, score_soft and warm_start=False. A
    `_core.SemiconductorSchedule` is solved in place.

    Without a warm start the input assignments are ignored; with one (see
    `with_initial_solution`) the search starts from them, and a complete seed is returned
    unchanged unless the search finds a strictly better schedule.

    ``config`` becomes the base settings (`SolverConfig`; None = engine defaults: 30 s,
    simulated annealing with an automatic temperature, 1% of the start's |soft|). Explicit
    keyword arguments override it. ``stop_at_first_feasible`` stops
    as soon as hard 0 is reached; ``on_new_best`` is called with ``(step, Score)``.
    A zero budget (``time_limit_seconds=0``) returns the start as is.

    ``constraints`` picks the cost function (see `semiconductor_scheduling.constraints`):
    only the listed constraints are scored, a name picked several times once per pick, by a
    single start; None scores all 20 defaults.
    """
    # A copy: the keyword overrides below must not leak into the caller's config.
    cfg = copy.copy(config) if config is not None else SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        cfg.best_score_limit = (0, float("-inf"))
    if full_assert:
        cfg.full_assert = True
    if on_new_best is not None:
        cfg.on_new_best = on_new_best

    core, model = _core_of(problem)
    if constraints is None:
        Solver(cfg).solve(core)
    else:
        Solver(cfg).solve_with_constraints(core, constraints)
    if model is not None:
        model.assignments = _assignment_models(core, core.assignments)
        _write_scores(model, core)
        model.warm_start = False  # a seed is used once
    return core.score


def calculate_score(problem, constraints=None) -> Score:
    """Score ``problem.assignments`` as given and return the `Score`.

    Stores makespan, total_weighted_tardiness, total_cycle_time, score_hard and score_soft
    (on the model, or on the engine object). ``constraints`` = None scores the 20 defaults;
    a list scores exactly those picks, repeats and offsets included (the totals always come
    from every constraint).
    """
    core, model = _core_of(problem)
    score = _core.calculate_score(core, constraints)
    if model is not None:
        _write_scores(model, core)
    return score


def constraint_costs(problem, constraints=None) -> dict:
    """Raw cost of every constraint on ``problem.assignments``:
    ``{key: {"costs": [total], "weight", "exponent", "offset", "clip", "hard", "global",
    "level", ...}}`` — the input of `semiconductor_scheduling.constraints.score`. None = the
    default constraints; a pick list (as for `solve`) scores exactly those picks, each entry
    carrying its pick's knobs, the k-th pick of a name keyed ``"<name>#<k>"``. Every
    constraint is global, so ``costs`` holds its total as one element (the per-item
    lists are `constraint_items`)."""
    core, _model = _core_of(problem)
    return _core.constraint_costs(core, constraints)


def constraint_items(problem) -> dict[str, list[int]]:
    """The per-item raw costs of every constraint (one per lot, route pair, machine, window,
    ...). Their sums are the ``constraint_costs`` totals."""
    core, _model = _core_of(problem)
    return _core.constraint_items(core)


# --- decoder entry points the tests use ---------------------------------------------


def _operation_rows(problem):
    """The operation rows: (lot, route position, operation), lot order then route order."""
    return [(lot, index, operation) for lot in problem.lots for index, operation in enumerate(lot.operations)]


def _construct(problem, choices: list[int], ranks: list[int]):
    """Decode (choices, ranks) with the C++ decoder: a list of `models.AssignmentModel`."""
    core, _model = _core_of(problem)
    return _assignment_models(core, _core.decode(core, list(choices), list(ranks)))


def _seed_state(problem, rows, choices: list[int]) -> tuple[list[int], list[int]]:
    """``problem.assignments`` as a (choices, ranks) search state (``rows`` is only checked
    for its length)."""
    core, _model = _core_of(problem)
    if len(rows) != core.row_count():
        raise ValueError("rows must be the problem's operation rows")
    selected, ranks = _core.seed_state(core, list(choices))
    return list(selected), list(ranks)
