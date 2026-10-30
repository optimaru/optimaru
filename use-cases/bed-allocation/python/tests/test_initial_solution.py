"""with_initial_solution: a seed reaches the search untouched and bad input never reaches C++."""

import pytest

pytest.importorskip("pydantic")

import bed_allocation as ba
from bed_allocation.demo import generate_demo_data
from bed_allocation.models import BedPlanModel


def _beds(model):
    return {stay.id: stay.bed_id for stay in model.stays}


def _score(model):
    return (model.score_hard, model.score_medium, model.score_soft)


def _construct(model):
    """Construction only: step_count_limit = 0 means "no limit", a zero time budget stops before the first step."""
    return ba.solve(model, time_limit_seconds=0, random_seed=1)


def _non_greedy_seed(plan):
    """Every stay one bed along from where construction puts it, so no stay is left unassigned."""
    seed = _construct(plan)
    bed_ids = [bed.id for bed in plan.beds]
    for stay in seed.stays:
        position = bed_ids.index(stay.bed_id) if stay.bed_id is not None else -1
        stay.bed_id = bed_ids[(position + 1) % len(bed_ids)]
    return seed


def test_exported_from_the_package():
    assert "with_initial_solution" in ba.__all__


def test_complete_seed_skips_construction():
    plan = generate_demo_data()
    seed = _non_greedy_seed(plan)
    result = _construct(ba.with_initial_solution(plan, seed))
    assert _beds(result) == _beds(seed)
    assert _beds(result) != _beds(_construct(plan))


@pytest.mark.parametrize("as_json", [False, True])
def test_solved_result_feeds_back(as_json):
    plan = generate_demo_data()
    solved = ba.solve(plan, time_limit_seconds=0.5, random_seed=1)
    solution = solved.model_dump(mode="json") if as_json else solved
    seeded = ba.with_initial_solution(plan, solution)
    assert _beds(seeded) == _beds(solved)
    result = _construct(seeded)
    assert isinstance(result, BedPlanModel)
    assert all(_beds(result)[stay_id] == bed for stay_id, bed in _beds(solved).items() if bed is not None)
    assert _score(result) >= _score(solved)


def test_no_bed_stays_come_back_unassigned_and_are_placed_again():
    plan = generate_demo_data()
    seed = _construct(plan)
    no_bed = [stay.id for stay in seed.stays if stay.bed_id is None]
    assert no_bed
    core = ba.with_initial_solution(plan, seed).to_core()
    index = {stay.id: i for i, stay in enumerate(plan.stays)}
    assert all(core.assignment()[index[stay_id]] == -1 for stay_id in no_bed)
    ba.solve(core, time_limit_seconds=0, random_seed=1)
    assert all(value >= 0 for value in core.assignment())


def test_partial_seed_keeps_its_part_and_fills_the_rest():
    plan = generate_demo_data()
    seed = _non_greedy_seed(plan)
    solution = seed.model_dump(mode="json")
    solution["stays"] = solution["stays"][:40]
    kept = {stay["id"]: stay["bed_id"] for stay in solution["stays"]}

    seeded = ba.with_initial_solution(plan, solution)
    assert all(bed is None for stay_id, bed in _beds(seeded).items() if stay_id not in kept)

    core = seeded.to_core()
    ba.solve(core, time_limit_seconds=0, random_seed=1)
    result = BedPlanModel.from_core(core)
    assert all(_beds(result)[stay_id] == bed for stay_id, bed in kept.items())
    assert all(value >= 0 for value in core.assignment())
    assert any(bed is not None for stay_id, bed in _beds(result).items() if stay_id not in kept)


def test_stays_are_matched_by_id_and_prior_beds_are_replaced():
    solved = _non_greedy_seed(generate_demo_data())
    last = solved.stays[-1]
    solution = {"stays": [{"id": last.id, "bed_id": last.bed_id}, {"id": solved.stays[0].id, "bed_id": None}]}
    seeded = ba.with_initial_solution(solved, solution)
    assert _beds(seeded) == {stay.id: last.bed_id if stay.id == last.id else None for stay in solved.stays}
    assert (seeded.score_hard, seeded.score_medium, seeded.score_soft) == (None, None, None)


def test_unknown_stay_raises_naming_it():
    plan = generate_demo_data()
    with pytest.raises(ValueError, match="no-such-stay"):
        ba.with_initial_solution(plan, {"stays": [{"id": "no-such-stay", "bed_id": None}]})


@pytest.mark.parametrize("bed_id", ["no-such-bed", 3, ["bed"], True])
def test_unknown_bed_raises_naming_the_stay(bed_id):
    plan = generate_demo_data()
    stay_id = plan.stays[0].id
    with pytest.raises(ValueError, match=stay_id):
        ba.with_initial_solution(plan, {"stays": [{"id": stay_id, "bed_id": bed_id}]})


def test_repeated_stay_raises():
    plan = generate_demo_data()
    stay_id = plan.stays[0].id
    with pytest.raises(ValueError, match=stay_id):
        ba.with_initial_solution(plan, {"stays": [{"id": stay_id}, {"id": stay_id}]})


def test_repeated_id_on_the_problem_raises():
    plan = generate_demo_data()
    plan.stays[1].id = plan.stays[0].id
    with pytest.raises(ValueError, match=plan.stays[0].id):
        ba.with_initial_solution(plan, {"stays": []})


@pytest.mark.parametrize("solution", [None, [], {}, {"stays": "x"}, {"stays": ["x"]}, {"stays": [{}]}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError):
        ba.with_initial_solution(generate_demo_data(), solution)


def test_inputs_are_not_mutated():
    plan = _construct(generate_demo_data())
    seed = _non_greedy_seed(plan)
    plan_before, seed_before = plan.model_dump(), seed.model_dump()
    seeded = ba.with_initial_solution(plan, seed)
    assert plan.model_dump() == plan_before
    assert seed.model_dump() == seed_before
    assert seeded is not plan and seeded.stays[0] is not plan.stays[0]


def test_seeded_solve_is_not_worse_than_the_seed():
    plan = generate_demo_data()
    seeded = ba.with_initial_solution(plan, _non_greedy_seed(plan))
    start = _construct(seeded)
    assert _score(ba.solve(seeded, time_limit_seconds=0.3, random_seed=1)) >= _score(start)
