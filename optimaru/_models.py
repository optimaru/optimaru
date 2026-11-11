"""What the use cases' ``models.py`` share.

``optimaru.<use case>.models`` is the pydantic side of a use case: the JSON shape of
its problem (or of a whole request) and the conversion to and from the compiled
classes. The parts several of them need are written once, here:

- `StrictModel`, the base of the models: an unknown field is an error.
- `SolveConfigBase`, the solver settings a ``models.SolveConfig`` mirrors.
- `solution_dict`, `entities_by_id`, `require_same_order` and `valid_positions`, the
  checks ``with_initial_solution`` makes while it carries a solution over to a problem.
- `apply_solver_parameters`, a request's free-form ``solver_parameters`` on a
  ``SolverConfig``.
- `SolveRequest`, the base of every use case's request model, and `solve_model_request`,
  the ``solve_request`` of a use case whose request wraps its problem model.

Every use case solves a compiled problem with ``solve(problem, ...)`` and a pydantic
request with ``models.solve_request(request, ...)``. A request carries the settings of
one solve under the names of ``solve``'s keywords (`SolveRequest`).
"""

from typing import Any

from pydantic import AliasChoices, BaseModel, ConfigDict, Field, model_validator

from ._solve import FIRST_FEASIBLE_LIMIT

__all__ = [
    "SolveConfigBase",
    "SolveRequest",
    "StrictModel",
    "apply_solver_parameters",
    "entities_by_id",
    "require_same_order",
    "solution_dict",
    "solve_model_request",
    "valid_positions",
]


class StrictModel(BaseModel):
    """A pydantic model that refuses unknown fields, so a misspelled key is an error and not ignored."""

    model_config = ConfigDict(extra="forbid")


class SolveConfigBase(StrictModel):
    """The solver settings of a ``models.SolveConfig``, where they are exactly these.

    A serializable mirror of the ``SolverConfig`` fields that flight crew scheduling,
    sports league scheduling and school timetabling have in common: their ``SolveConfig``
    builds on it and adds ``to_core()``. On the compiled config ``time_limit_seconds`` is
    ``spent_limit_seconds`` and ``stop_at_first_feasible`` sets ``best_score_limit``; every
    other field keeps its name. ``late_acceptance_size`` 0 is automatic, by the use case's
    own rule; a positive value is used as it is.

    Conference scheduling keeps a mirror of its own: its ``SolverConfig`` has more fields,
    and its mirror does not check the ruin fraction range.
    """

    time_limit_seconds: float = Field(default=30.0, ge=0)
    random_seed: int = 0
    parallel_solver_count: int = Field(default=1, ge=1)
    late_acceptance_size: int = Field(default=0, ge=0)
    stop_at_first_feasible: bool = False
    best_score_limit: tuple[int, float] | None = None
    unimproved_spent_limit_seconds: float = Field(default=0.0, ge=0)
    step_count_limit: int = Field(default=0, ge=0)
    unimproved_step_count_limit: int = Field(default=0, ge=0)
    full_assert: bool = False

    ruin_recreate: bool = True
    ruin_recreate_unimproved_step_trigger: int = Field(default=5000, ge=0)
    ruin_recreate_min_ruin_fraction: float = Field(default=0.10, ge=0)
    ruin_recreate_max_ruin_fraction: float = Field(default=0.30, ge=0)
    ruin_recreate_max_ruin_count: int = Field(default=60, ge=1)
    ruin_recreate_operator_decay: float = Field(default=0.8, ge=0, le=1)
    ruin_recreate_rewards: list[float] = Field(
        default_factory=lambda: [33.0, 9.0, 3.0, 0.0],
        min_length=4,
        max_length=4,
    )
    ruin_recreate_polish_steps: int = Field(default=5000, ge=0)

    change_move_weight: float = Field(default=0.75, ge=0)
    swap_move_weight: float = Field(default=0.25, ge=0)

    @model_validator(mode="after")
    def _valid_ruin_fraction_range(self):
        if self.ruin_recreate_min_ruin_fraction > self.ruin_recreate_max_ruin_fraction:
            raise ValueError("ruin_recreate_min_ruin_fraction must be <= ruin_recreate_max_ruin_fraction")
        return self

    def _apply_to(self, cfg):
        """Write these settings into ``cfg``, a compiled ``SolverConfig``, and return it."""
        cfg.spent_limit_seconds = self.time_limit_seconds
        cfg.random_seed = self.random_seed
        cfg.parallel_solver_count = self.parallel_solver_count
        cfg.late_acceptance_size = self.late_acceptance_size  # 0 = auto
        cfg.unimproved_spent_limit_seconds = self.unimproved_spent_limit_seconds
        cfg.step_count_limit = self.step_count_limit
        cfg.unimproved_step_count_limit = self.unimproved_step_count_limit
        cfg.full_assert = self.full_assert
        cfg.ruin_recreate = self.ruin_recreate
        cfg.ruin_recreate_unimproved_step_trigger = self.ruin_recreate_unimproved_step_trigger
        cfg.ruin_recreate_min_ruin_fraction = self.ruin_recreate_min_ruin_fraction
        cfg.ruin_recreate_max_ruin_fraction = self.ruin_recreate_max_ruin_fraction
        cfg.ruin_recreate_max_ruin_count = self.ruin_recreate_max_ruin_count
        cfg.ruin_recreate_operator_decay = self.ruin_recreate_operator_decay
        cfg.ruin_recreate_rewards = list(self.ruin_recreate_rewards)
        cfg.ruin_recreate_polish_steps = self.ruin_recreate_polish_steps
        cfg.change_move_weight = self.change_move_weight
        cfg.swap_move_weight = self.swap_move_weight
        if self.stop_at_first_feasible:
            # The use cases built on this base all score (hard, soft).
            cfg.best_score_limit = FIRST_FEASIBLE_LIMIT
        if self.best_score_limit is not None:
            cfg.best_score_limit = self.best_score_limit
        return cfg


def solution_dict(solution) -> dict:
    """``solution`` as a dict: a solved model is dumped, the JSON a solve returned is taken as it is."""
    if isinstance(solution, BaseModel):
        return solution.model_dump()
    if isinstance(solution, dict):
        return solution
    raise ValueError(f"solution must be a solved model or a dict of that shape: {type(solution)!r}")


def entities_by_id(solved: dict, name: str, ids: list, key: str = "id") -> dict:
    """`solved[name]` by id. Rejects an id repeated on either side and an id outside `ids`."""
    entities = solved.get(name)
    if not isinstance(entities, list):
        raise ValueError(f"solution has no {name!r} list")
    known = set(ids)
    if len(known) != len(ids):
        raise ValueError(f"problem repeats an id in {name!r}: {next(i for i in ids if ids.count(i) > 1)!r}")
    by_id: dict = {}
    for entity in entities:
        entity_id = entity.get(key) if isinstance(entity, dict) else None
        if not isinstance(entity_id, (str, int)) or entity_id not in known:
            raise ValueError(f"solution has an unknown id in {name!r}: {entity_id!r}")
        if entity_id in by_id:
            raise ValueError(f"solution repeats an id in {name!r}: {entity_id!r}")
        by_id[entity_id] = entity
    return by_id


def require_same_order(solved: dict, name: str, ids: list, key: str = "id") -> None:
    """A positional value only means the same thing over the same list. `solved` may omit the list."""
    values = solved.get(name)
    if values is None:
        return
    if not isinstance(values, list) or [v.get(key) if isinstance(v, dict) else None for v in values] != ids:
        raise ValueError(f"solution {name!r} list differs from the problem's (same entries, same order required)")


def valid_positions(count: int) -> str:
    """The valid positions among `count`, for an error message."""
    return f"0..{count - 1}" if count else "(the list is empty)"


def apply_solver_parameters(config, parameters: dict[str, Any]) -> None:
    """Set a request's ``solver_parameters`` on ``config``, a compiled ``SolverConfig``.

    Each name is a field of ``config``. The four the solve request controls (the time
    limit, the seed, the worker count and the callback) are refused, and so is a name that
    is no field. ``None`` leaves a field as it is, a JSON list becomes the
    ``best_score_limit`` tuple, and the name of an enum member (``"HILL_CLIMBING"``)
    becomes the member. Raises ValueError naming the parameter.
    """
    for name, value in parameters.items():
        if name in {"spent_limit_seconds", "random_seed", "parallel_solver_count", "on_new_best"}:
            raise ValueError(f"solver parameter {name!r} is controlled by the solve request")
        if name.startswith("_") or not hasattr(config, name) or callable(getattr(config, name)):
            raise ValueError(f"unknown solver parameter: {name}")
        if value is None:
            continue
        if name == "best_score_limit" and isinstance(value, list):
            value = tuple(value)
        current = getattr(config, name)
        if isinstance(value, str) and hasattr(type(current), value):
            value = getattr(type(current), value)
        try:
            setattr(config, name, value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"invalid solver parameter {name!r}: {exc}") from exc


class SolveRequest(StrictModel):
    """The settings of one solve, in a use case's pydantic request (``models.<Name>Request``).

    The fields are named like the keywords of ``solve()``, so a request and a call say
    the same thing in the same words: ``time_limit_seconds``, ``random_seed``,
    ``parallel_solver_count``, ``stop_at_first_feasible`` and ``full_assert``.
    ``solver_parameters`` sets further ``SolverConfig`` fields by name (see
    `apply_solver_parameters`). The names of earlier versions are accepted as aliases:
    ``timelimit`` for ``time_limit_seconds`` and ``num_workers`` for
    ``parallel_solver_count``, in JSON, as keywords and as attributes.

    ``name`` is a label for the caller's own use; no solve reads it. Where a request has
    both ``stop_at_first_feasible`` and a ``best_score_limit`` (in ``solver_parameters``
    or, for employee scheduling, in ``settings``), the solve stops at the first feasible
    solution; employee scheduling is the exception and keeps the explicit limit.
    """

    model_config = ConfigDict(extra="forbid", validate_by_name=True, validate_by_alias=True)

    name: str | None = None
    time_limit_seconds: float = Field(
        default=30.0,
        ge=0,
        validation_alias=AliasChoices("time_limit_seconds", "timelimit"),
        description="Seconds the search may run. Alias: timelimit.",
    )
    random_seed: int = Field(default=0, description="Seed of the first start; start i uses random_seed + i.")
    parallel_solver_count: int = Field(
        default=1,
        ge=1,
        validation_alias=AliasChoices("parallel_solver_count", "num_workers"),
        description="Independent starts; the best is returned. Alias: num_workers.",
    )
    stop_at_first_feasible: bool = Field(default=False, description="Stop as soon as the hard score is 0.")
    full_assert: bool = Field(
        default=False, description="Check every incremental score against a full recompute (slow; for debugging)."
    )
    solver_parameters: dict[str, Any] = Field(
        default_factory=dict, description="Further SolverConfig fields by name, such as step_count_limit."
    )

    @property
    def timelimit(self) -> float:
        """Alias of `time_limit_seconds`."""
        return self.time_limit_seconds

    @timelimit.setter
    def timelimit(self, value: float) -> None:
        self.time_limit_seconds = value

    @property
    def num_workers(self) -> int:
        """Alias of `parallel_solver_count`."""
        return self.parallel_solver_count

    @num_workers.setter
    def num_workers(self, value: int) -> None:
        self.parallel_solver_count = value


def solve_model_request(solve, config_type, request: SolveRequest, **keywords):
    """``solve_request`` where the request wraps the use case's problem model as ``model_input``.

    It is ``solve(request.model_input, ...)`` with the request's settings as the keywords
    of the same names, and ``solver_parameters`` on a ``config_type()`` passed as
    ``config``. ``keywords`` (``constraints=``, and what else that use case's ``solve``
    takes) go through as they are. The result is what ``solve`` returns for the model:
    a new, solved model; the request is not changed.
    """
    config = None
    if request.solver_parameters:
        config = config_type()
        apply_solver_parameters(config, request.solver_parameters)
    return solve(
        request.model_input,
        time_limit_seconds=request.time_limit_seconds,
        random_seed=request.random_seed,
        parallel_solver_count=request.parallel_solver_count,
        stop_at_first_feasible=request.stop_at_first_feasible,
        full_assert=request.full_assert,
        config=config,
        **keywords,
    )
