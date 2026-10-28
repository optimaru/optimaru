"""Pydantic mirrors (models.py): serialization and the bridge to the engine."""

import pytest

pytest.importorskip("pydantic")

import st_timetabling as st
from st_timetabling.models import (
    LessonModel,
    RoomModel,
    SolveConfig,
    TimeslotModel,
    TimetableModel,
)


def _small_table_model() -> TimetableModel:
    return TimetableModel(
        timeslots=[TimeslotModel(day=0, period=1), TimeslotModel(day=0, period=2)],
        rooms=[RoomModel(name="Room 1-1", type="regular")],
        lessons=[
            LessonModel(id="L1", subject="Math", teacher="Park (Math)", student_group="1-1"),
            LessonModel(
                id="L2",
                subject="Korean",
                teacher="Kim (Korean)",
                student_group="1-2",
                slot_room=0,
                pinned=True,
            ),
        ],
        teacher_unavailable={"Kim (Korean)": [1]},
    )


def test_json_round_trip():
    model = _small_table_model()
    clone = TimetableModel.model_validate_json(model.model_dump_json())
    assert clone == model


def test_to_core_from_core_round_trip():
    model = _small_table_model()
    table = model.to_core()
    assert table.timeslot_count() == 2
    assert table.room_count() == 1
    assert table.teacher_unavailable == {"Kim (Korean)": {1}}
    assert table.assignment() == [-1, 0]
    assert [l.pinned for l in table.lessons] == [False, True]

    back = TimetableModel.from_core(table)
    assert back.timeslots == model.timeslots
    assert back.rooms == model.rooms
    assert back.lessons == model.lessons
    assert back.teacher_unavailable == model.teacher_unavailable
    assert back.score_hard == 0  # not scored yet (default)


def test_calculate_score_via_core_bridge():
    # Two lessons of the same teacher in the same cell (same room): room + teacher clash = -2hard.
    model = _small_table_model()
    table = model.to_core()
    table.set_assignment([0, 0])
    lessons = table.lessons
    assert lessons[0].slot_room == lessons[1].slot_room == 0

    table2 = TimetableModel(
        timeslots=model.timeslots,
        rooms=model.rooms,
        lessons=[
            LessonModel(id="L1", subject="Math", teacher="Kim (Korean)", student_group="1-1"),
            LessonModel(id="L2", subject="Korean", teacher="Kim (Korean)", student_group="1-2"),
        ],
    ).to_core()
    table2.set_assignment([0, 0])
    score = st.calculate_score(table2)
    assert score.hard == -2
    assert not score.is_feasible()
    assert table2.score.hard == -2


def test_solve_config_to_core():
    config = SolveConfig(
        time_limit_seconds=1.5,
        random_seed=9,
        parallel_solver_count=2,
        late_acceptance_size=50,
        stop_at_first_feasible=True,
        unimproved_spent_limit_seconds=0.5,
        step_count_limit=100,
        unimproved_step_count_limit=40,
        ruin_recreate=False,
        ruin_recreate_rewards=[10.0, 5.0, 1.0, 0.0],
        change_move_weight=1.0,
        swap_move_weight=0.0,
    )
    clone = SolveConfig.model_validate_json(config.model_dump_json())
    assert clone == config

    cfg = clone.to_core()
    assert cfg.spent_limit_seconds == pytest.approx(1.5)
    assert cfg.random_seed == 9
    assert cfg.parallel_solver_count == 2
    assert cfg.late_acceptance_size == 50
    assert cfg.best_score_limit == (0, -1e18)  # stop_at_first_feasible
    assert cfg.unimproved_spent_limit_seconds == pytest.approx(0.5)
    assert cfg.step_count_limit == 100
    assert cfg.unimproved_step_count_limit == 40
    assert cfg.ruin_recreate is False
    assert cfg.ruin_recreate_rewards == [10.0, 5.0, 1.0, 0.0]
    assert cfg.change_move_weight == 1.0
    assert cfg.swap_move_weight == 0.0


def test_solve_config_validation():
    with pytest.raises(ValueError):
        SolveConfig(ruin_recreate_min_ruin_fraction=0.5, ruin_recreate_max_ruin_fraction=0.2)
    with pytest.raises(ValueError):
        SolveConfig(ruin_recreate_rewards=[1.0, 2.0, 3.0])
    cfg = st.SolverConfig()
    cfg.late_acceptance_size = 0  # 0 = auto (from the instance size)
    with pytest.raises(ValueError, match="auto"):
        cfg.late_acceptance_size = -1
    with pytest.raises(ValueError, match="positive"):
        cfg.parallel_solver_count = 0
    with pytest.raises(ValueError, match="exactly 4"):
        cfg.ruin_recreate_rewards = [1.0, 2.0, 3.0]


# ------------------------------------------------------- the objective comes from the registry


def test_objective_weights_are_generated_from_the_constraint_registry():
    """The weight fields are not written by hand — the soft constraints are the terms of the objective.

    Two separately kept lists drift apart as soon as a constraint is added. This
    pins that both the names and the defaults come from the specs.
    """
    from st_timetabling.constraints import default_weights, soft_specs
    from st_timetabling.models import ObjectiveWeightsModel

    assert list(ObjectiveWeightsModel.model_fields) == [spec.name for spec in soft_specs()]
    assert ObjectiveWeightsModel().model_dump() == default_weights()


def test_a_new_soft_constraint_gets_a_weight_field_by_itself():
    """Registering one more soft constraint brings its objective term along automatically."""
    from pydantic import Field, create_model

    from st_timetabling.constraints import Constraint
    from st_timetabling.models import _ObjectiveWeightsBase

    class FirstPeriodLoad(Constraint):
        """S7 (hypothetical): 1 point per lesson in period 1."""

        name = "first_period_load"
        default_hard = False
        default_weight = 7

    specs = (FirstPeriodLoad,)
    generated = create_model(
        "Extended",
        __base__=_ObjectiveWeightsBase,
        **{s.name: (int, Field(default=int(s.default_weight), ge=0)) for s in specs},
    )
    assert generated().model_dump() == {"first_period_load": 7}
    # A term the monolithic struct does not know is silently left out by to_core (the assembly path uses it).
    assert "first_period_load" not in generated().to_core().__dir__()
