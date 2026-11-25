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

from ._config import set_nested
from ._solve import FIRST_FEASIBLE_LIMIT

__all__ = [
    "AcceptanceGroup",
    "AlnsGroup",
    "AlnsTriggerGroup",
    "MovesGroup",
    "SolveConfigBase",
    "SolveRequest",
    "StopGroup",
    "StrictModel",
    "apply_solver_parameters",
    "entities_by_id",
    "require_same_order",
    "solution_dict",
    "solve_model_request",
    "valid_positions",
    "write_groups",
]


class StrictModel(BaseModel):
    """A pydantic model that refuses unknown fields, so a misspelled key is an error and not ignored."""

    model_config = ConfigDict(extra="forbid")


# ---------------------------------------------------------------------------
# A SolverConfig in JSON: the same groups as the compiled one (optimaru._config)
# ---------------------------------------------------------------------------


class StopGroup(StrictModel):
    """``config.stop`` beyond the time limit (that one is the request's ``time_limit_seconds``)."""

    unimproved_time_limit_seconds: float = Field(default=0.0, ge=0)
    step_limit: int = Field(default=0, ge=0)
    unimproved_step_limit: int = Field(default=0, ge=0)
    target_score: tuple[int, float] | None = None


class AcceptanceGroup(StrictModel):
    """``config.acceptance``: late acceptance's history, 0 for the use case's automatic size."""

    late_acceptance_size: int = Field(default=0, ge=0)


class AlnsTriggerGroup(StrictModel):
    """``config.alns_trigger``: whether a stalled search is kicked, and after how many steps."""

    enabled: bool = True
    unimproved_steps: int = Field(default=5000, ge=0)


class AlnsGroup(StrictModel):
    """``config.alns``: the kick."""

    min_ruin_fraction: float = Field(default=0.10, ge=0)
    max_ruin_fraction: float = Field(default=0.30, ge=0)
    max_ruin_count: int = Field(default=60, ge=1)
    operator_decay: float = Field(default=0.8, ge=0, le=1)
    rewards: list[float] = Field(default_factory=lambda: [33.0, 9.0, 3.0, 0.0], min_length=4, max_length=4)
    polish_steps: int = Field(default=5000, ge=0)

    @model_validator(mode="after")
    def _valid_ruin_fraction_range(self):
        if self.min_ruin_fraction > self.max_ruin_fraction:
            raise ValueError("alns.min_ruin_fraction must be <= alns.max_ruin_fraction")
        return self


class MovesGroup(StrictModel):
    """``config.moves`` of an assignment use case: the move mix."""

    change_weight: float = Field(default=0.75, ge=0)
    swap_weight: float = Field(default=0.25, ge=0)


#: The groups a JSON config may hold, in `optimaru._config.GROUPS` order.
_GROUP_FIELDS = (
    "construction",
    "stop",
    "cost",
    "alns_trigger",
    "strategies",
    "custom_moves",
    "moves",
    "selection",
    "acceptance",
    "alns",
    "run",
)


def write_groups(model: BaseModel, cfg, *, skip: frozenset[str] = frozenset({"cost"})) -> None:
    """Copy every group of ``model`` (a JSON config) onto ``cfg``, a compiled ``SolverConfig``, by name.

    A group field of the model holds the settings of the config's group of the same name,
    under the same names, so the copy needs no list of its own. ``None`` leaves a setting
    as the config has it. ``cost`` is left to the use case: its settings are objects of the
    use case's own (school timetabling's weights), which it converts itself.
    """
    for group in _GROUP_FIELDS:
        settings = getattr(model, group, None)
        if settings is None or group in skip:
            continue
        view = getattr(cfg, group)
        for name, value in settings.model_dump().items():
            if value is not None:
                setattr(view, name, value)


class SolveConfigBase(StrictModel):
    """The solver settings of a ``models.SolveConfig``: a ``SolverConfig`` in JSON.

    The settings a solve call takes as keywords sit at the top under those names
    (``time_limit_seconds``, ``random_seed``, ``parallel_solver_count``,
    ``stop_at_first_feasible``, ``full_assert``); everything else is grouped as the compiled
    ``SolverConfig`` groups it (`optimaru._config`), under the same names:
    ``{"alns": {"min_ruin_fraction": 0.05}}``. Flight crew scheduling, sports league
    scheduling and school timetabling use these groups as they are; conference scheduling
    adds settings of its own to some of them.
    """

    time_limit_seconds: float = Field(default=30.0, ge=0)
    random_seed: int = 0
    parallel_solver_count: int = Field(default=1, ge=1)
    stop_at_first_feasible: bool = False
    full_assert: bool = False

    stop: StopGroup = Field(default_factory=StopGroup)
    acceptance: AcceptanceGroup = Field(default_factory=AcceptanceGroup)
    alns_trigger: AlnsTriggerGroup = Field(default_factory=AlnsTriggerGroup)
    alns: AlnsGroup = Field(default_factory=AlnsGroup)
    moves: MovesGroup = Field(default_factory=MovesGroup)

    #: The target "stop at the first feasible solution" sets on this use case's score.
    _first_feasible: tuple = FIRST_FEASIBLE_LIMIT

    def _apply_to(self, cfg):
        """Write these settings into ``cfg``, a compiled ``SolverConfig``, and return it."""
        cfg.stop.time_limit_seconds = self.time_limit_seconds
        cfg.run.random_seed = self.random_seed
        cfg.run.parallel_solver_count = self.parallel_solver_count
        cfg.run.full_assert = self.full_assert
        write_groups(self, cfg)
        if self.stop_at_first_feasible and self.stop.target_score is None:
            cfg.stop.target_score = self._first_feasible
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

    The parameters come by group, as the config has them (`optimaru._config`):
    ``{"alns": {"min_ruin_fraction": 0.05}, "moves": {"two_opt_weight": 0.2}}``. The four
    the solve request controls (the time limit, the seed, the worker count and the
    callback) are refused, and so is a group or a name that does not exist (a flat name
    of an earlier version is refused with its new place). ``None`` leaves a setting as it
    is, a JSON list becomes the ``stop.target_score`` tuple, and the name of an enum member
    (``"HILL_CLIMBING"``) becomes the member. Raises ValueError naming the parameter.
    """
    set_nested(config, parameters, refused=_REQUEST_CONTROLLED)


#: Settings a solve request sets with its own fields, so ``solver_parameters`` may not.
_REQUEST_CONTROLLED = frozenset(
    {"stop.time_limit_seconds", "run.random_seed", "run.parallel_solver_count", "run.on_new_best"}
)


class SolveRequest(StrictModel):
    """The settings of one solve, in a use case's pydantic request (``models.<Name>Request``).

    The fields are named like the keywords of ``solve()``, so a request and a call say
    the same thing in the same words: ``time_limit_seconds``, ``random_seed``,
    ``parallel_solver_count``, ``stop_at_first_feasible`` and ``full_assert``.
    ``solver_parameters`` sets further ``SolverConfig`` settings by group (see
    `apply_solver_parameters`). The names of earlier versions are accepted as aliases:
    ``timelimit`` for ``time_limit_seconds`` and ``num_workers`` for
    ``parallel_solver_count``, in JSON, as keywords and as attributes.

    ``name`` is a label for the caller's own use; no solve reads it. Where a request has
    both ``stop_at_first_feasible`` and a ``stop.target_score`` (in ``solver_parameters``
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
        default_factory=dict,
        description='Further SolverConfig settings by group, such as {"stop": {"step_limit": 1000}}.',
    )

    def model_copy(self, *, update=None, deep: bool = False):
        """`BaseModel.model_copy`, with the earlier field names in ``update`` read as the new ones.

        pydantic does not read aliases in ``update``: without this, ``{"timelimit": 5}`` would
        be stored beside the fields and the copy would keep its old time limit. A name given
        together with its earlier name is refused, as it is when a request is built.
        """
        if update:
            aliases = {"timelimit": "time_limit_seconds", "num_workers": "parallel_solver_count"}
            for old, new in aliases.items():
                if old in update and new in update:
                    raise ValueError(f"{new} and {old} are the same field: give one")
            update = {aliases.get(name, name): value for name, value in update.items()}
        return super().model_copy(update=update, deep=deep)

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
