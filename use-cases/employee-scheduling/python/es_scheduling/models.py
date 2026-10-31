"""Pydantic request/response models — JSON-friendly in/out for the solver.

Defines a request carrying workers, shifts, and a discriminated list of rules,
plus a response with a per-worker schedule and violation report:

    from es_scheduling.constraints import MaxConsecutiveDaysCost, RequiredSkillCost
    from es_scheduling.models import (
        ScheduleRequest, SchedulingInput, WorkerModel, ShiftModel, solve_request,
    )

    request = ScheduleRequest(
        timelimit=2,
        model_input=SchedulingInput(
            workers=[WorkerModel(id="Worker 01", skills=["Skill A"])],
            shifts=[ShiftModel(id="Shift 1", start=540, end=1020, required_skill="Skill A")],
            rules=[RequiredSkillCost(), MaxConsecutiveDaysCost(value=6, weight=2)],
        ),
    )
    solution = solve_request(request)      # -> ScheduleSolution
    print(solution.schedule)               # {"Worker 01": [ShiftAssignmentModel(...)]}
    print(solution.model_dump_json(indent=2))

Everything round-trips through JSON (`ScheduleRequest.model_validate_json`,
`solution.model_dump_json`), so this doubles as a service API contract. A rule
in JSON is ``{"name": "max_consecutive_days_cost", "weight": 2, "value": 6}``:
the spec's ``name``, its knobs (``weight`` / ``hard`` / ``exponent``) and its
domain parameters.

There is ONE class per constraint: the spec in `es_scheduling.constraints`.
The JSON rule models (`RULE_MODELS`, the `Rule` union) are generated from the
specs' fields and bounds, so they cannot drift from them;
`SchedulingInput.rules` accepts spec instances and JSON dicts alike, and
`rule.to_spec()` turns a validated rule back into its spec.

Times are integer minutes from day 0, 00:00 (see `es_scheduling.minutes`);
dates are day indices. Requires the optional `pydantic` dependency
(`pip install es-scheduling[models]`).

A solve can start from an earlier result: `with_initial_solution(request,
solution)` returns a new request whose `ShiftModel.worker` fields carry that
result, and construction then fills only the shifts it left out.
"""

import dataclasses
import os
import typing
from collections.abc import Callable, Mapping
from typing import Annotated, Any, ClassVar, Literal, Union

from pydantic import BaseModel, ConfigDict, Field, StrictBool, create_model, field_validator, model_validator

from . import _core
from . import constraints as _specs

__all__ = [
    "WorkerModel",
    "ShiftModel",
    "SolverSettings",
    "SchedulingInput",
    "ScheduleRequest",
    "ShiftAssignmentModel",
    "RuleTotal",
    "ScheduleSolution",
    "Rule",
    "RuleBase",
    "RULE_MODELS",
    "rule_model",
    "default_rules",
    "to_schedule",
    "solve_request",
    "with_initial_solution",
]


# ---------------------------------------------------------------------------
# Problem facts
# ---------------------------------------------------------------------------


class WorkerModel(BaseModel):
    """One person who can be assigned to shifts."""

    model_config = ConfigDict(extra="forbid")

    id: str
    skills: list[str] = Field(default_factory=list)
    unavailable_dates: list[int] = Field(default_factory=list, description="day indices")
    undesired_dates: list[int] = Field(default_factory=list, description="day indices")
    desired_dates: list[int] = Field(default_factory=list, description="day indices")
    desired_shifts: list[tuple[int, str]] = Field(
        default_factory=list,
        description="(day, location) location-specific work preferences (see the desired_shift_cost rule)",
    )


class ShiftModel(BaseModel):
    """One shift to be covered by exactly one worker."""

    model_config = ConfigDict(extra="forbid")

    id: str
    start: int = Field(ge=0, description="minutes from day 0 00:00; day 0 is the first day, history included")
    end: int = Field(description="minutes from day 0 00:00, after `start`")
    location: str = ""
    required_skill: str = ""
    worker: str | None = Field(
        default=None,
        description=(
            "Pre-assigned worker id. The solve starts from it instead of constructing this "
            "shift; local search may still move it. A starting point, not a pin."
        ),
    )
    pinned: bool = Field(
        default=False,
        description=(
            "Fix the assignment to `worker` (scored but never moved) — for history carried in "
            "from the previous planning period and for settled assignments; requires `worker`"
        ),
    )

    @model_validator(mode="after")
    def _ends_after_it_starts(self):
        if self.end <= self.start:
            raise ValueError(f"shift {self.id!r}: end must be after start, got {self.start} to {self.end}")
        return self

    @model_validator(mode="after")
    def _pinned_to_a_worker(self):
        if self.pinned and self.worker is None:
            raise ValueError(f"shift {self.id!r} is pinned but has no worker")
        return self


# ---------------------------------------------------------------------------
# Rules: a discriminated union GENERATED from the constraint specs
# ---------------------------------------------------------------------------
# One JSON model per spec class (es_scheduling.constraints), with the spec's
# own fields: the three knobs from RuleBase, the domain parameters (their
# bounds from the spec's `bounds`), and `name` as the discriminator. No second
# field list to drift.

_KNOBS = ("weight", "hard", "exponent")


_CATALOG: dict[str, Any] | None = None


def _catalog() -> dict[str, Any]:
    """{name: constraint_info() row}: the C++ defaults, reward and flippability."""
    global _CATALOG
    if _CATALOG is None:
        _CATALOG = {row.name: row for row in _core.constraint_info()}
    return _CATALOG


class RuleBase(BaseModel):
    """One rule as JSON: the spec's knobs and parameters; `to_spec()` is the spec.

    ``weight`` None = the component's default weight; ``hard`` None = its
    default level, True = hard, False = soft; ``exponent`` > 0 shapes the cost.
    A rule is also a valid pick for ``DecomposedCalculator`` (it carries
    ``name`` / ``weight`` / ``hard`` / ``exponent`` and ``params()``).
    """

    model_config = ConfigDict(extra="forbid", frozen=True)

    weight: float | None = Field(default=None, ge=0, description="None = the constraint's default weight")
    hard: StrictBool | None = Field(
        default=None, description="None = the default level; true = hard; false = soft (rewards refuse true)"
    )
    exponent: float = Field(default=1.0, gt=0, description="sign-preserving power shaping of the cost")

    spec_class: ClassVar[type]

    def to_spec(self) -> _specs.Constraint:
        """The constraint spec this rule states."""
        return self.spec_class(**{key: getattr(self, key) for key in type(self).model_fields if key != "name"})

    def params(self) -> dict[str, Any]:
        """The domain parameters (every field but name and the knobs), as the spec has them."""
        return self.to_spec().params()

    @model_validator(mode="after")
    def _spec_checks(self):
        # The spec's own checks (bounds) surface as a ValidationError, and so do
        # the level rules the C++ component declares (constraint_info()).
        spec = self.to_spec()
        row = _catalog()[spec.name]
        if spec.hard is True and row.reward:
            raise ValueError(f"constraint {spec.name!r} is a reward (raw cost <= 0) and cannot be hard")
        wanted = None if spec.hard is None else ("hard" if spec.hard else "soft")
        if wanted is not None and wanted != row.default_level and not row.level_flippable:
            raise ValueError(
                f"constraint {spec.name!r} is always {row.default_level}; hard={spec.hard!r} is not allowed"
            )
        return self


def _rule_model(spec_class: type) -> type[RuleBase]:
    hints = typing.get_type_hints(spec_class)
    params: dict[str, Any] = {}
    for spec_field in dataclasses.fields(spec_class):
        if spec_field.name in _KNOBS:
            continue
        annotation = hints[spec_field.name]
        if spec_field.name in spec_class.bounds:
            annotation = Annotated[annotation, Field(**spec_class.bounds[spec_field.name])]
        if spec_field.default_factory is not dataclasses.MISSING:
            params[spec_field.name] = (annotation, Field(default_factory=spec_field.default_factory))
        else:
            params[spec_field.name] = (annotation, spec_field.default)
    model = create_model(
        f"{spec_class.__name__}Rule",
        __base__=RuleBase,
        __module__=__name__,
        name=(Literal[spec_class.name], spec_class.name),
        **params,
    )
    model.spec_class = spec_class
    model.__doc__ = spec_class.__doc__
    return model


#: {constraint name: its JSON rule model}, in factory order (= CONSTRAINT_NAMES).
RULE_MODELS: dict[str, type[RuleBase]] = {spec.name: _rule_model(spec) for spec in _specs.SPEC_CLASSES}
# Each model is also a module attribute under its own name (<SpecClass>Rule,
# e.g. MaxConsecutiveDaysCostRule), so rules pickle (multi-start processes).
globals().update({model.__name__: model for model in RULE_MODELS.values()})

#: A rule as JSON: the union of every rule model, discriminated by ``name``.
Rule = Annotated[Union[tuple(RULE_MODELS.values())], Field(discriminator="name")]


def rule_model(constraint: "str | type[_specs.Constraint]") -> type[RuleBase]:
    """The JSON rule model of a constraint, by name or spec class."""
    name = constraint if isinstance(constraint, str) else constraint.name
    try:
        return RULE_MODELS[name]
    except KeyError:
        raise ValueError(f"unknown constraint {name!r}; available: {', '.join(RULE_MODELS)}") from None


def _as_rule_data(rule: Any) -> Any:
    """A spec instance -> its JSON form; anything else is left to pydantic."""
    if isinstance(rule, _specs.Constraint):
        return {"name": rule.name, **{knob: getattr(rule, knob) for knob in _KNOBS}, **rule.params()}
    return rule


def default_rules() -> list[RuleBase]:
    """The eight default rules at their default weights (equal to the built-in score)."""
    return [RULE_MODELS[spec.name]() for spec in _specs.get_constraints()]


# ---------------------------------------------------------------------------
# Request / response envelopes
# ---------------------------------------------------------------------------


# The SolverConfig attributes a portfolio entry may override: search knobs only.
# The time budget, the termination limits, the seed and the callback stay with
# the request, so a request cannot widen its own budget through a portfolio.
_PORTFOLIO_KEYS = frozenset(
    {
        "guided_moves",
        "employee_greedy_construction",
        "greedy_repair_budget_fraction",
        "late_acceptance_size",
        "ruin_recreate",
        "ruin_recreate_unimproved_step_trigger",
        "ruin_recreate_min_ruin_fraction",
        "ruin_recreate_max_ruin_fraction",
        "ruin_recreate_max_ruin_count",
        "ruin_recreate_operator_decay",
        "ruin_recreate_rewards",
        "ruin_recreate_polish_steps",
        "ruin_recreate_as_move_weight",
        "ruin_recreate_as_move_max_ruin",
        "descent_weight",
        "descent_max_sweeps",
        "change_move_weight",
        "swap_move_weight",
        "nearby_shifts",
        "nearby_shifts_probability",
        "smart_change_moves",
        "smart_change_candidate_count",
        "employee_day_repair_moves",
        "employee_run_repair_moves",
        "employee_run_repair_max_days",
        "employee_window_moves",
        "employee_window_max_days",
        "custom_move_probability",
        "employee_ruin_moves",
        "employee_unassign_moves",
    }
)


class SolverSettings(BaseModel):
    model_config = ConfigDict(extra="forbid")

    random_seed: int = 0
    guided_moves: bool = True
    employee_greedy_construction: bool = True
    # Share of the time budget reserved for the post-local-search greedy
    # hard-repair (local search gets the rest); total wall time still honors the
    # limit. Default 0 (off): local search already returns its best-so-far
    # near-feasible solution, and the extra pass does not improve on it here.
    greedy_repair_budget_fraction: float = Field(default=0.0, ge=0.0, lt=0.9)
    late_acceptance_size: int = Field(default=400, ge=1)
    stop_at_first_feasible: bool = False
    best_score_limit: tuple[int, float] | None = None
    no_improvement_limit_seconds: float | None = Field(default=None, ge=0)
    unimproved_spent_limit_seconds: float = Field(default=0.0, ge=0)
    step_count_limit: int = Field(default=0, ge=0)
    no_improvement_step_count_limit: int | None = Field(default=None, ge=0)
    unimproved_step_count_limit: int = Field(default=0, ge=0)

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
    ruin_recreate_as_move_weight: float = Field(default=0.0, ge=0)
    ruin_recreate_as_move_max_ruin: int = Field(default=12, ge=1)
    descent_weight: float = Field(default=0.02, ge=0)
    descent_max_sweeps: int = Field(default=4, ge=1)

    change_move_weight: float = Field(default=0.75, ge=0)
    swap_move_weight: float = Field(default=0.25, ge=0)
    # Temporal nearby selection: bias swap partners / descent pairs /
    # related-ruin clusters toward shifts starting close in time (where the
    # pairwise hard constraints interact). 0.65 measured best across roster
    # sizes; 0.8 wins bigger on ~1000+-shift rosters.
    nearby_shifts: bool = True
    nearby_shifts_probability: float = Field(default=0.65, ge=0, le=1)
    smart_change_moves: bool = False
    smart_change_candidate_count: int = Field(default=6, ge=1)
    employee_day_repair_moves: bool = True
    employee_run_repair_moves: bool = True
    employee_run_repair_max_days: int = Field(default=3, ge=1)
    employee_window_moves: bool = True
    employee_window_max_days: int = Field(default=3, ge=1)
    custom_move_probability: float = Field(default=0.30, ge=0, le=1)
    employee_ruin_moves: bool = True
    employee_unassign_moves: bool = False

    # Multi-start: run this many independent solves (seed = random_seed + i) in
    # parallel processes and keep the best. The metaheuristic has real
    # run-to-run variance, so best-of-n is a cheap, reliable quality lift.
    # 1 (default) = a single solve. `ScheduleRequest.num_workers` asks for the
    # same thing; the larger of the two applies.
    multi_start: int = Field(default=1, ge=1)

    # Portfolio multi-start: per-start SolverConfig overrides, cycled across the
    # n starts (start i uses portfolio[i % len]). Each dict maps a search knob of
    # SolverConfig (see _PORTFOLIO_KEYS) to a value, applied for that start only;
    # any other key is rejected here. Diversifying the search
    # (e.g. late_acceptance_size, ruin_recreate_max_ruin_fraction) makes the
    # starts explore complementary basins instead of n seeds of one config, which
    # -- paired with a true-objective selector -- lifts the hardest instances at
    # no cost to the easy ones. Empty (default) => every start identical.
    portfolio: list[dict[str, Any]] = Field(default_factory=list)

    # Multi-start SELECTION correction. When a caller inflates a constraint's
    # weight for the search (e.g. multiplies the shift_coverage_cost weight to
    # prevent under-fill), the reported soft score no longer tracks the true
    # objective, so picking the best-of-n by that score picks the wrong winner.
    # This divisor un-inflates the shift_coverage_cost soft cost when ranking candidates
    # (selection only; the solved scores are untouched), so best-of-n is chosen
    # on the faithful objective. 1.0 = no correction (default); set it to the
    # multiplier used.
    select_coverage_divisor: float = Field(default=1.0, gt=0)

    relocate_move_weight: float = Field(default=0.5, ge=0)
    list_swap_move_weight: float = Field(default=0.3, ge=0)
    two_opt_move_weight: float = Field(default=0.2, ge=0)
    or_opt_move_weight: float = Field(default=0.0, ge=0)
    tail_swap_move_weight: float = Field(default=0.0, ge=0)

    @field_validator("portfolio")
    @classmethod
    def _valid_portfolio_keys(cls, portfolio):
        for index, overrides in enumerate(portfolio):
            unknown = set(overrides) - _PORTFOLIO_KEYS
            if unknown:
                raise ValueError(f"portfolio[{index}]: keys cannot be overridden per start: {sorted(unknown)}")
        return portfolio

    @model_validator(mode="after")
    def _valid_ruin_fraction_range(self):
        if self.ruin_recreate_min_ruin_fraction > self.ruin_recreate_max_ruin_fraction:
            raise ValueError("ruin_recreate_min_ruin_fraction must be <= ruin_recreate_max_ruin_fraction")
        return self

    @model_validator(mode="after")
    def _valid_no_improvement_aliases(self):
        if (
            self.no_improvement_limit_seconds is not None
            and self.unimproved_spent_limit_seconds != 0.0
            and abs(self.no_improvement_limit_seconds - self.unimproved_spent_limit_seconds) > 1e-9
        ):
            raise ValueError(
                "no_improvement_limit_seconds and unimproved_spent_limit_seconds must match when both are set"
            )
        if (
            self.no_improvement_step_count_limit is not None
            and self.unimproved_step_count_limit != 0
            and self.no_improvement_step_count_limit != self.unimproved_step_count_limit
        ):
            raise ValueError(
                "no_improvement_step_count_limit and unimproved_step_count_limit must match when both are set"
            )
        return self


class SchedulingInput(BaseModel):
    model_config = ConfigDict(extra="forbid")

    workers: list[WorkerModel]
    shifts: list[ShiftModel]
    # Constraint specs or their JSON form; a rule may repeat (other parameters).
    rules: list[Rule] = Field(default_factory=default_rules)

    @field_validator("rules", mode="before")
    @classmethod
    def _specs_as_rules(cls, rules):
        if isinstance(rules, (list, tuple)):
            return [_as_rule_data(rule) for rule in rules]
        return rules

    @model_validator(mode="after")
    def _unique_ids(self):
        for kind, items in (("worker", self.workers), ("shift", self.shifts)):
            ids = [x.id for x in items]
            if len(ids) != len(set(ids)):
                raise ValueError(f"duplicate {kind} ids")
        return self

    @model_validator(mode="after")
    def _known_workers(self):
        workers = {worker.id for worker in self.workers}
        for shift in self.shifts:
            if shift.worker is not None and shift.worker not in workers:
                raise ValueError(f"shift {shift.id!r} pre-assigned to unknown worker {shift.worker!r}")
        return self


class ScheduleRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str | None = None
    timelimit: float = Field(default=30.0, ge=0, description="seconds")
    num_workers: int = Field(
        default=1,
        ge=1,
        description=(
            "Independent parallel starts, like `settings.multi_start`: the larger of the two "
            "applies, they do not multiply. Each start gets the full `timelimit`."
        ),
    )
    model_input: SchedulingInput
    settings: SolverSettings = Field(default_factory=SolverSettings)


class ShiftAssignmentModel(BaseModel):
    shift_id: str
    start: int
    end: int
    location: str = ""
    required_skill: str = ""


class RuleTotal(BaseModel):
    """Weighted cost of one rule instance in the final schedule (positive =
    penalizing, negative = rewarding) — the score explanation. ``name`` is the
    constraint name (``"shift_coverage_cost"``); ``label`` tells the instances
    of a parametrized rule apart (``"Shift coverage[Skill A 1..inf]"``; empty
    for a rule without one)."""

    name: str
    label: str = ""
    cost: float
    level: Literal["hard", "medium", "soft"] = "soft"
    is_hard: bool


class ScheduleSolution(BaseModel):
    schedule: dict[str, list[ShiftAssignmentModel]]  # worker id -> assignments
    unassigned: list[str] = Field(default_factory=list)  # shift ids
    score_hard: int
    score_soft: float
    feasible: bool
    rule_totals: list[RuleTotal] = Field(default_factory=list)


# ---------------------------------------------------------------------------
# Bridging + the one-call entry point
# ---------------------------------------------------------------------------


def to_schedule(model_input: SchedulingInput) -> "_core.EmployeeSchedule":
    """Build the C++ EmployeeSchedule from the pydantic input (pre-assigned
    `shift.worker` ids become the starting assignment)."""
    workers = []
    for w in model_input.workers:
        emp = _core.Employee(
            w.id,
            set(w.skills),
            set(w.unavailable_dates),
            set(w.undesired_dates),
            set(w.desired_dates),
        )
        if w.desired_shifts:
            emp.desired_shifts = {(int(d), str(loc)) for d, loc in w.desired_shifts}
        workers.append(emp)
    index_of = {w.id: i for i, w in enumerate(model_input.workers)}
    shifts = []
    for s in model_input.shifts:
        shift = _core.Shift(s.id, s.start, s.end, s.location, s.required_skill)
        if s.worker is not None:
            if s.worker not in index_of:
                raise ValueError(f"shift {s.id!r} pre-assigned to unknown worker {s.worker!r}")
            shift.employee_index = index_of[s.worker]
        if s.pinned:
            if s.worker is None:
                raise ValueError(f"shift {s.id!r} is pinned but has no worker")
            shift.pinned = True
        shifts.append(shift)
    return _core.EmployeeSchedule(workers, shifts)


def _read_solution(solution: "ScheduleSolution | Mapping[str, Any]", model_input: SchedulingInput):
    """Check a solution against the problem's ids; returns ({shift id: worker id},
    the shift ids it lists as unassigned)."""
    data = solution.model_dump(mode="json") if isinstance(solution, ScheduleSolution) else solution
    if not isinstance(data, Mapping) or not isinstance(data.get("schedule"), Mapping):
        raise ValueError("solution must be a ScheduleSolution or a dict with a 'schedule' mapping")
    unassigned = data.get("unassigned", [])
    if not isinstance(unassigned, list):
        raise ValueError(f"solution: 'unassigned' must be a list of shift ids, got {unassigned!r}")

    worker_ids = {worker.id for worker in model_input.workers}
    shift_ids = {shift.id for shift in model_input.shifts}
    assigned: dict[str, str] = {}
    for worker, entries in data["schedule"].items():
        if worker not in worker_ids:
            raise ValueError(f"solution: unknown worker {worker!r}")
        if not isinstance(entries, list):
            raise ValueError(f"worker {worker!r}: assignments must be a list, got {entries!r}")
        for entry in entries:
            shift_id = entry.get("shift_id") if isinstance(entry, Mapping) else None
            if not isinstance(shift_id, str):
                raise ValueError(f"worker {worker!r}: an assignment needs a shift_id, got {entry!r}")
            if shift_id not in shift_ids:
                raise ValueError(f"worker {worker!r}: unknown shift {shift_id!r}")
            if shift_id in assigned:
                raise ValueError(f"shift {shift_id!r} is assigned twice: {assigned[shift_id]!r} and {worker!r}")
            assigned[shift_id] = worker
    for shift_id in unassigned:
        if not isinstance(shift_id, str) or shift_id not in shift_ids:
            raise ValueError(f"solution: unknown unassigned shift {shift_id!r}")
        if shift_id in assigned:
            raise ValueError(f"shift {shift_id!r} is both assigned to {assigned[shift_id]!r} and unassigned")
    return assigned, set(unassigned)


def with_initial_solution(
    problem: ScheduleRequest,
    solution: "ScheduleSolution | Mapping[str, Any]",
) -> ScheduleRequest:
    """A new request whose solve starts from `solution`; `problem` is not changed.

    `solution` is what `solve_request` returns: a `ScheduleSolution` or the plain
    dict of `model_dump(mode="json")`. Workers and shifts are matched by id, never
    by position, and an assignment is read for its `shift_id` only.

    Each shift's `ShiftModel.worker` becomes what the solution says, so the solve
    starts there instead of constructing it. A partial solution is fine: a shift
    it leaves out, or lists as `unassigned`, loses any earlier `worker` and is
    placed by normal construction, and nothing else is. A complete solution leaves
    construction nothing to do. Local search may still move a shift afterwards;
    use `pinned` to fix one.

    A pinned shift keeps its worker, whether the solution mentions it or not.

    Raises ValueError, naming the worker or shift, for an unknown id, a shift
    assigned twice or both assigned and unassigned, a malformed assignment, or a
    pinned shift the solution moves away from its worker or unassigns.
    """
    model_input = problem.model_input
    assigned, unassigned = _read_solution(solution, model_input)
    for shift in model_input.shifts:
        if shift.pinned and (shift.id in unassigned or assigned.get(shift.id, shift.worker) != shift.worker):
            raise ValueError(f"shift {shift.id!r} is pinned to {shift.worker!r}; the solution cannot change it")

    seeded = problem.model_copy(deep=True)
    for shift in seeded.model_input.shifts:
        if not shift.pinned:
            shift.worker = assigned.get(shift.id)
    return seeded


def _build_and_solve(request: ScheduleRequest, seed: int, overrides: dict | None = None) -> ScheduleSolution:
    """One solve at a fixed random seed — the building block for multi-start.

    ``overrides`` sets extra SolverConfig attributes for THIS start only (used by
    portfolio multi-start to diversify per-start search behaviour, e.g. a
    different late_acceptance_size / ruin fraction). Applied last, so it wins."""
    model_input = request.model_input
    schedule = to_schedule(model_input)
    calc = _core.DecomposedCalculator([rule.to_spec() for rule in model_input.rules])

    cfg = _core.SolverConfig()
    cfg.spent_limit_seconds = request.timelimit
    cfg.random_seed = seed
    cfg.guided_moves = request.settings.guided_moves
    cfg.employee_greedy_construction = request.settings.employee_greedy_construction
    cfg.greedy_repair_budget_fraction = request.settings.greedy_repair_budget_fraction
    cfg.late_acceptance_size = request.settings.late_acceptance_size
    cfg.no_improvement_limit_seconds = (
        request.settings.no_improvement_limit_seconds
        if request.settings.no_improvement_limit_seconds is not None
        else request.settings.unimproved_spent_limit_seconds
    )
    cfg.step_count_limit = request.settings.step_count_limit
    cfg.no_improvement_step_count_limit = (
        request.settings.no_improvement_step_count_limit
        if request.settings.no_improvement_step_count_limit is not None
        else request.settings.unimproved_step_count_limit
    )
    cfg.ruin_recreate = request.settings.ruin_recreate
    cfg.ruin_recreate_unimproved_step_trigger = request.settings.ruin_recreate_unimproved_step_trigger
    cfg.ruin_recreate_min_ruin_fraction = request.settings.ruin_recreate_min_ruin_fraction
    cfg.ruin_recreate_max_ruin_fraction = request.settings.ruin_recreate_max_ruin_fraction
    cfg.ruin_recreate_max_ruin_count = request.settings.ruin_recreate_max_ruin_count
    cfg.ruin_recreate_operator_decay = request.settings.ruin_recreate_operator_decay
    cfg.ruin_recreate_rewards = request.settings.ruin_recreate_rewards
    cfg.ruin_recreate_polish_steps = request.settings.ruin_recreate_polish_steps
    cfg.ruin_recreate_as_move_weight = request.settings.ruin_recreate_as_move_weight
    cfg.ruin_recreate_as_move_max_ruin = request.settings.ruin_recreate_as_move_max_ruin
    cfg.descent_weight = request.settings.descent_weight
    cfg.descent_max_sweeps = request.settings.descent_max_sweeps
    cfg.change_move_weight = request.settings.change_move_weight
    cfg.swap_move_weight = request.settings.swap_move_weight
    cfg.nearby_shifts = request.settings.nearby_shifts
    cfg.nearby_shifts_probability = request.settings.nearby_shifts_probability
    cfg.smart_change_moves = request.settings.smart_change_moves
    cfg.smart_change_candidate_count = request.settings.smart_change_candidate_count
    cfg.employee_day_repair_moves = request.settings.employee_day_repair_moves
    cfg.employee_run_repair_moves = request.settings.employee_run_repair_moves
    cfg.employee_run_repair_max_days = request.settings.employee_run_repair_max_days
    cfg.employee_window_moves = request.settings.employee_window_moves
    cfg.employee_window_max_days = request.settings.employee_window_max_days
    cfg.custom_move_probability = request.settings.custom_move_probability
    cfg.employee_ruin_moves = request.settings.employee_ruin_moves
    cfg.employee_unassign_moves = request.settings.employee_unassign_moves
    cfg.relocate_move_weight = request.settings.relocate_move_weight
    cfg.list_swap_move_weight = request.settings.list_swap_move_weight
    cfg.two_opt_move_weight = request.settings.two_opt_move_weight
    cfg.or_opt_move_weight = request.settings.or_opt_move_weight
    cfg.tail_swap_move_weight = request.settings.tail_swap_move_weight
    if request.settings.stop_at_first_feasible:
        cfg.best_score_limit = (0, -1e18)
    if request.settings.best_score_limit is not None:
        cfg.best_score_limit = request.settings.best_score_limit
    for key, value in (overrides or {}).items():
        setattr(cfg, key, value)  # per-start portfolio diversification
    _core.Solver(cfg).solve(schedule, calc)

    per_worker: dict[str, list[ShiftAssignmentModel]] = {w.id: [] for w in model_input.workers}
    unassigned: list[str] = []
    for spec, e in zip(model_input.shifts, schedule.assignment()):
        if e < 0:
            unassigned.append(spec.id)
            continue
        per_worker[model_input.workers[e].id].append(
            ShiftAssignmentModel(
                shift_id=spec.id,
                start=spec.start,
                end=spec.end,
                location=spec.location,
                required_skill=spec.required_skill,
            )
        )
    for assignments in per_worker.values():
        assignments.sort(key=lambda a: a.start)

    score = schedule.score
    return ScheduleSolution(
        schedule=per_worker,
        unassigned=unassigned,
        score_hard=score.hard,
        score_soft=score.soft,
        feasible=score.is_feasible(),
        rule_totals=[RuleTotal(**t) for t in calc.constraint_totals()],
    )


def _ms_worker(args):
    request, seed, *rest = args
    overrides = rest[0] if rest else None
    return _build_and_solve(request, seed, overrides)


def _solve_in_processes(jobs: list, max_workers: int) -> list[ScheduleSolution] | None:
    """Run the starts in forked processes; None if processes cannot be used here.

    Only that one case returns None. An error raised by a start itself, such as a
    bad configuration, comes straight back to the caller: rerunning it would fail
    the same way after spending the time budget again."""
    try:
        import multiprocessing as mp
        from concurrent.futures import ProcessPoolExecutor
        from concurrent.futures.process import BrokenProcessPool

        # A daemonic process (a multiprocessing.Pool worker) may not have children.
        if mp.current_process().daemon:
            return None
        # 'fork' avoids re-running the caller's __main__ (which 'spawn' does,
        # breaking calls from a REPL/stdin/notebook) and gives true parallelism.
        # ValueError: no fork on this platform (e.g. Windows). The others: no
        # working semaphores to build the pool's queues with.
        pool = ProcessPoolExecutor(max_workers=max_workers, mp_context=mp.get_context("fork"))
    except (ValueError, OSError, NotImplementedError, ImportError):
        return None

    with pool:
        try:
            futures = [pool.submit(_ms_worker, job) for job in jobs]
        except (OSError, BrokenProcessPool):  # fork itself failed
            return None
        try:
            return [future.result() for future in futures]
        except BrokenProcessPool:  # a worker process died
            return None


def solve_request(
    request: ScheduleRequest,
    *,
    select: Callable[[list[ScheduleSolution]], ScheduleSolution] | None = None,
) -> ScheduleSolution:
    """Solve a pydantic request end-to-end and return the pydantic solution.

    ``request.num_workers`` or ``settings.multi_start`` > 1 runs that many
    independent solves (the larger of the two; seeds ``random_seed + i``) in
    parallel processes and returns the best-scoring one. Each start gets the
    full ``timelimit`` and at most one start per core runs at a time. The
    metaheuristic has real run-to-run variance, so best-of-n is a cheap,
    reliable quality lift (and uses the extra cores). 1 (default) is a single
    solve.

    Parallel starts are forked. Forking a process that runs other threads is
    unsafe: the child gets a copy of any lock another thread held and can hang
    on it, Python 3.12+ warns with a DeprecationWarning, and macOS system
    libraries may abort in the child. From a threaded host, call this in a
    subprocess of its own, or stay at one start. A single start does not fork,
    and releases the GIL while it solves.

    Where processes cannot be used (no fork, a daemonic process) or a worker
    dies, the starts run one after another, so the wall time can reach n times
    ``timelimit``. An error raised by a start is not retried.

    ``select`` lets the caller pick the best-of-n candidate itself (given the
    full list of solutions) instead of the built-in faithful-score key -- e.g.
    to rank candidates on an external objective the internal score only
    approximates. Ignored for a single start."""
    n = max(request.num_workers, request.settings.multi_start)
    if n <= 1:
        return _build_and_solve(request, request.settings.random_seed)

    # Portfolio multi-start: start i uses seed random_seed+i AND per-start config
    # overrides portfolio[i % len] (diversifying late_acceptance_size / ruin
    # aggressiveness so the n starts explore complementary basins rather than
    # n seeds of one identical config). Empty portfolio => all identical.
    port = request.settings.portfolio
    jobs = [(request, request.settings.random_seed + i, port[i % len(port)] if port else {}) for i in range(n)]
    solutions = _solve_in_processes(jobs, min(n, os.cpu_count() or 1))
    if solutions is None:
        # Sequential best-of-n, so the feature always works, just without the
        # parallel speedup.
        solutions = [_build_and_solve(r, s, o) for r, s, o in jobs]
    if select is not None:
        return select(solutions)

    # Higher score is better: hard first (<= 0, closer to 0), then FAITHFUL soft.
    # The faithful soft un-inflates the search-boosted shift_coverage_cost
    # (divide that cost by the divisor) so the winner is the best on the true
    # objective, not on the boosted internal score. div = 1 => plain score_soft.
    div = request.settings.select_coverage_divisor

    def _faithful_key(s: ScheduleSolution) -> tuple[int, float]:
        if div == 1.0:
            return (s.score_hard, s.score_soft)
        cov = sum(t.cost for t in s.rule_totals if t.is_hard is False and t.name == "shift_coverage_cost")
        # rule_totals cost is positive for penalties; score_soft is its negative.
        # Removing the boost inflation (cov -> cov/div) lifts the soft score by
        # the inflated excess cov*(1 - 1/div).
        return (s.score_hard, s.score_soft + cov * (1.0 - 1.0 / div))

    return max(solutions, key=_faithful_key)
