"""with_initial_solution: a seed reaches the search untouched and bad input never reaches C++."""

import random

import pytest

pytest.importorskip("pydantic")

import facility_location as fl
from facility_location.models import (
    ConsumerModel,
    FacilityLocationProblemModel,
    FacilityModel,
    LocationModel,
)


def _problem(facility_count=8, consumer_count=24):
    rng = random.Random(0)

    def location():
        return LocationModel(latitude=rng.uniform(51.44, 51.56), longitude=rng.uniform(-0.16, -0.01))

    return FacilityLocationProblemModel(
        facilities=[
            FacilityModel(id=f"f{i}", location=location(), setup_cost=rng.randint(1000, 5000), capacity=200)
            for i in range(facility_count)
        ],
        consumers=[ConsumerModel(id=f"c{i}", location=location(), demand=10) for i in range(consumer_count)],
    )


def _facilities(model):
    return {consumer.id: consumer.facility for consumer in model.consumers}


def _score(model):
    return (model.score_hard, model.score_soft)


def _solve(model, seconds):
    core = model.to_core()
    fl.solve(core, time_limit_seconds=seconds, random_seed=1)
    return FacilityLocationProblemModel.from_core(core)


def _construct(model):
    """Construction only: step_count_limit = 0 means "no limit", a zero time budget stops before the first step."""
    return _solve(model, 0)


def _non_greedy_seed(problem):
    """Every consumer one facility along from where construction puts it."""
    seed = _construct(problem)
    for consumer in seed.consumers:
        consumer.facility = (consumer.facility + 1) % len(problem.facilities)
    return seed


def test_exported_from_the_package():
    assert "with_initial_solution" in fl.__all__


def test_complete_seed_skips_construction():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    result = _construct(fl.with_initial_solution(problem, seed))
    assert _facilities(result) == _facilities(seed)
    assert _facilities(result) != _facilities(_construct(problem))


@pytest.mark.parametrize("as_json", [False, True])
def test_solved_result_feeds_back(as_json):
    problem = _problem()
    solved = _solve(problem, 0.3)
    solution = solved.model_dump(mode="json") if as_json else solved
    result = _construct(fl.with_initial_solution(problem, solution))
    assert _facilities(result) == _facilities(solved)
    assert _score(result) == _score(solved)


def test_partial_seed_keeps_its_part_and_fills_the_rest():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    solution = seed.model_dump(mode="json")
    solution["consumers"] = solution["consumers"][:10]
    kept = {consumer["id"]: consumer["facility"] for consumer in solution["consumers"]}

    seeded = fl.with_initial_solution(problem, solution)
    assert all(value == -1 for consumer_id, value in _facilities(seeded).items() if consumer_id not in kept)

    result = _construct(seeded)
    assert all(_facilities(result)[consumer_id] == value for consumer_id, value in kept.items())
    assert all(value >= 0 for value in _facilities(result).values())


def test_consumers_are_matched_by_id_and_prior_values_are_replaced():
    solved = _non_greedy_seed(_problem())
    last = solved.consumers[-1]
    seeded = fl.with_initial_solution(solved, {"consumers": [{"id": last.id, "facility": last.facility}]})
    assert _facilities(seeded) == {c.id: last.facility if c.id == last.id else -1 for c in solved.consumers}
    assert _score(seeded) == (None, None)


def test_unknown_consumer_raises_naming_it():
    with pytest.raises(ValueError, match="no-such-consumer"):
        fl.with_initial_solution(_problem(), {"consumers": [{"id": "no-such-consumer", "facility": 0}]})


def test_highest_value_is_accepted():
    problem = _problem()
    solution = {"consumers": [{"id": "c0", "facility": len(problem.facilities) - 1}]}
    assert fl.with_initial_solution(problem, solution).consumers[0].facility == len(problem.facilities) - 1


@pytest.mark.parametrize("offset", [0, 1, 9_000_000])
def test_value_past_the_end_raises_naming_the_consumer(offset):
    problem = _problem()
    with pytest.raises(ValueError, match="c0"):
        fl.with_initial_solution(problem, {"consumers": [{"id": "c0", "facility": len(problem.facilities) + offset}]})


def test_value_past_the_end_on_a_model_raises():
    problem = _problem()
    seed = _construct(problem)
    seed.consumers[3].facility = len(problem.facilities)
    with pytest.raises(ValueError, match="c3"):
        fl.with_initial_solution(problem, seed)


@pytest.mark.parametrize("value", [-2, None, "1", 1.0, True])
def test_value_of_the_wrong_kind_raises_naming_the_consumer(value):
    with pytest.raises(ValueError, match="c0"):
        fl.with_initial_solution(_problem(), {"consumers": [{"id": "c0", "facility": value}]})


def test_repeated_consumer_raises():
    with pytest.raises(ValueError, match="c0"):
        fl.with_initial_solution(_problem(), {"consumers": [{"id": "c0"}, {"id": "c0"}]})


def test_repeated_id_on_the_problem_raises():
    problem = _problem()
    problem.consumers[1].id = problem.consumers[0].id
    with pytest.raises(ValueError, match="c0"):
        fl.with_initial_solution(problem, {"consumers": []})


def test_index_over_a_different_facility_order_raises():
    problem = _problem()
    solution = _construct(problem).model_dump(mode="json")
    solution["facilities"].reverse()
    with pytest.raises(ValueError, match="facilities"):
        fl.with_initial_solution(problem, solution)


@pytest.mark.parametrize("solution", [None, [], {}, {"consumers": "x"}, {"consumers": ["x"]}, {"consumers": [{}]}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError):
        fl.with_initial_solution(_problem(), solution)


def test_inputs_are_not_mutated():
    problem = _construct(_problem())
    seed = _non_greedy_seed(problem)
    problem_before, seed_before = problem.model_dump(), seed.model_dump()
    seeded = fl.with_initial_solution(problem, seed)
    assert problem.model_dump() == problem_before
    assert seed.model_dump() == seed_before
    assert seeded is not problem and seeded.consumers[0] is not problem.consumers[0]


def test_seeded_solve_is_not_worse_than_the_seed():
    problem = _problem()
    seeded = fl.with_initial_solution(problem, _non_greedy_seed(problem))
    start = _construct(seeded)
    assert _score(_solve(seeded, 0.3)) >= _score(start)
