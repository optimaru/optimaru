"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.conference_scheduling as cs
from optimaru.conference_scheduling.constraints import get_constraints
from optimaru.conference_scheduling.models import ConferenceScheduleModel, parameter_uses

from .._picks import replace_default, without


def schedule() -> ConferenceScheduleModel:
    data = ConferenceScheduleModel.from_core(cs.generate_demo_data()).model_dump()
    talk = data["talks"][0]
    talk |= {
        "sector_tags": ["finance"],
        "language": "de",
        "crowd_control_risk": 1,
        "prerequisite_talks": [1],
        "required_timeslot_tags": ["morning"],
    }
    data["rooms"][0]["unavailable_timeslots"] = [0]
    data["minimum_consecutive_talks_pause_in_minutes"] = 15
    return ConferenceScheduleModel.model_validate(data)


CHECKED = [
    ("sector_tags", "sector_conflict_cost"),
    ("language", "language_diversity_cost"),
    ("crowd_control_risk", "crowd_control_risk_cost"),
    ("prerequisite_talks", "talk_prerequisite_talks_cost"),
    ("required_timeslot_tags", "talk_required_timeslot_tags_cost"),
    ("unavailable_timeslots", "room_unavailability_cost"),
    ("minimum_consecutive_talks_pause_in_minutes", "consecutive_talks_pause_cost"),
]


def spec_of(name):
    return type(next(spec for spec in get_constraints() if spec.name == name))


def test_the_default_selection_accepts_every_parameter():
    assert {use.field for use in parameter_uses(schedule())} >= {field for field, _ in CHECKED}
    cs.solve(schedule(), time_limit_seconds=0)


@pytest.mark.parametrize("field, cost", CHECKED)
def test_a_selection_without_the_cost_is_refused(field, cost):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        cs.solve(schedule(), time_limit_seconds=0, constraints=without(get_constraints(), cost))


@pytest.mark.parametrize("field, cost", CHECKED)
def test_the_cost_at_weight_zero_counts(field, cost):
    cs.solve(
        schedule(), time_limit_seconds=0, constraints=replace_default(get_constraints(), cost, spec_of(cost)(weight=0))
    )


def test_the_default_pause_needs_no_cost_on_its_own():
    data = schedule().model_dump()
    data["minimum_consecutive_talks_pause_in_minutes"] = 30
    model = ConferenceScheduleModel.model_validate(data)
    assert "minimum_consecutive_talks_pause_in_minutes" not in {use.field for use in parameter_uses(model)}
    # The speakers still give the pause cost an effect, but so do the other speaker costs.
    cs.solve(model, time_limit_seconds=0, constraints=without(get_constraints(), "consecutive_talks_pause_cost"))


def test_an_engine_object_is_not_checked():
    cs.solve(schedule().to_core(), time_limit_seconds=0, constraints=without(get_constraints(), "sector_conflict_cost"))
