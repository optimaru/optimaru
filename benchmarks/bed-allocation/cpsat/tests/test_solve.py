from datetime import date

import pytest

import bed_allocation as domain
from bed_allocation.constraints import (
    UnassignedStaysCost,
    bed_conflict_cost,
    get_constraints,
    score,
)
from bed_allocation.models import (
    BedModel,
    BedPlanModel,
    DepartmentModel,
    RoomModel,
    StayModel,
)
from bed_allocation_cpsat import catalog, solve
from bed_allocation import calculate_score

MEDIUM_UNIT = 1_000_000_000  # the domain's medium fold (into soft)


def test_overlapping_stays_use_distinct_beds() -> None:
    problem = BedPlanModel(
        departments=[DepartmentModel(id="d", specialty_to_priority={"cardio": 1})],
        rooms=[RoomModel(id="r", department_id="d", capacity=2)],
        beds=[BedModel(id="a", room_id="r"), BedModel(id="b", room_id="r")],
        stays=[
            StayModel(
                id="s1",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="s2",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert solved.score_medium == 0
    assert {stay.bed_id for stay in solved.stays} == {"a", "b"}
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_medium, solved.score_soft) == (
        shared.hard,
        shared.medium,
        shared.soft,
    )


def test_bed_conflict_constraint_can_be_removed() -> None:
    problem = BedPlanModel(
        departments=[DepartmentModel(id="d", specialty_to_priority={"cardio": 1})],
        rooms=[RoomModel(id="r", department_id="d", capacity=1)],
        beds=[BedModel(id="a", room_id="r")],
        stays=[
            StayModel(
                id="s1",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="s2",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
        ],
    )

    solved = solve(problem, time_limit_seconds=2, constraints=["unassigned_stays_cost"])

    assert [stay.bed_id for stay in solved.stays] == ["a", "a"]
    assert solved.score_hard == 0


def _parity_problem() -> BedPlanModel:
    return BedPlanModel(
        departments=[
            DepartmentModel(
                id="d",
                specialty_to_priority={"cardio": 2},
                minimum_age=18,
                maximum_age=65,
            )
        ],
        rooms=[
            RoomModel(
                id="same",
                department_id="d",
                capacity=2,
                gender_limitation="SAME_GENDER",
                equipment=["telemetry"],
            ),
            RoomModel(
                id="male",
                department_id="d",
                capacity=1,
                gender_limitation="MALE_ONLY",
            ),
            RoomModel(
                id="female",
                department_id="d",
                capacity=1,
                gender_limitation="FEMALE_ONLY",
            ),
        ],
        beds=[
            BedModel(id="s0", room_id="same"),
            BedModel(id="s1", room_id="same"),
            BedModel(id="m", room_id="male"),
            BedModel(id="f", room_id="female"),
        ],
        stays=[
            StayModel(
                id="a",
                patient_gender="FEMALE",
                patient_age=10,
                patient_preferred_maximum_room_capacity=1,
                patient_required_equipment=["telemetry", "oxygen"],
                patient_preferred_equipment=["television"],
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="b",
                patient_gender="MALE",
                patient_age=80,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="missing",
            ),
            StayModel(
                id="c",
                patient_gender="FEMALE",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="d",
                patient_gender="MALE",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="e",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
        ],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_a_fixed_plan(constraint) -> None:
    problem = _parity_problem()
    fixed = [0, 0, 2, 3, len(problem.beds)]
    candidate = problem.model_copy(deep=True)
    for stay, bed in zip(candidate.stays, fixed, strict=True):
        stay.bed_id = None if bed == len(problem.beds) else candidate.beds[bed].id
    core = candidate.to_core()
    core.set_assignment(fixed)
    expected = -sum(domain.constraint_costs(core)[constraint.name]["costs"])
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_beds=fixed,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """The plugins restate no default: the catalog is the engine's constraint_info()."""
    engine = {row.name: row for row in domain.constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == domain.list_constraints()
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == "either"  # every bed-allocation constraint is flippable
        assert entry["params"] == {"weight": None, "level": None, "exponent": 1.0}
    # The medium constraint: weight 1 in medium units on both solvers (the engine
    # used to carry 10^9 as a soft weight).
    assert engine["unassigned_stays_cost"].default_level == "medium"
    unassigned = next(e for e in entries if e["name"] == "unassigned_stays_cost")
    assert (unassigned["default_level"], unassigned["default_weight"]) == ("medium", 1)


def _crowded_problem() -> BedPlanModel:
    """Two overlapping 2-night stays and one bed: one stay must go without a bed."""
    return BedPlanModel(
        departments=[DepartmentModel(id="d", specialty_to_priority={"cardio": 1})],
        rooms=[RoomModel(id="r", department_id="d", capacity=1)],
        beds=[BedModel(id="a", room_id="r")],
        stays=[
            StayModel(
                id=f"s{index}",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            )
            for index in range(2)
        ],
    )


def _engine_score(solved: BedPlanModel, selection) -> tuple[int, float]:
    """The engine-level (hard, soft) of CP-SAT's assignment under `selection`."""
    core = solved.to_core()
    no_bed = core.no_bed_value()
    ids = [bed.id for bed in solved.beds]
    core.set_assignment(
        [
            no_bed if stay.bed_id is None else ids.index(stay.bed_id)
            for stay in solved.stays
        ]
    )
    return score(domain.constraint_costs(core), selection)


def test_default_selection_scores_like_the_engine() -> None:
    """At the defaults (medium unassigned_stays_cost) the engine scores CP-SAT's
    assignment the same, the medium level folded into soft."""
    solved = solve(_crowded_problem(), time_limit_seconds=2)
    levels = (solved.score_hard, solved.score_medium, solved.score_soft)
    assert levels == (0, -2, 0)  # one 2-night stay without a bed
    engine = _engine_score(solved, get_constraints())
    assert engine == (levels[0], levels[2] + levels[1] * MEDIUM_UNIT)


def test_demoting_the_medium_constraint_scores_like_the_engine() -> None:
    """level="soft" moves unassigned_stays_cost to soft on BOTH solvers (CP-SAT used to
    keep a demoted medium term on medium)."""
    selection = [bed_conflict_cost, UnassignedStaysCost(level="soft")]
    solved = solve(_crowded_problem(), time_limit_seconds=2, constraints=selection)
    assert (solved.score_hard, solved.score_medium, solved.score_soft) == (0, 0, -2)
    assert _engine_score(solved, selection) == (solved.score_hard, solved.score_soft)


def test_promoting_the_medium_constraint_scores_like_the_engine() -> None:
    """level="hard": one hard unit per night without a bed, still cheaper than sharing."""
    selection = [bed_conflict_cost, UnassignedStaysCost(level="hard")]
    solved = solve(_crowded_problem(), time_limit_seconds=2, constraints=selection)
    assert (solved.score_hard, solved.score_medium, solved.score_soft) == (-2, 0, 0)
    assert _engine_score(solved, selection) == (solved.score_hard, solved.score_soft)
