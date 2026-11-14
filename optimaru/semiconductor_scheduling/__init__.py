"""Semiconductor fab scheduling — Python wrapper of the C++ solver on the optimaru engine.

Lots follow re-entrant routes over alternative tools (batch tools included), with
sequence-dependent setups, inter-tool transport, queue-time windows, maintenance windows,
auxiliary resources and pinned operations. The search state is one machine choice and one
priority rank per operation, folded into one assignment-shape decision value per operation row
(``rank * K + slot``); the C++ serial schedule-generation decoder turns it into the
schedule (``assignments``).

Score, at the default weights: hard = the sum of the 14 hard constraints,
soft = preference + minimum batch fill + load imbalance + 10 x weighted tardiness + cycle
time + makespan (see `optimaru.semiconductor_scheduling.constraints`; ``constraint_info()`` lists
every default level and weight).

Basic usage::

    import optimaru.semiconductor_scheduling as fab
    from optimaru.semiconductor_scheduling.models import SemiconductorScheduleModel

    problem = SemiconductorScheduleModel.model_validate(data)
    problem = fab.solve(problem, time_limit_seconds=5.0, random_seed=42)   # a new, solved model
    print(problem.score_hard, problem.score_soft, problem.makespan, problem.assignments[0])

``solve`` and ``calculate_score`` take the Pydantic model or the engine object
``_core.SemiconductorSchedule`` (``problem.to_core()``). ``solve`` returns the solved
problem: the engine object it was given, solved in place, or, for a model, a new model
with the result (the model passed in is not changed). ``calculate_score`` returns the
`Score` and writes the score fields into the model it is given.
"""

from __future__ import annotations

from typing import TypeVar

from .. import _api
from .._solve import solve_in_place, solver_config
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
from .models import AssignmentModel, SemiconductorScheduleModel, solve_request, with_initial_solution

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
    "solve_request",
    "with_initial_solution",
]


def _core_of(problem):
    """(engine object, the model to write back into or None)."""
    if isinstance(problem, _core.SemiconductorSchedule):
        return problem, None
    if isinstance(problem, SemiconductorScheduleModel):
        return problem.to_core(), problem
    raise TypeError(
        f"problem must be a SemiconductorSchedule or a models.SemiconductorScheduleModel: {type(problem)!r}"
    )


def _write_scores(model, core) -> None:
    scored = core.scored
    model.makespan = core.makespan if scored else None
    model.total_weighted_tardiness = core.total_weighted_tardiness if scored else None
    model.total_cycle_time = core.total_cycle_time if scored else None
    model.score_hard = core.score.hard if scored else None
    model.score_soft = core.score.soft if scored else None


def _assignment_models(core, assignments):
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


# solve() returns what it was given: the compiled problem, or its pydantic model.
_Problem = TypeVar("_Problem")


@_api.model_solve("semiconductor-scheduling", SemiconductorScheduleModel)
def solve(
    problem: _Problem,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config: SolverConfig | None = None,
    constraints=None,
) -> _Problem:
    """Solve `problem` and return it, solved.

    A `_core.SemiconductorSchedule` is solved in place and returned. A
    `models.SemiconductorScheduleModel` is converted, solved and returned as a new model
    with assignments (with batch ids), makespan, total_weighted_tardiness,
    total_cycle_time, score_hard, score_soft and warm_start=False filled in; the one
    passed in is left as it was.

    The score is on the result: ``problem.score`` (`Score`) on the engine object,
    ``score_hard`` / ``score_soft`` on the model.

    Without a warm start the input assignments are ignored; with one (see
    `with_initial_solution`) the search starts from them, and a complete seed is returned
    unchanged unless the search finds a strictly better schedule.

    ``config`` becomes the base settings (`SolverConfig`; None = engine defaults: 30 s,
    simulated annealing with an automatic temperature, 1% of the start's |soft|). Explicit
    keyword arguments override it. ``stop_at_first_feasible`` stops
    as soon as hard 0 is reached; ``on_new_best`` is called with ``(step, Score)``.
    A zero budget (``time_limit_seconds=0``) returns the start as is.

    ``constraints`` picks the cost function (see `optimaru.semiconductor_scheduling.constraints`):
    only the listed constraints are scored, a name picked several times once per pick, by a
    single start; None scores all 20 defaults.
    """
    cfg = solver_config(
        SolverConfig,
        config,
        time_limit_seconds=time_limit_seconds,
        random_seed=random_seed,
        parallel_solver_count=parallel_solver_count,
        stop_at_first_feasible=stop_at_first_feasible,
        full_assert=full_assert,
        on_new_best=on_new_best,
    )
    core, model = _core_of(problem)
    solve_in_place(Solver(cfg), core, constraints)
    if model is None:
        return problem
    # A model is not solved in place: the caller's stays the problem it was.
    solved = model.model_copy(deep=True)
    solved.assignments = _assignment_models(core, core.assignments)
    _write_scores(solved, core)
    solved.warm_start = False  # a seed is used once
    return solved


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
    ``{key: {"costs": [total], "weight", "exponent", "offset", "clip", "global",
    "level", ...}}`` — the input of `optimaru.semiconductor_scheduling.constraints.score`. None = the
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
