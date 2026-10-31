"""models.SemiconductorScheduleModel <-> _core.SemiconductorSchedule."""

import json
from pathlib import Path

import pytest
from fab_random import instance
from pydantic import ValidationError

import semiconductor_scheduling as fab
from semiconductor_scheduling import _core
from semiconductor_scheduling.models import AssignmentModel, SemiconductorScheduleModel

# A copy of the web demo sample (web: src/lib/demo-data/semiconductor-scheduling.json).
WEB_DEMO = Path(__file__).parent / "data" / "web-demo.json"


def _web_demo() -> dict | None:
    return json.loads(WEB_DEMO.read_text()) if WEB_DEMO.exists() else None


@pytest.mark.parametrize("seed", range(40))
def test_round_trip_is_lossless(seed):
    """Facts, pins, assignments (batch ids None / "" / strings) and warm_start survive; an
    unscored schedule comes back with None totals and scores."""
    data = instance(seed)
    data["warm_start"] = seed % 2 == 0
    model = SemiconductorScheduleModel.model_validate(data)
    core = model.to_core()
    assert isinstance(core, _core.SemiconductorSchedule)
    back = SemiconductorScheduleModel.from_core(core)
    assert back == model
    assert back.score_hard is None and back.makespan is None


def test_round_trip_of_the_web_demo_and_of_a_scored_schedule():
    data = _web_demo()
    if data is None:
        pytest.skip("web demo sample not available")
    model = SemiconductorScheduleModel.model_validate(data)
    assert SemiconductorScheduleModel.from_core(model.to_core()) == model
    core = model.to_core()
    fab.calculate_score(core)
    scored = SemiconductorScheduleModel.from_core(core)
    assert (scored.score_hard, scored.score_soft) == (core.score.hard, core.score.soft)
    assert (scored.makespan, scored.total_weighted_tardiness, scored.total_cycle_time) == (
        core.makespan,
        core.total_weighted_tardiness,
        core.total_cycle_time,
    )


def test_solving_a_core_schedule_writes_it_back_through_from_core():
    """The server path: to_core() -> solve(core) -> from_core(core)."""
    model = SemiconductorScheduleModel.model_validate(instance(11))
    core = model.to_core()
    result = fab.solve(core, time_limit_seconds=5, random_seed=1, config=_steps(100))
    solved = SemiconductorScheduleModel.from_core(core)
    assert (solved.score_hard, solved.score_soft) == tuple(result)
    assert len(solved.assignments) == sum(len(lot.operations) for lot in solved.lots)
    assert solved.warm_start is False
    batch_tools = {machine.id for machine in solved.machines if machine.batch_capacity > 1}
    for item in solved.assignments:
        # Decoded rows get batch ids on batch tools only (the fixtures' pins carry none).
        assert (item.batch_id is not None) == (item.machine_id in batch_tools)


def test_core_validates_like_the_model():
    """The engine refuses what the Pydantic validator refuses (ValueError)."""
    model = SemiconductorScheduleModel.model_validate(instance(2))
    core = model.to_core()
    machines = core.machines
    lots = core.lots
    with pytest.raises(ValueError, match="machine ids must be unique"):
        _core.SemiconductorSchedule(machines=[machines[0], machines[0]], lots=lots[:0])
    with pytest.raises(ValueError, match="assigned more than once"):
        core.assignments = [_core.Assignment(0, 0, 0, 0, 1), _core.Assignment(0, 0, 0, 2, 3)]
    with pytest.raises(ValueError, match="unknown operation"):
        core.assignments = [_core.Assignment(len(lots), 0, 0, 0, 1)]


@pytest.mark.parametrize(
    ("change", "message"),
    [
        (lambda d: d["machines"].append(dict(d["machines"][0])), "machine ids must be unique"),
        (lambda d: d["lots"].append(dict(d["lots"][0])), "lot ids must be unique"),
        (lambda d: d["assignments"].append(dict(d["assignments"][0])), "assigned more than once"),
    ],
)
def test_model_validator_errors_are_unchanged(change, message):
    data = instance(4)
    assert data["assignments"]
    change(data)
    with pytest.raises(ValidationError, match=message):
        SemiconductorScheduleModel.model_validate(data)


def test_times_beyond_the_engine_range_raise_value_error():
    data = instance(1)
    data["lots"][0]["due_time"] = 2**70
    model = SemiconductorScheduleModel.model_validate(data)
    with pytest.raises(ValueError, match="outside the engine's range"):
        model.to_core()


def test_extra_fields_are_refused():
    data = instance(1)
    data["weights"] = {"makespan": 2}
    with pytest.raises(ValidationError):
        SemiconductorScheduleModel.model_validate(data)


def test_calculate_score_on_a_model_writes_totals_and_scores():
    model = SemiconductorScheduleModel.model_validate(instance(6))
    score = fab.calculate_score(model)
    assert (model.score_hard, model.score_soft) == tuple(score)
    raw = fab.constraint_costs(model)
    assert model.makespan == raw["makespan_cost"]["costs"][0]
    assert model.total_weighted_tardiness == raw["weighted_tardiness_cost"]["costs"][0]
    assert model.total_cycle_time == raw["cycle_time_cost"]["costs"][0]
    assert all(isinstance(item, AssignmentModel) for item in model.assignments)


def test_score_is_a_comparable_pair():
    low = _core.Score(-1, 0.0)
    high = _core.Score(0, -5.0)
    assert low < high and high >= low and low != high
    assert tuple(high) == (0, -5.0) and high == (0, -5.0) and list(high) == [0, -5.0]
    assert high[0] == 0 and high[-1] == -5.0 and len(high) == 2
    hard, soft = high
    assert (hard, soft) == (0, -5.0)


def _steps(count: int) -> fab.SolverConfig:
    config = fab.SolverConfig()
    config.step_count_limit = count
    return config
