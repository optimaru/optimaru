"""pydantic 미러(models.py) 직렬화/브리지 검증."""

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
        rooms=[RoomModel(name="1-1교실", type="일반")],
        lessons=[
            LessonModel(id="L1", subject="수학", teacher="박수학", student_group="1-1"),
            LessonModel(
                id="L2",
                subject="국어",
                teacher="김국어",
                student_group="1-2",
                slot_room=0,
                pinned=True,
            ),
        ],
        teacher_unavailable={"김국어": [1]},
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
    assert table.teacher_unavailable == {"김국어": {1}}
    assert table.assignment() == [-1, 0]
    assert [l.pinned for l in table.lessons] == [False, True]

    back = TimetableModel.from_core(table)
    assert back.timeslots == model.timeslots
    assert back.rooms == model.rooms
    assert back.lessons == model.lessons
    assert back.teacher_unavailable == model.teacher_unavailable
    assert back.score_hard == 0  # 아직 점수 계산 전 (기본값)


def test_calculate_score_via_core_bridge():
    # 같은 교사 두 수업을 같은 칸(같은 교실)에 두면 room + teacher 충돌 = -2hard.
    model = _small_table_model()
    table = model.to_core()
    table.set_assignment([0, 0])
    lessons = table.lessons
    assert lessons[0].slot_room == lessons[1].slot_room == 0

    table2 = TimetableModel(
        timeslots=model.timeslots,
        rooms=model.rooms,
        lessons=[
            LessonModel(id="L1", subject="수학", teacher="김국어", student_group="1-1"),
            LessonModel(id="L2", subject="국어", teacher="김국어", student_group="1-2"),
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
    cfg.late_acceptance_size = 0  # 0 = 자동 (인스턴스 크기 기반)
    with pytest.raises(ValueError, match="auto"):
        cfg.late_acceptance_size = -1
    with pytest.raises(ValueError, match="positive"):
        cfg.parallel_solver_count = 0
    with pytest.raises(ValueError, match="exactly 4"):
        cfg.ruin_recreate_rewards = [1.0, 2.0, 3.0]


# ------------------------------------------------------- 목적함수는 레지스트리에서 온다


def test_objective_weights_are_generated_from_the_constraint_registry():
    """가중치 필드를 손으로 적지 않는다 — 소프트 제약이 곧 목적함수의 항이다.

    두 목록을 따로 들고 있으면 제약을 추가할 때 반드시 갈라진다. 이름도
    기본값도 스펙에서 나오는지 고정한다.
    """
    from st_timetabling.constraints import default_weights, soft_specs
    from st_timetabling.models import ObjectiveWeightsModel

    assert list(ObjectiveWeightsModel.model_fields) == [spec.name for spec in soft_specs()]
    assert ObjectiveWeightsModel().model_dump() == default_weights()


def test_a_new_soft_constraint_gets_a_weight_field_by_itself():
    """소프트 제약을 하나 더 등록하면 목적함수 항이 저절로 따라온다."""
    from pydantic import Field, create_model

    from st_timetabling.constraints import Constraint
    from st_timetabling.models import _ObjectiveWeightsBase

    class FirstPeriodLoad(Constraint):
        """S7 (가상): 1교시 배정당 1점."""

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
    # 모놀리식 구조체가 모르는 항은 to_core 에서 조용히 빠진다 (조립 경로가 쓴다).
    assert "first_period_load" not in generated().to_core().__dir__()
