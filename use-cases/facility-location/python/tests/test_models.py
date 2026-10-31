"""pydantic mirror round-trip tests (facility_location.models)."""

import pytest

import facility_location as fl

models = pytest.importorskip("facility_location.models")


def sample_problem():
    facilities = [
        fl.Facility("F0", fl.Location(51.5, -0.1), 50000, 150),
        fl.Facility("F1", fl.Location(51.45, -0.05), 30000, 150),
    ]
    consumers = [fl.Consumer("C0", fl.Location(51.52, -0.12), 15)]
    problem = fl.FacilityLocationProblem(facilities, consumers, fl.Location(51.44, -0.16), fl.Location(51.56, -0.01))
    problem.set_assignment([1])
    fl.calculate_score(problem)
    return problem


def test_json_round_trip_preserves_everything():
    problem = sample_problem()
    model = models.FacilityLocationProblemModel.from_core(problem)
    clone = models.FacilityLocationProblemModel.model_validate_json(model.model_dump_json())
    assert clone == model

    rebuilt = clone.to_core()
    assert rebuilt.assignment() == problem.assignment()
    assert [f.id for f in rebuilt.facilities] == [f.id for f in problem.facilities]
    assert rebuilt.facilities[1].setup_cost == 30000
    assert rebuilt.south_west_corner.latitude == 51.44
    assert rebuilt.distance_from_facility_weight == 6

    # The score fields captured the solved values...
    assert model.score_hard == problem.score.hard
    assert model.score_soft == problem.score.soft
    # ...and the rebuilt problem recomputes to the same score.
    recomputed = fl.calculate_score(rebuilt)
    assert (recomputed.hard, recomputed.soft) == (problem.score.hard, problem.score.soft)


def test_weight_overrides_survive_round_trip():
    problem = sample_problem()
    problem.distance_from_facility_weight = 5
    problem.setup_cost_weight = 3
    model = models.FacilityLocationProblemModel.from_core(problem)
    rebuilt = model.to_core()
    assert rebuilt.distance_from_facility_weight == 5
    assert rebuilt.setup_cost_weight == 3


def test_extra_fields_are_rejected():
    from pydantic import ValidationError

    with pytest.raises(ValidationError):
        models.LocationModel(latitude=0.0, longitude=0.0, altitude=1.0)


def _dump_with_facility(value):
    data = models.FacilityLocationProblemModel.from_core(sample_problem()).model_dump()
    data["consumers"][0]["facility"] = value
    return data


@pytest.mark.parametrize("value", [-2, 2, 5, 99999999])
def test_a_facility_out_of_range_is_refused_before_the_engine(value):
    with pytest.raises(ValueError, match=rf"consumer 'C0': facility {value} is out of range -1\.\.1"):
        models.FacilityLocationProblemModel.model_validate(_dump_with_facility(value))


@pytest.mark.parametrize("value", [-1, 0, 1])
def test_every_facility_in_range_is_accepted(value):
    model = models.FacilityLocationProblemModel.model_validate(_dump_with_facility(value))
    assert model.to_core().consumers[0].facility == value


def test_a_facility_edited_after_validation_is_refused_by_to_core():
    model = models.FacilityLocationProblemModel.from_core(sample_problem())
    model.consumers[0].facility = 99999999
    with pytest.raises(ValueError, match="out of range"):
        model.to_core()
