"""A parameter that acts only through a cost needs that cost in the selection."""

import datetime

import pytest

import optimaru.bed_allocation as ba
from optimaru.bed_allocation.constraints import get_constraints
from optimaru.bed_allocation.models import BedModel, BedPlanModel, DepartmentModel, RoomModel, StayModel, parameter_uses

from .._picks import replace_default, without


def plan(gender_limitation: str = "MALE_ONLY", **stay_fields) -> BedPlanModel:
    stay = {
        "id": "s",
        "patient_age": 40,
        "arrival_date": datetime.date(2026, 1, 1),
        "departure_date": datetime.date(2026, 1, 3),
        "patient_preferred_maximum_room_capacity": 1,
        "patient_required_equipment": ["oxygen"],
        "patient_preferred_equipment": ["tv"],
        "specialty": "cardio",
    } | stay_fields
    return BedPlanModel(
        departments=[DepartmentModel(id="d", minimum_age=18, maximum_age=90, specialty_to_priority={"cardio": 1})],
        rooms=[
            RoomModel(id="r", department_id="d", capacity=2, gender_limitation=gender_limitation, equipment=["oxygen"])
        ],
        beds=[BedModel(id="b0", room_id="r"), BedModel(id="b1", room_id="r", index_in_room=1)],
        stays=[StayModel(**stay)],
    )


CHECKED = [
    ("minimum_age", "department_min_age_cost"),
    ("maximum_age", "department_max_age_cost"),
    ("patient_preferred_maximum_room_capacity", "preferred_max_room_capacity_cost"),
    ("patient_required_equipment", "required_patient_equipment_cost"),
    ("patient_preferred_equipment", "preferred_patient_equipment_cost"),
    ("gender_limitation", "female_in_male_room_cost"),
]


def spec_of(name):
    return type(next(spec for spec in get_constraints() if spec.name == name))


def test_the_default_selection_accepts_every_parameter():
    fields = {use.field for use in parameter_uses(plan())}
    assert fields >= {field for field, _ in CHECKED} | {"specialty", "specialty_to_priority", "equipment"}
    ba.solve(plan(), time_limit_seconds=0)


@pytest.mark.parametrize("field, cost", CHECKED)
def test_a_selection_without_the_cost_is_refused(field, cost):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        ba.solve(plan(), time_limit_seconds=0, constraints=without(get_constraints(), cost))


@pytest.mark.parametrize("field, cost", CHECKED)
def test_the_cost_at_weight_zero_counts(field, cost):
    ba.solve(
        plan(), time_limit_seconds=0, constraints=replace_default(get_constraints(), cost, spec_of(cost)(weight=0))
    )


@pytest.mark.parametrize(
    "limitation, cost",
    [("FEMALE_ONLY", "male_in_female_room_cost"), ("SAME_GENDER", "mixed_gender_room_cost")],
)
def test_a_gender_limitation_needs_the_cost_of_its_value(limitation, cost):
    with pytest.raises(ValueError, match=f"sets gender_limitation, but the constraints leave out {cost}"):
        ba.solve(plan(limitation), time_limit_seconds=0, constraints=without(get_constraints(), cost))
    # The cost of another value is not needed.
    ba.solve(plan(limitation), time_limit_seconds=0, constraints=without(get_constraints(), "female_in_male_room_cost"))


def test_the_two_specialty_costs_each_give_the_specialty_its_effect():
    picks = without(get_constraints(), "department_specialty_cost")
    ba.solve(plan(), time_limit_seconds=0, constraints=picks)
    with pytest.raises(ValueError, match="sets specialty, but the constraints leave out department_specialty_cost or"):
        ba.solve(plan(), time_limit_seconds=0, constraints=without(picks, "department_specialty_priority_cost"))


def test_without_the_parameters_no_cost_is_needed():
    bare = BedPlanModel(
        departments=[DepartmentModel(id="d")],
        rooms=[RoomModel(id="r", department_id="d", capacity=1)],
        beds=[BedModel(id="b0", room_id="r")],
        stays=[
            StayModel(
                id="s", patient_age=40, arrival_date=datetime.date(2026, 1, 1), departure_date=datetime.date(2026, 1, 2)
            )
        ],
    )
    assert parameter_uses(bare) == []
    ba.solve(
        bare, time_limit_seconds=0, constraints=[spec for spec in get_constraints() if spec.name == "bed_conflict_cost"]
    )
