"""with_initial_solution: a solve that starts from a given assignment."""

import pytest

import optimaru.box_sizing as bs
from optimaru.box_sizing.models import BoxSizingProblemModel, with_initial_solution


def boxes_of(model) -> dict:
    return {parcel.id: parcel.box for parcel in model.parcels}


def test_exported_from_the_package():
    assert bs.with_initial_solution is with_initial_solution


def test_a_complete_seed_with_a_zero_budget_comes_back_as_it_is(nested):
    problem = nested()
    seed = {"parcels": [{"id": "small", "box": "L"}, {"id": "medium", "box": "L"}, {"id": "large", "box": "L"}]}
    seeded = with_initial_solution(problem, seed)
    assert seeded.warm_start is True
    solved = bs.solve(seeded, time_limit_seconds=0)
    assert boxes_of(solved) == {"small": "L", "medium": "L", "large": "L"}
    assert solved.void_volume == 10 * 26000 + 5 * 21000


def test_a_plain_solve_ignores_the_boxes_on_the_parcels(nested):
    data = nested().model_dump()
    for parcel in data["parcels"]:
        parcel["box"] = "L"
    solved = bs.solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=0)
    assert boxes_of(solved) == {"small": "S", "medium": "M", "large": "L"}


@pytest.mark.parametrize("as_json", [False, True])
def test_a_solved_result_feeds_back(nested, as_json):
    problem = bs.generate_demo_data()
    solved = bs.solve(problem, time_limit_seconds=0.2, random_seed=1)
    solution = solved.model_dump(mode="json") if as_json else solved
    resumed = bs.solve(with_initial_solution(problem, solution), time_limit_seconds=0)
    assert boxes_of(resumed) == boxes_of(solved)
    assert (resumed.score_hard, resumed.score_soft) == (solved.score_hard, solved.score_soft)


def test_a_partial_seed_keeps_its_part_and_fills_the_rest(nested):
    problem = nested()
    seeded = with_initial_solution(problem, {"parcels": [{"id": "small", "box": "M"}]})
    solved = bs.solve(seeded, time_limit_seconds=0)
    # M is open through the seed, so the medium parcel uses it; the large one needs L.
    assert boxes_of(solved) == {"small": "M", "medium": "M", "large": "L"}


def test_a_seeded_solve_is_not_worse_than_its_seed(nested):
    problem = bs.generate_demo_data(max_box_types=4)
    seed = bs.solve(problem, time_limit_seconds=0)
    config = bs.SolverConfig(stop={"time_limit_seconds": 60, "step_limit": 4000})
    resumed = bs.solve(with_initial_solution(problem, seed), config=config)
    assert (resumed.score_hard, resumed.score_soft) >= (seed.score_hard, seed.score_soft)


def test_the_seed_is_used_once(nested):
    core = with_initial_solution(nested(), {"parcels": [{"id": "small", "box": "L"}]}).to_core()
    assert core.warm_start is True
    bs.solve(core, time_limit_seconds=0)
    assert core.warm_start is False


def test_parcels_are_matched_by_id_and_prior_boxes_are_replaced(nested):
    data = nested().model_dump()
    data["parcels"][1]["box"] = "L"
    problem = BoxSizingProblemModel.model_validate(data)
    seeded = with_initial_solution(problem, {"parcels": [{"id": "large", "box": "L"}, {"id": "small", "box": "S"}]})
    assert boxes_of(seeded) == {"small": "S", "medium": None, "large": "L"}


def test_the_outputs_of_the_copy_are_cleared(nested):
    solved = bs.solve(nested(), time_limit_seconds=0)
    seeded = with_initial_solution(solved, solved)
    assert (seeded.score_hard, seeded.score_soft, seeded.box_types_used, seeded.void_volume, seeded.fill_ratio) == (
        None,
        None,
        None,
        None,
        None,
    )
    assert all(box.parcels is None for box in seeded.boxes)


def test_an_unknown_parcel_raises_naming_it(nested):
    with pytest.raises(ValueError, match="unknown id in 'parcels': 'ghost'"):
        with_initial_solution(nested(), {"parcels": [{"id": "ghost", "box": "S"}]})


def test_a_repeated_parcel_raises(nested):
    with pytest.raises(ValueError, match="repeats an id in 'parcels': 'small'"):
        with_initial_solution(nested(), {"parcels": [{"id": "small", "box": "S"}, {"id": "small", "box": "M"}]})


@pytest.mark.parametrize("box", ["XL", 2, ["S"]])
def test_a_box_that_is_not_a_candidate_raises_naming_the_parcel(nested, box):
    with pytest.raises(ValueError, match="parcel 'small': box .* is not one of the candidate boxes"):
        with_initial_solution(nested(), {"parcels": [{"id": "small", "box": box}]})


@pytest.mark.parametrize("solution", [None, 3, "S", {"boxes": []}, {"parcels": "S"}])
def test_a_malformed_solution_raises(nested, solution):
    with pytest.raises(ValueError):
        with_initial_solution(nested(), solution)


def test_only_the_model_is_taken(nested):
    with pytest.raises(TypeError, match="problem must be a BoxSizingProblemModel"):
        with_initial_solution(nested().to_core(), {"parcels": []})


def test_inputs_are_not_mutated(nested):
    problem = nested()
    solution = {"parcels": [{"id": "small", "box": "M"}]}
    before = (problem.model_dump(), {"parcels": [dict(solution["parcels"][0])]})
    with_initial_solution(problem, solution)
    assert (problem.model_dump(), solution) == before
