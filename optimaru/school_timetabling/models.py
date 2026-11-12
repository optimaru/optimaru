"""Pydantic v2 mirrors of the engine domain — for JSON serialization.

Mirrors `_core`'s Timeslot/Room/Lesson/Timetable and SolverConfig 1:1.
The canonical `TimetableProblem` JSON is handled by `convert.py`; these models
store or send the engine's own shape (debug dumps, regression snapshots, ...)::

    from optimaru.school_timetabling.models import TimetableModel, SolveConfig

    model = TimetableModel.from_core(table)     # engine -> pydantic
    clone = TimetableModel.model_validate_json(model.model_dump_json())
    table2 = clone.to_core()                    # pydantic -> engine
"""

from typing import Any, Literal

from pydantic import Field, create_model, model_validator

from .._models import SolveConfigBase, SolveRequest, StrictModel, apply_solver_parameters
from . import _core, convert
from .constraints import default_weights, soft_specs

__all__ = [
    "CalendarModel",
    "ClassGroupModel",
    "ClassroomModel",
    "CourseModel",
    "LessonModel",
    "ObjectiveWeightsModel",
    "RoomModel",
    "ScheduledLessonModel",
    "SlotModel",
    "SolveConfig",
    "SubjectModel",
    "TeacherModel",
    "TimeslotModel",
    "TimetableModel",
    "TimetableProblemModel",
    "TimetableRequest",
    "TimetableResultModel",
    "solve_request",
]


class TimeslotModel(StrictModel):
    """One lesson cell: (weekday, period). day 0 = Monday, period is 1-based."""

    day: int = Field(ge=0)
    period: int = Field(ge=1)


class RoomModel(StrictModel):
    """A room. type is informational (regular/science lab/...) — not a hard constraint in the engine."""

    name: str
    type: str = ""


class LessonModel(StrictModel):
    """Slot_room is the timeslot*room composite variable (-1 = unassigned)."""

    id: str
    subject: str
    teacher: str
    student_group: str
    slot_room: int = -1
    pinned: bool = False


class TimetableModel(StrictModel):
    """Solution mirror. The score is a read-only result and has its own field."""

    timeslots: list[TimeslotModel]
    rooms: list[RoomModel]
    lessons: list[LessonModel]
    teacher_unavailable: dict[str, list[int]] = Field(default_factory=dict)
    score_hard: int | None = None
    score_soft: float | None = None

    @model_validator(mode="after")
    def _check_indices(self):
        """`slot_room` is a raw value the engine reads without a range check. The slots in
        `teacher_unavailable` it skips when out of range."""
        value_count = len(self.timeslots) * len(self.rooms)
        for lesson in self.lessons:
            if not -1 <= lesson.slot_room < value_count:
                raise ValueError(
                    f"lesson {lesson.id!r}: slot_room {lesson.slot_room!r} is out of range -1..{value_count - 1}"
                )
        return self

    def to_core(self) -> "_core.Timetable":
        """pydantic -> engine objects (the score field is ignored; the engine recomputes it)."""
        self._check_indices()  # the fields stay mutable after validation
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


class _ObjectiveWeightsBase(StrictModel):
    """Body of `ObjectiveWeightsModel`. The fields are generated from the constraint catalog."""

    def to_core(self) -> "_core.canon.ObjectiveWeights":
        # The monolithic fast path's ObjectiveWeights has a field for the soft terms it
        # scores. A term only in the catalog (not on the default path yet) is dropped at its
        # default weight and refused at any other, the same rule as solve(weights=); the
        # plugin path skips this mirror and weighs such a term at its pick's weight.
        return convert._fast_path_weights(self.model_dump())


#: Soft-weight mirror of the fast path. **The fields are not written by hand** —
#: names and defaults come from the soft rows of `constraint_info()` (the C++
#: components' kDefaultLevel / kDefaultWeight), descriptions from the specs. A
#: new soft component brings its weight field along, so the lists have nowhere
#: to drift apart.
_objective_weight_fields: dict[str, Any | tuple[Any, Any]] = {
    spec.name: (
        float,
        Field(default=default_weights()[spec.name], ge=0, allow_inf_nan=False, description=spec.label_en),
    )
    for spec in soft_specs()
}

_create_model: Any = create_model
ObjectiveWeightsModel = _create_model(
    "ObjectiveWeightsModel",
    __base__=_ObjectiveWeightsBase,
    __doc__=(
        "Soft-weight mirror of the fast path (`_core.canon.ObjectiveWeights`), keyed by the soft "
        "constraint names. The fields are generated from the soft rows of `constraint_info()`."
    ),
    **_objective_weight_fields,
)


class SolveConfig(SolveConfigBase):
    """Serializable mirror of `_core.SolverConfig` / `_core.canon.SolverConfig`.

    The common engine fields are the same in both modes; ``weights`` (the fast
    path's soft weights, keyed by the soft constraint names) applies to
    canonical mode only (`to_canon()`) — the quickstart config (`to_core()`)
    has no such field.

    ``late_acceptance_size`` 0 is automatic: ``clamp(4 * lesson count, 400, 3200)`` in
    canonical mode and ``clamp(8 * lesson count, 400, 3200)`` in quickstart mode. A positive
    value is used as it is.
    """

    weights: ObjectiveWeightsModel = Field(default_factory=ObjectiveWeightsModel)

    def to_canon(self) -> "_core.canon.SolverConfig":
        """Canonical-mode config (with the fast path's soft weights)."""
        cfg = self._apply_to(_core.canon.SolverConfig())
        cfg.weights = self.weights.to_core()
        return cfg

    def to_core(self) -> "_core.SolverConfig":
        """Quickstart-mode config (weights do not exist in the quickstart domain and are ignored)."""
        return self._apply_to(_core.SolverConfig())


# ---------------------------------------------------------------------------
# The canonical problem: what `solve()` takes by default (mode="canonical")
# ---------------------------------------------------------------------------
# The pydantic form of the `TimetableProblem` dict that `convert_problem` reads, and of the
# `Timetable` dict `solve()` returns for it. They check field bounds only: references and
# the calendar limits are checked by `convert_problem`, as for a dict.


class SlotModel(StrictModel):
    """One lesson cell: a weekday (0 = Monday) and a 1-based period."""

    day: int = Field(ge=0, le=7)
    period: int = Field(ge=1)


class CalendarModel(StrictModel):
    """Periods per weekday (0 = Monday); days may differ in length."""

    periods_per_day: dict[int, int] = Field(min_length=1)


class SubjectModel(StrictModel):
    id: str
    name: str
    required_room_type: str | None = Field(
        default=None, description="The special room type it needs; None is the class's own room."
    )
    preferred_periods: list[int] = Field(
        default_factory=list, description="Periods it prefers; empty is no preference."
    )
    avoided_periods: list[int] = Field(default_factory=list, description="Periods it avoids.")


class TeacherModel(StrictModel):
    id: str
    name: str
    subject_ids: list[str] = Field(default_factory=list, description="Subjects the teacher may teach; empty is any.")
    unavailable: list[SlotModel] = Field(default_factory=list, description="Slots the teacher cannot teach in.")
    max_periods_per_week: int | None = Field(default=None, ge=0)
    max_periods_per_day: int = Field(default=6, ge=1)
    max_consecutive_periods: int = Field(default=4, ge=1, description="Most periods in a row without a break.")
    preferred_free_days: list[int] = Field(default_factory=list, description="Weekdays the teacher would like free.")
    homeroom_class_id: str | None = Field(
        default=None, description="The class group the teacher is homeroom teacher of."
    )


class ClassGroupModel(StrictModel):
    id: str
    name: str
    grade: int = Field(ge=1)
    unavailable: list[SlotModel] = Field(default_factory=list, description="Slots the class cannot have lessons in.")


class ClassroomModel(StrictModel):
    """A special room. `room_type` is what a subject's `required_room_type` names."""

    id: str
    name: str
    room_type: str


class CourseModel(StrictModel):
    """One unit to place: a class group, a subject, its teachers and the periods per week."""

    id: str
    class_group_id: str
    subject_id: str
    teacher_ids: list[str] = Field(min_length=1, description="Two or more teach together.")
    periods_per_week: int = Field(ge=1)
    block_periods: int = Field(default=0, ge=0, description="Periods to place in a row; 0 or 1 is no block.")
    max_periods_per_day: int = Field(default=1, ge=1, description="Most periods of this course on one day.")
    fixed_slots: list[SlotModel] = Field(default_factory=list, description="Slots the course must be in.")
    forbidden_slots: list[SlotModel] = Field(default_factory=list, description="Slots the course may not be in.")
    parallel_group: str | None = Field(
        default=None, description="Courses that share this value run at the same times (a class split into sections)."
    )


class TimetableProblemModel(StrictModel):
    """The timetabling problem `solve()` takes: the pydantic form of the `TimetableProblem` dict."""

    calendar: CalendarModel
    subjects: list[SubjectModel]
    teachers: list[TeacherModel]
    class_groups: list[ClassGroupModel]
    rooms: list[ClassroomModel] = Field(default_factory=list)
    courses: list[CourseModel]


class ScheduledLessonModel(StrictModel):
    """One placed period of a course."""

    course_id: str
    class_group_id: str
    subject_id: str
    teacher_ids: list[str]
    slot: SlotModel
    room_id: str | None = None


class TimetableResultModel(StrictModel):
    """What `solve()` returns for a `TimetableProblemModel`: the pydantic form of the `Timetable` dict."""

    status: str = Field(description="FEASIBLE or INFEASIBLE from the engine.")
    lessons: list[ScheduledLessonModel] = Field(default_factory=list)
    objective_value: float | None = None
    best_objective_bound: float | None = None
    wall_time_seconds: float = 0.0
    penalties: dict[str, int] = Field(default_factory=dict, description="Raw violation per soft constraint.")
    notes: list[str] = Field(default_factory=list)


class TimetableRequest(SolveRequest):
    """The timetabling problem with the settings of one solve (see `SolveRequest` for the shared ones).

    ``mode``, ``weights`` and ``initial_solution`` are the keywords of the same names that
    only this use case's ``solve`` takes.
    """

    model_input: TimetableProblemModel
    mode: Literal["canonical", "quickstart"] = Field(
        default="canonical", description="canonical: every constraint; quickstart: the approximate domain."
    )
    weights: dict[str, float] | None = Field(
        default=None, description="Soft weights by constraint name, for the default fast path (canonical mode)."
    )
    initial_solution: TimetableResultModel | None = Field(
        default=None, description="An earlier result to start from; its lessons are matched by course id."
    )


def solve_request(request: TimetableRequest, *, constraints=None) -> TimetableResultModel:
    """Solve a pydantic request and return the timetable as a `TimetableResultModel`.

    This is ``solve(problem, ...)`` on the request's problem as a dict, with the request's
    settings as the keywords of the same names (see `SolveRequest`), and ``constraints``
    the selection ``solve`` takes. The request is not changed.
    """
    from . import SolverConfig, canon, solve

    config = None
    if request.solver_parameters:
        config = canon.SolverConfig() if request.mode == "canonical" else SolverConfig()
        apply_solver_parameters(config, request.solver_parameters)
    initial = request.initial_solution
    result = solve(
        # Unset optional fields are left out, as in a hand-written problem dict.
        request.model_input.model_dump(mode="json", exclude_none=True),
        mode=request.mode,
        time_limit_seconds=request.time_limit_seconds,
        random_seed=request.random_seed,
        parallel_solver_count=request.parallel_solver_count,
        stop_at_first_feasible=request.stop_at_first_feasible,
        full_assert=request.full_assert,
        weights=request.weights,
        constraints=constraints,
        config=config,
        initial_solution=None if initial is None else initial.model_dump(mode="json"),
    )
    return TimetableResultModel.model_validate(result)
