"""Pydantic mirrors (models.py): serialization and the bridge to the engine."""

import pytest

import optimaru.school_timetabling as st
from optimaru.school_timetabling.models import (
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
    assert cfg.best_score_limit == (0, float("-inf"))  # stop_at_first_feasible
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


# ------------------------------------------------------- the objective comes from the catalog


def test_objective_weights_are_generated_from_the_constraint_catalog():
    """The weight fields are not written by hand — the soft constraints are the terms of the objective.

    Two separately kept lists drift apart as soon as a constraint is added. This
    pins that both the names and the defaults come from the C++ catalog
    (constraint_info(): kDefaultLevel / kDefaultWeight)."""
    from optimaru.school_timetabling.constraints import default_weights, soft_specs
    from optimaru.school_timetabling.models import ObjectiveWeightsModel

    soft_rows = [row for row in st.constraint_info() if row.default_level == "soft"]
    assert list(ObjectiveWeightsModel.model_fields) == [spec.name for spec in soft_specs()]
    assert list(ObjectiveWeightsModel.model_fields) == [row.name for row in soft_rows]
    assert (
        ObjectiveWeightsModel().model_dump() == default_weights() == {row.name: row.default_weight for row in soft_rows}
    )
    assert ObjectiveWeightsModel().to_core() == st.canon.ObjectiveWeights()


@pytest.mark.parametrize("old", ["teacher_gap", "teacher_workday", "teacher_free_day", "class_gap"])
def test_objective_weights_refuse_the_old_field_names(old):
    from pydantic import ValidationError

    from optimaru.school_timetabling.models import ObjectiveWeightsModel

    with pytest.raises(ValidationError):
        ObjectiveWeightsModel.model_validate({old: 1})


def test_objective_weights_take_non_integer_values():
    from optimaru.school_timetabling.models import ObjectiveWeightsModel, SolveConfig

    weights = ObjectiveWeightsModel(teacher_gap_cost=2.5)
    assert weights.to_core().teacher_gap_cost == 2.5
    assert SolveConfig(weights=weights).to_canon().weights.teacher_gap_cost == 2.5


@pytest.mark.parametrize("bad", [-1, float("nan"), float("inf")])
def test_objective_weights_refuse_non_finite_and_negative_values(bad):
    """The pydantic mirror and the C++ struct refuse the same weights a constraint pick refuses."""
    from pydantic import ValidationError

    from optimaru.school_timetabling.models import ObjectiveWeightsModel

    with pytest.raises(ValidationError):
        ObjectiveWeightsModel(teacher_gap_cost=bad)
    with pytest.raises(ValueError, match="teacher_gap_cost: weight must be finite and >= 0"):
        st.canon.ObjectiveWeights(teacher_gap_cost=bad)
    weights = st.canon.ObjectiveWeights()
    with pytest.raises(ValueError, match="teacher_gap_cost: weight must be finite and >= 0"):
        weights.teacher_gap_cost = bad
    assert weights == st.canon.ObjectiveWeights()


@pytest.mark.parametrize("bad", [True, "20", None])
def test_core_objective_weights_refuse_bools_and_non_numbers(bad):
    with pytest.raises(TypeError, match="class_gap_cost: weight must be a number"):
        st.canon.ObjectiveWeights(class_gap_cost=bad)
    weights = st.canon.ObjectiveWeights()
    with pytest.raises(TypeError, match="class_gap_cost: weight must be a number"):
        weights.class_gap_cost = bad
    weights.class_gap_cost = 4  # an int is a real number
    assert weights.class_gap_cost == 4.0


def test_a_term_the_fast_path_does_not_know_is_left_out_of_to_core():
    """A soft term only the catalog knows is silently left out by to_core (the plugin path uses it)."""
    from pydantic import Field, create_model

    from optimaru.school_timetabling.models import _ObjectiveWeightsBase

    generated = create_model(
        "Extended",
        __base__=_ObjectiveWeightsBase,
        first_period_load_cost=(float, Field(default=7.0, ge=0)),
    )
    assert generated().model_dump() == {"first_period_load_cost": 7.0}
    assert "first_period_load_cost" not in generated().to_core().__dir__()


def _dump_with_slot_room(value):
    data = _small_table_model().model_dump()
    data["lessons"][0]["slot_room"] = value
    return data


@pytest.mark.parametrize("value", [-2, 2, 5, 99999999])
def test_a_slot_room_out_of_range_is_refused_before_the_engine(value):
    with pytest.raises(ValueError, match=rf"lesson 'L1': slot_room {value} is out of range -1\.\.1"):
        TimetableModel.model_validate(_dump_with_slot_room(value))


@pytest.mark.parametrize("value", [-1, 0, 1])
def test_every_slot_room_in_range_is_accepted(value):
    assert TimetableModel.model_validate(_dump_with_slot_room(value)).to_core().lessons[0].slot_room == value


def test_a_slot_room_edited_after_validation_is_refused_by_to_core():
    model = _small_table_model()
    model.lessons[0].slot_room = 99999999
    with pytest.raises(ValueError, match="out of range"):
        model.to_core()
