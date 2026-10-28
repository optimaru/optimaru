"""Pydantic v2 mirrors of the engine domain — for JSON serialization.

Mirrors `_core`'s Timeslot/Room/Lesson/Timetable and SolverConfig 1:1.
The canonical `TimetableProblem` JSON is handled by `convert.py`; these models
store or send the engine's own shape (debug dumps, regression snapshots, ...)::

    from st_timetabling.models import TimetableModel, SolveConfig

    model = TimetableModel.from_core(table)     # engine -> pydantic
    clone = TimetableModel.model_validate_json(model.model_dump_json())
    table2 = clone.to_core()                    # pydantic -> engine

`pydantic` is an optional dependency (`pip install st-timetabling[models]`).
"""

from typing import Any

from pydantic import BaseModel, ConfigDict, Field, create_model, model_validator

from . import _core
from .constraints import soft_specs

__all__ = [
    "LessonModel",
    "ObjectiveWeightsModel",
    "RoomModel",
    "SolveConfig",
    "TimeslotModel",
    "TimetableModel",
]


class TimeslotModel(BaseModel):
    """One lesson cell: (weekday, period). day 0 = Monday, period is 1-based."""

    model_config = ConfigDict(extra="forbid")

    day: int = Field(ge=0)
    period: int = Field(ge=1)


class RoomModel(BaseModel):
    """A room. type is informational (regular/science lab/...) — not a hard constraint in the engine."""

    model_config = ConfigDict(extra="forbid")

    name: str
    type: str = ""


class LessonModel(BaseModel):
    """Planning entity. slot_room is the timeslot*room composite variable (-1 = unassigned)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    subject: str
    teacher: str
    student_group: str
    slot_room: int = -1
    pinned: bool = False


class TimetableModel(BaseModel):
    """Planning-solution mirror. The score is a read-only result and has its own field."""

    model_config = ConfigDict(extra="forbid")

    timeslots: list[TimeslotModel]
    rooms: list[RoomModel]
    lessons: list[LessonModel]
    teacher_unavailable: dict[str, list[int]] = Field(default_factory=dict)
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.Timetable":
        """pydantic -> engine objects (the score field is ignored; the engine recomputes it)."""
        return _core.Timetable(
            [_core.Timeslot(t.day, t.period) for t in self.timeslots],
            [_core.Room(r.name, r.type) for r in self.rooms],
            [self._lesson_to_core(l) for l in self.lessons],
            {name: set(slots) for name, slots in self.teacher_unavailable.items()},
        )

    @staticmethod
    def _lesson_to_core(l: LessonModel) -> "_core.Lesson":
        lesson = _core.Lesson(l.id, l.subject, l.teacher, l.student_group)
        lesson.slot_room = l.slot_room
        lesson.pinned = l.pinned
        return lesson

    @classmethod
    def from_core(cls, table: "_core.Timetable") -> "TimetableModel":
        """engine objects -> pydantic (with the current assignment and score)."""
        return cls(
            timeslots=[TimeslotModel(day=t.day, period=t.period) for t in table.timeslots],
            rooms=[RoomModel(name=r.name, type=r.type) for r in table.rooms],
            lessons=[
                LessonModel(
                    id=l.id,
                    subject=l.subject,
                    teacher=l.teacher,
                    student_group=l.student_group,
                    slot_room=l.slot_room,
                    pinned=l.pinned,
                )
                for l in table.lessons
            ],
            teacher_unavailable={name: sorted(slots) for name, slots in table.teacher_unavailable.items()},
            score_hard=table.score.hard,
            score_soft=table.score.soft,
        )


class _ObjectiveWeightsBase(BaseModel):
    """Body of `ObjectiveWeightsModel`. The fields are generated from the constraint registry."""

    model_config = ConfigDict(extra="forbid")

    def to_core(self) -> "_core.canon.ObjectiveWeights":
        # The monolithic fast path's ObjectiveWeights is a fixed struct, so only the
        # terms it knows are passed. The assembly path skips this mirror and uses
        # the components' weights directly, so a soft term only in the registry still works there.
        known = set(_core.canon.ObjectiveWeights().__dir__())
        return _core.canon.ObjectiveWeights(**{k: v for k, v in self.model_dump().items() if k in known})


#: Canonical soft-weight mirror. **The fields are not written by hand** — names and
#: defaults come straight from the soft terms of the constraint registry (plugin
#: style: the objective is the sum of the registered constraints, not a separately
#: maintained list). Adding a soft constraint creates its weight field automatically,
#: so the two lists have nowhere to drift apart.
_objective_weight_fields: dict[str, Any | tuple[Any, Any]] = {
    spec.name: (
        int,
        Field(default=int(spec.default_weight), ge=0, description=(spec.__doc__ or "").strip().split("\n")[0]),
    )
    for spec in soft_specs()
}

_create_model: Any = create_model
ObjectiveWeightsModel = _create_model(
    "ObjectiveWeightsModel",
    __base__=_ObjectiveWeightsBase,
    __doc__=(
        "Canonical soft-weight mirror (`_core.canon.ObjectiveWeights`). The fields are "
        "generated from the soft constraints of `st_timetabling.constraints`."
    ),
    **_objective_weight_fields,
)


class SolveConfig(BaseModel):
    """Serializable mirror of `_core.SolverConfig` / `_core.canon.SolverConfig`.

    The common engine fields are the same in both modes; ``weights`` applies to
    canonical mode only (`to_canon()`) — the quickstart config (`to_core()`)
    has no such field."""

    model_config = ConfigDict(extra="forbid")

    time_limit_seconds: float = Field(default=30.0, ge=0)
    random_seed: int = 0
    parallel_solver_count: int = Field(default=1, ge=1)
    # 0 = auto (canonical: clamp(4 * lesson count, 400, 3200), quickstart:
    # clamp(8 * lesson count, 400, 3200)). A positive value is used as is.
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

    weights: ObjectiveWeightsModel = Field(default_factory=ObjectiveWeightsModel)

    @model_validator(mode="after")
    def _valid_ruin_fraction_range(self):
        if self.ruin_recreate_min_ruin_fraction > self.ruin_recreate_max_ruin_fraction:
            raise ValueError("ruin_recreate_min_ruin_fraction must be <= ruin_recreate_max_ruin_fraction")
        return self

    def to_canon(self) -> "_core.canon.SolverConfig":
        """Canonical-mode config (with weights)."""
        cfg = self._apply_common(_core.canon.SolverConfig())
        cfg.weights = self.weights.to_core()
        return cfg

    def to_core(self) -> "_core.SolverConfig":
        """Quickstart-mode config (weights do not exist in the quickstart domain and are ignored)."""
        return self._apply_common(_core.SolverConfig())

    def _apply_common(self, cfg):
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
            cfg.best_score_limit = (0, -1e18)
        if self.best_score_limit is not None:
            cfg.best_score_limit = self.best_score_limit
        return cfg
