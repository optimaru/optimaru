"""quadratic_assignment.models — the canonical JSON contract and its conversion to the engine object."""

import json
from pathlib import Path

import pytest
from pydantic import ValidationError

import quadratic_assignment as qap
from quadratic_assignment.models import ForbiddenAssignmentModel, QuadraticAssignmentProblemModel

FIXTURES = Path(__file__).parent / "fixtures"


def rich(assignment=None) -> QuadraticAssignmentProblemModel:
    """Every fact: unsorted dict orders, a facility pinned twice, a repeated forbidden row, partial
    demand / capacity maps, a direct-cost matrix with a negative entry."""
    return QuadraticAssignmentProblemModel(
        facilities=["Fa", "Fb", "Fc", "Fd"],
        locations=["Lw", "Lx", "Ly", "Lz"],
        flows=[[0, 3, 1, 2], [3, 0, 2, 0], [1, 2, 0, 4], [2, 0, 4, 1]],
        distances=[[0, 1, 4, 2], [1, 0, 2, 3], [4, 2, 0, 1], [2, 3, 1, 5]],
        assignment_costs=[[1, 2, 3, 4], [0, -1, 2, 2], [5, 5, 0, 1], [2, 2, 2, 2]],
        fixed_assignments={"Lz": "Fb", "Lw": "Fb"},
        forbidden_assignments=[
            ForbiddenAssignmentModel(facility="Fc", location="Lx"),
            ForbiddenAssignmentModel(facility="Fa", location="Lw"),
            ForbiddenAssignmentModel(facility="Fc", location="Lx"),
        ],
        facility_demands={"Fd": 6, "Fa": 0, "Fb": 2},
        location_capacities={"Ly": 3, "Lw": 0},
        assignment=assignment if assignment is not None else {"Lw": "Fd", "Lx": "Fa", "Ly": "Fb", "Lz": "Fc"},
    )


def test_to_core_maps_ids_to_indices_and_keeps_order_and_repeats():
    core = rich().to_core()
    assert isinstance(core, qap.QuadraticAssignmentProblem)
    assert [facility.id for facility in core.facilities] == ["Fa", "Fb", "Fc", "Fd"]
    assert [facility.demand for facility in core.facilities] == [0, 2, None, 6]  # key presence survives
    assert [(location.id, location.capacity) for location in core.locations] == [
        ("Lw", 0),
        ("Lx", None),
        ("Ly", 3),
        ("Lz", None),
    ]
    assert [(fixed.location, fixed.facility) for fixed in core.fixed_assignments] == [(3, 1), (0, 1)]  # dict order
    assert [(row.facility, row.location) for row in core.forbidden_assignments] == [(2, 1), (0, 0), (2, 1)]
    assert core.assignment_costs == [[1, 2, 3, 4], [0, -1, 2, 2], [5, 5, 0, 1], [2, 2, 2, 2]]
    assert core.assignment() == [3, 0, 1, 2]  # per location: Lw <- Fd, Lx <- Fa, Ly <- Fb, Lz <- Fc
    assert [facility.location for facility in core.facilities] == [1, 2, 3, 0]
    assert core.is_bijection() and core.warm_start is False


def test_from_core_round_trips_the_facts_and_fills_the_results():
    model = rich()
    core = model.to_core()
    qap.calculate_score(core)
    back = QuadraticAssignmentProblemModel.from_core(core)
    expected = model.model_dump()
    expected.update(total_cost=core.total_cost(), score_hard=core.score.hard, score_soft=core.score.soft)
    assert back.model_dump() == expected
    assert list(back.fixed_assignments) == ["Lz", "Lw"]  # input order survives
    assert len(back.forbidden_assignments) == 3  # the repeated row too
    assert list(back.assignment) == ["Lw", "Lx", "Ly", "Lz"]  # location order


def test_from_core_keeps_none_and_empty_side_data_apart():
    model = QuadraticAssignmentProblemModel(
        facilities=["A", "B"], locations=["X", "Y"], flows=[[0, 1], [1, 0]], distances=[[0, 2], [2, 0]]
    )
    back = QuadraticAssignmentProblemModel.from_core(model.to_core())
    assert back.assignment_costs is None
    assert (back.facility_demands, back.location_capacities, back.fixed_assignments) == ({}, {}, {})
    assert back.forbidden_assignments == []
    zero_matrix = model.model_copy(update={"assignment_costs": [[0, 0], [0, 0]]})
    assert QuadraticAssignmentProblemModel.from_core(zero_matrix.to_core()).assignment_costs == [[0, 0], [0, 0]]


def test_from_core_of_a_partial_assignment_leaves_the_results_empty():
    model = rich({"Lw": "Fd", "Ly": "Fb"})
    core = model.to_core()
    assert core.assignment() == [3, -1, 1, -1] and not core.is_bijection()
    back = QuadraticAssignmentProblemModel.from_core(core)
    assert back.assignment == {"Lw": "Fd", "Ly": "Fb"}
    assert (back.total_cost, back.score_hard, back.score_soft) == (None, None, None)


def test_from_core_of_a_solved_core_fills_the_results():
    core = rich().to_core()
    qap.solve(core, time_limit_seconds=0.2, random_seed=1)
    back = QuadraticAssignmentProblemModel.from_core(core)
    assert sorted(back.assignment.values()) == sorted(back.facilities)
    assert back.total_cost == core.total_cost() == core.flow_distance_cost() + core.assignment_cost()
    assert (back.score_hard, back.score_soft) == (core.score.hard, core.score.soft)
    assert back.warm_start is False


def test_the_web_demo_validates_and_converts():
    data = json.loads((FIXTURES / "web_demo.json").read_text())
    model = QuadraticAssignmentProblemModel.model_validate(data)
    core = model.to_core()
    assert core.size() == 6
    assert core.assignment() == [-1] * 6  # the sample carries no assignment
    assert QuadraticAssignmentProblemModel.from_core(core).model_dump() == model.model_dump()


def test_solving_the_model_writes_the_result_back_in_place():
    model = rich()
    model.warm_start = True
    result = qap.solve(model, time_limit_seconds=0.2, random_seed=3)
    assert (model.score_hard, model.score_soft) == (result.hard, result.soft)
    assert model.warm_start is False
    assert sorted(model.assignment) == sorted(model.locations)
    assert sorted(model.assignment.values()) == sorted(model.facilities)
    assert model.total_cost == qap.QuadraticAssignmentProblemModel.from_core(model.to_core()).total_cost


@pytest.mark.parametrize(
    "assignment, message",
    [
        ({"Lw": "Fa", "L9": "Fb"}, "unknown location: 'L9'"),
        ({"Lw": "Fa", "Lx": "F9"}, "unknown facility: 'F9'"),
        ({"Lw": "Fa", "Lx": "Fa"}, "'Fa' is assigned to both 'Lw' and 'Lx'"),
    ],
)
def test_to_core_refuses_an_assignment_it_cannot_represent(assignment, message):
    """One facility per location and one location per facility: a non-injective map raises."""
    with pytest.raises(ValueError, match=message):
        rich(assignment).to_core()


def test_model_level_scoring_needs_a_complete_assignment():
    """On a model, calculate_score / constraint_costs need every location (0.1.x raised KeyError)."""
    model = rich({"Lw": "Fd", "Lx": "Fa"})
    with pytest.raises(ValueError, match=r"missing: \['Ly', 'Lz'\]"):
        qap.calculate_score(model)
    with pytest.raises(ValueError, match=r"missing: \['Ly', 'Lz'\]"):
        qap.constraint_costs(model)
    # The engine object scores the partial state (an unassigned facility contributes nothing).
    core = model.to_core()
    raw = qap.constraint_costs(core)
    assert raw["pinned_assignments_cost"]["costs"] == [1.0, 0.0, 0.0, 1.0]  # Lw holds Fd, Lz is empty
    assert qap.calculate_score(core).hard <= -2


def test_a_plain_solve_ignores_a_valid_input_assignment_but_validates_it():
    model = rich()
    qap.solve(model, time_limit_seconds=0)
    assert model.assignment == {"Lw": "Fa", "Lx": "Fb", "Ly": "Fc", "Lz": "Fd"}  # the identity start
    bad = rich({"Lw": "Fa", "Lx": "Fa"})
    with pytest.raises(ValueError, match="assigned to both"):
        qap.solve(bad, time_limit_seconds=0)


@pytest.mark.parametrize(
    "change, message",
    [
        (lambda d: d["facilities"].append("Fe"), "same square size"),
        (lambda d: d["flows"][0].append(1), "same square size"),
        (lambda d: d["assignment_costs"].pop(), "facility-by-location square matrix"),
        (lambda d: d.update(facilities=["Fa", "Fa", "Fc", "Fd"]), "unique"),
        (lambda d: d["fixed_assignments"].update(L9="Fa"), "unknown fixed locations"),
        (lambda d: d["fixed_assignments"].update(Lx="F9"), "unknown fixed facilities"),
        (lambda d: d["forbidden_assignments"].append({"facility": "F9", "location": "Lw"}), "known facilities"),
        (lambda d: d["facility_demands"].update(F9=1), "known ids"),
        (lambda d: d["location_capacities"].update(Lx=-1), "non-negative"),
        (lambda d: d.update(max_iterations=1), "Extra inputs"),
    ],
)
def test_validators_are_unchanged(change, message):
    data = rich().model_dump()
    change(data)
    with pytest.raises(ValidationError, match=message):
        QuadraticAssignmentProblemModel.model_validate(data)


def test_negative_matrix_entries_stay_accepted():
    """0.2.0 adds no sign validation: negative flows, distances and direct costs are scored as given."""
    model = QuadraticAssignmentProblemModel(
        facilities=["A", "B"],
        locations=["X", "Y"],
        flows=[[0, -2], [1, 0]],
        distances=[[0, 3], [3, -1]],
        assignment_costs=[[-5, 0], [0, -5]],
        assignment={"X": "A", "Y": "B"},
    )
    raw = qap.constraint_costs(model)
    assert raw["flow_distance_cost"]["costs"] == [-3.0]  # 3*(-2) + 3*1 + 0
    assert raw["assignment_cost"]["costs"] == [-10.0]
    assert (qap.calculate_score(model).hard, model.score_soft, model.total_cost) == (0, 13.0, -13)


def test_the_core_validates_what_a_direct_caller_could_get_wrong():
    """A core built by hand is checked by the C++ prepare(), as ValueError."""
    core = rich().to_core()
    core.fixed_assignments = [qap.FixedAssignment(0, 1), qap.FixedAssignment(0, 2)]
    with pytest.raises(ValueError, match="fixed more than once"):
        qap.calculate_score(core)
    core = rich().to_core()
    core.forbidden_assignments = [qap.ForbiddenAssignment(7, 0)]
    with pytest.raises(ValueError, match="out of range"):
        qap.constraint_costs(core)
    core = rich().to_core()
    core.flows = [[0, 1], [1, 0]]
    with pytest.raises(ValueError, match="flows must have 4 rows"):
        core.total_cost()
    core = rich().to_core()
    core.facilities = [qap.Facility("Fa", -1), *core.facilities[1:]]
    with pytest.raises(ValueError, match="demand must be non-negative"):
        qap.calculate_score(core)
    core = rich().to_core()
    with pytest.raises(ValueError, match="'Fa' is assigned to both 'Lw' and 'Lx'"):
        core.set_assignment([0, 0, 1, 2])
    with pytest.raises(ValueError, match="out of range"):
        core.set_assignment([0, 1, 2, 4])
    with pytest.raises(ValueError, match="length"):
        core.set_assignment([0, 1])
    core.set_assignment([2, -1, 0, 1])
    assert [facility.location for facility in core.facilities] == [2, 3, 0, -1]


def test_a_fact_written_after_scoring_is_picked_up():
    """Fact setters drop the prepared caches, so the next score sees the new numbers."""
    core = rich().to_core()
    before = qap.calculate_score(core).soft
    core.flows = [[row_value * 2 for row_value in row] for row in core.flows]
    after = qap.calculate_score(core)
    assert after.soft == before - core.flow_distance_cost() / 2
    core.locations = [qap.Location(location.id, 0) for location in core.locations]
    assert qap.calculate_score(core).hard < after.hard  # Fb's demand 2 and Fd's 6 now overflow
