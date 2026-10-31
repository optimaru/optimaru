"""The canonical model and its engine conversion (assembly_line_balancing.models)."""

import json
import re
from pathlib import Path

import pytest
from pydantic import ValidationError

import assembly_line_balancing as alb
from assembly_line_balancing.constraints import WorkloadImbalanceCost, get_constraints
from assembly_line_balancing.models import (
    AssemblyLineBalancingProblemModel,
    PrecedenceModel,
    TaskGroupModel,
    TaskModel,
    TaskPairModel,
)

WEB_DEMO = Path(__file__).parent / "fixtures" / "web_demo.json"


def rich_model() -> AssemblyLineBalancingProblemModel:
    return AssemblyLineBalancingProblemModel(
        cycle_time=6,
        max_stations=4,
        tasks=[
            TaskModel(id="A", duration=3, fixed_station=0, forbidden_stations=[2, 2]),
            TaskModel(id="B", duration=2, min_station=1, max_station=3, station=2),
            TaskModel(id="C", duration=4),
            TaskModel(id="D", duration=1, forbidden_stations=[0]),
        ],
        precedences=[PrecedenceModel(before="A", after="B"), PrecedenceModel(before="A", after="B")],
        same_station_groups=[TaskGroupModel(id="zone", task_ids=["C", "D", "D"])],
        different_station_pairs=[TaskPairModel(first="A", second="C"), TaskPairModel(first="B", second="B")],
        warm_start=True,
    )


def test_to_core_resolves_ids_and_keeps_every_fact():
    core = rich_model().to_core()
    assert core.cycle_time == 6 and core.max_stations == 4 and core.warm_start is True
    assert [(t.id, t.duration, t.min_station, t.max_station, t.fixed_station) for t in core.tasks] == [
        ("A", 3, 0, None, 0),
        ("B", 2, 1, 3, None),
        ("C", 4, 0, None, None),
        ("D", 1, 0, None, None),
    ]
    assert [t.forbidden_stations for t in core.tasks] == [[2, 2], [], [], [0]]  # duplicates kept
    assert core.assignment() == [-1, 2, -1, -1]
    assert [(p.before, p.after) for p in core.precedences] == [(0, 1), (0, 1)]
    assert [(g.id, g.tasks) for g in core.same_station_groups] == [("zone", [2, 3, 3])]
    assert [(p.first, p.second) for p in core.different_station_pairs] == [(0, 2), (1, 1)]


def test_round_trip_through_the_core_is_lossless():
    model = rich_model()
    back = AssemblyLineBalancingProblemModel.from_core(model.to_core())
    facts = {"stations_used", "station_loads", "score_hard", "score_soft"}
    assert back.model_dump(exclude=facts) == model.model_dump(exclude=facts)
    clone = AssemblyLineBalancingProblemModel.model_validate_json(back.model_dump_json())
    assert clone == back


def test_none_limits_stay_none():
    model = AssemblyLineBalancingProblemModel(cycle_time=5, tasks=[TaskModel(id="A", duration=1)])
    core = model.to_core()
    assert core.max_stations is None and core.tasks[0].max_station is None and core.tasks[0].fixed_station is None
    back = AssemblyLineBalancingProblemModel.from_core(core)
    assert back.max_stations is None and back.tasks[0].max_station is None


def test_from_core_reports_the_solved_results():
    core = rich_model().to_core()
    alb.solve(core, time_limit_seconds=0)
    result = AssemblyLineBalancingProblemModel.from_core(core)
    assert [t.station for t in result.tasks] == core.assignment()
    assert result.station_loads == core.station_loads()
    assert result.stations_used == core.stations_used()
    assert (result.score_hard, result.score_soft) == (core.score.hard, core.score.soft)
    assert result.warm_start is False  # the seed was used


def test_the_model_and_the_core_solve_alike():
    """solve(model) converts, solves and writes back in place: the same result as the core path."""
    config = alb.SolverConfig()
    config.spent_limit_seconds = 600
    config.step_count_limit = 500
    config.random_seed = 7
    data = json.loads(WEB_DEMO.read_text())
    model = AssemblyLineBalancingProblemModel.model_validate(data)
    returned = alb.solve(model, config=config)
    core = AssemblyLineBalancingProblemModel.model_validate(data).to_core()
    alb.solve(core, config=config)
    assert [t.station for t in model.tasks] == core.assignment()
    assert (model.score_hard, model.score_soft) == (core.score.hard, core.score.soft)
    assert (returned.hard, returned.soft) == (core.score.hard, core.score.soft)
    assert model == AssemblyLineBalancingProblemModel.from_core(core)


@pytest.mark.parametrize("constraints", [None, [*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)]])
def test_full_assert_solves_on_both_paths(constraints):
    """FULL_ASSERT cross-checks the incremental score after every move (monolith and plugin assembly)."""
    config = alb.SolverConfig()
    config.spent_limit_seconds = 600
    config.step_count_limit = 2000
    config.random_seed = 1
    config.full_assert = True
    config.ruin_recreate_unimproved_step_trigger = 100
    config.ruin_recreate_polish_steps = 50
    model = AssemblyLineBalancingProblemModel.model_validate(json.loads(WEB_DEMO.read_text()))
    alb.solve(model, config=config, constraints=constraints)
    check = alb.calculate_score(model.model_copy(deep=True), constraints)
    assert (check.hard, check.soft) == (model.score_hard, model.score_soft)


def test_the_input_schema_is_unchanged():
    """The canonical problem's JSON fields (the server's OpenAPI input/output) did not change in 0.2.0."""
    schema = AssemblyLineBalancingProblemModel.model_json_schema()
    assert list(schema["properties"]) == [
        "cycle_time",
        "tasks",
        "precedences",
        "max_stations",
        "same_station_groups",
        "different_station_pairs",
        "stations_used",
        "station_loads",
        "score_hard",
        "score_soft",
        "warm_start",
    ]
    assert list(schema["$defs"]["TaskModel"]["properties"]) == [
        "id",
        "duration",
        "min_station",
        "max_station",
        "fixed_station",
        "forbidden_stations",
        "station",
    ]


@pytest.mark.parametrize(
    "mutate, message",
    [
        (lambda d: d.update(weights={"used_stations_cost": 2}), "Extra inputs"),
        (lambda d: d["tasks"][0].update(duration=0), "greater than 0"),
        (lambda d: d["precedences"].append({"before": "Frame", "after": "Nope"}), "must be tasks"),
        (lambda d: d["tasks"][0].update(station=-2), "greater than or equal to -1"),
    ],
)
def test_invalid_input_is_refused_before_the_engine(mutate, message):
    data = json.loads(WEB_DEMO.read_text())
    mutate(data)
    with pytest.raises(ValidationError, match=message):
        AssemblyLineBalancingProblemModel.model_validate(data)


def test_the_core_refuses_what_the_engine_cannot_score():
    core = rich_model().to_core()
    with pytest.raises(ValueError, match="below -1"):
        core.set_assignment([0, 0, 0, -2])
    with pytest.raises(ValueError, match="length"):
        core.set_assignment([0])
    bad = alb.AssemblyLineBalancingProblem(5, [alb.Task("A", 0)])
    with pytest.raises(ValueError, match="duration must be positive"):
        alb.calculate_score(bad)
    cyclic = alb.AssemblyLineBalancingProblem(5, [alb.Task("A", 1)], [alb.Precedence(0, 0)])
    with pytest.raises(ValueError, match="precedence graph must be acyclic"):
        alb.solve(cyclic, time_limit_seconds=0)
    with pytest.raises(TypeError, match="problem must be"):
        alb.solve({"cycle_time": 5, "tasks": []})


@pytest.mark.parametrize(
    "tasks, max_stations, message",
    [
        ([alb.Task("A", 2, fixed_station=-3)], None, "task 'A': fixed_station must be >= 0"),
        ([alb.Task("A", 2, min_station=-1)], None, "task 'A': min_station must be >= 0"),
        ([alb.Task("A", 2, min_station=2, max_station=1)], None, "task 'A': max_station must be >= min_station"),
        ([alb.Task("A", 2)], 0, "max_stations must be >= 1"),
    ],
)
def test_the_core_refuses_the_station_domains_the_model_refuses(tasks, max_stations, message):
    # Direct core users get the pydantic model's rules, with the field named, on both
    # the scoring and the solve path (before the construction copies a fixed station).
    problem = alb.AssemblyLineBalancingProblem(5, tasks, max_stations=max_stations)
    with pytest.raises(ValueError, match=re.escape(message)):
        alb.calculate_score(problem)
    with pytest.raises(ValueError, match=re.escape(message)):
        alb.solve(problem, time_limit_seconds=0)
