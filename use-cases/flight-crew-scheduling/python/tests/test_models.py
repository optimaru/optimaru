"""pydantic 미러(models.py) 직렬화/브리지 검증."""

from datetime import date, datetime, timedelta, timezone

import pytest

pytest.importorskip("pydantic")

import flight_crew as fc
from flight_crew.models import (
    AirportModel,
    EmployeeModel,
    FlightAssignmentModel,
    FlightCrewScheduleModel,
    FlightModel,
    SolveConfig,
    datetime_to_minutes,
    minutes_to_datetime,
)

DAY0 = datetime(2026, 8, 10)  # 월요일


def _small_schedule_model() -> FlightCrewScheduleModel:
    return FlightCrewScheduleModel(
        airports=[
            AirportModel(code="ICN", name="Incheon", latitude=37.46, longitude=126.44),
            AirportModel(code="NRT", name="Narita"),
        ],
        employees=[
            EmployeeModel(id="E1", name="Amy Cole", home_airport="ICN", skills=["Pilot"]),
            EmployeeModel(
                id="E2",
                name="Beth Fox",
                home_airport="NRT",
                skills=["Flight attendant"],
                unavailable_days=[date(2026, 8, 11)],
            ),
        ],
        flights=[
            FlightModel(
                flight_number="F1",
                departure_airport="ICN",
                departure_utc_date_time=DAY0 + timedelta(hours=8),
                arrival_airport="NRT",
                arrival_utc_date_time=DAY0 + timedelta(hours=10),
            ),
        ],
        flight_assignments=[
            FlightAssignmentModel(id="A1", flight="F1", index_in_flight=1, required_skill="Pilot"),
            FlightAssignmentModel(
                id="A2",
                flight="F1",
                index_in_flight=2,
                required_skill="Flight attendant",
                employee="E2",
                pinned=True,
            ),
        ],
    )


def test_json_round_trip():
    model = _small_schedule_model()
    clone = FlightCrewScheduleModel.model_validate_json(model.model_dump_json())
    assert clone == model


def test_datetime_minute_bridge():
    dt = datetime(2026, 8, 10, 8, 30)
    minutes = datetime_to_minutes(dt)
    assert minutes % 60 == 30
    assert minutes_to_datetime(minutes) == dt
    # tz-aware 입력은 UTC 로 정규화된다.
    aware = dt.replace(tzinfo=timezone(timedelta(hours=9)))
    assert datetime_to_minutes(aware) == minutes - 9 * 60


def test_to_core_from_core_round_trip():
    model = _small_schedule_model()
    schedule = model.to_core()
    assert [a.code for a in schedule.airports] == ["ICN", "NRT"]
    assert schedule.employees[0].home_airport == 0
    assert schedule.employees[1].home_airport == 1
    assert schedule.flights[0].departure_airport == 0
    assert schedule.flights[0].arrival_utc - schedule.flights[0].departure_utc == 120
    assert schedule.assignment() == [-1, 1]
    assert [a.pinned for a in schedule.flight_assignments] == [False, True]
    # E2 의 불가일은 비행일 다음 날 (day 인덱스로 하루 차이).
    unavailable = next(iter(schedule.employees[1].unavailable_days))
    assert unavailable == datetime_to_minutes(DAY0) // 1440 + 1

    back = FlightCrewScheduleModel.from_core(schedule)
    assert back.airports == model.airports
    assert back.employees == model.employees
    assert back.flights == model.flights
    assert back.flight_assignments == model.flight_assignments
    assert back.score_hard == 0  # 아직 점수 계산 전 (기본값)


def test_reference_validation():
    model = _small_schedule_model()
    model.employees[0].home_airport = "XXX"
    with pytest.raises(ValueError, match="unknown airport"):
        model.to_core()
    with pytest.raises(ValueError, match="duplicate airport code"):
        FlightCrewScheduleModel(
            airports=[AirportModel(code="ICN"), AirportModel(code="ICN")],
            employees=[],
            flights=[],
            flight_assignments=[],
        )


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
    cfg = fc.SolverConfig()
    with pytest.raises(ValueError, match="auto"):
        cfg.late_acceptance_size = -1
    with pytest.raises(ValueError, match="positive"):
        cfg.parallel_solver_count = 0
    with pytest.raises(ValueError, match="exactly 4"):
        cfg.ruin_recreate_rewards = [1.0, 2.0, 3.0]
