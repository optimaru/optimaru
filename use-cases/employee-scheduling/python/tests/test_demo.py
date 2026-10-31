"""es_scheduling.demo: the sample request is deterministic and solvable."""

import pytest

pytest.importorskip("pydantic")

from es_scheduling.demo import generate_demo_data  # noqa: E402
from es_scheduling.models import ScheduleRequest, solve_request  # noqa: E402


def test_the_same_arguments_give_the_same_request():
    assert generate_demo_data() == generate_demo_data()
    assert generate_demo_data(seed=11, workers=8, days=7) == generate_demo_data(seed=11, workers=8, days=7)


def test_another_seed_gives_another_request():
    assert generate_demo_data(seed=1) != generate_demo_data(seed=2)


def test_the_request_is_the_public_model_and_survives_json():
    request = generate_demo_data()
    assert isinstance(request, ScheduleRequest)
    assert ScheduleRequest.model_validate_json(request.model_dump_json()) == request


def test_the_default_size():
    model_input = generate_demo_data().model_input
    assert len(model_input.workers) == 12
    assert len(model_input.shifts) == 14 * 6
    assert model_input.workers[0].id == "Worker 01"
    assert {shift.location for shift in model_input.shifts} == {"Location A", "Location B"}
    assert all(shift.worker is None for shift in model_input.shifts)


def test_the_size_arguments_apply():
    model_input = generate_demo_data(workers=20, days=7).model_input
    assert len(model_input.workers) == 20
    assert len(model_input.shifts) == 7 * 10


@pytest.mark.parametrize("seed", [7, 1, 2, 3, 4, 5])
def test_it_solves_to_hard_zero(seed):
    request = generate_demo_data(seed=seed)
    request.settings.stop_at_first_feasible = True
    solution = solve_request(request)
    assert solution.score_hard == 0
    assert solution.feasible
    assert solution.unassigned == []


@pytest.mark.parametrize("workers, days", [(0, 14), (12, 0)])
def test_an_empty_roster_is_refused(workers, days):
    with pytest.raises(ValueError, match="must be positive"):
        generate_demo_data(workers=workers, days=days)
