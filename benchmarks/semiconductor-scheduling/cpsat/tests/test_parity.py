"""CP-SAT vs the C++ engine: one semantics per constraint.

For every constraint, on hand-made fixtures with every operation fixed (``_fixed_assignments``)
and only that constraint selected:

- a PENALIZED plugin's objective term (``_proxy_score``) equals the engine's raw cost;
- a STRUCTURAL plugin accepts the fixed schedule exactly when the engine's raw cost is 0
  (and contributes 0); it rejects it (infeasible) when the raw cost is positive.

Plus: the catalog equals constraint_info(), demotions score like the engine, and the
structural plugins refuse what they cannot honour.
"""

from __future__ import annotations

import pytest
import semiconductor_scheduling as domain
from semiconductor_scheduling.constraints import (
    CycleTimeCost,
    Level,
    LotRoutePrecedenceCost,
    MachineCapacityCost,
    MachineEligibilityCost,
    MachineLoadImbalanceCost,
    MachinePreferenceCost,
    MakespanCost,
    MaxCycleTimeCost,
    MaxQueueTimeCost,
    MinBatchSizeCost,
    WeightedTardinessCost,
    get_constraints,
    score,
)
from semiconductor_scheduling.models import (
    AssignmentModel,
    AuxiliaryResourceModel,
    LotModel,
    MachineModel,
    MachineOptionModel,
    OperationModel,
    SemiconductorScheduleModel,
    SetupTimeModel,
    TimeWindowModel,
    TransportTimeModel,
)

from semiconductor_scheduling_cpsat import catalog, solve

STRUCTURAL = {
    "machine_eligibility_cost",
    "lot_route_precedence_cost",
    "machine_capacity_cost",
    "batch_capacity_cost",
    "batch_family_conflict_cost",
    "missing_setup_time_cost",
    "machine_unavailability_cost",
    "min_queue_time_cost",
    "missing_transport_time_cost",
    "auxiliary_resource_capacity_cost",
    "auxiliary_resource_unavailability_cost",
    "pinned_assignments_cost",
}


def _a(lot, op, machine, start, end):
    return AssignmentModel(lot_id=lot, operation_id=op, machine_id=machine, start=start, end=end)


def _fab(assignments) -> SemiconductorScheduleModel:
    """tests/test_semiconductor_scheduling.py::_fab_problem."""
    return SemiconductorScheduleModel(
        machines=[
            MachineModel(id="M1", group="PHOTO", unavailable=[TimeWindowModel(start=3, end=5)]),
            MachineModel(id="M2", group="PHOTO"),
            MachineModel(id="F1", group="DIFF", batch_capacity=2, minimum_batch_size=2),
        ],
        setup_times=[SetupTimeModel(machine_group="PHOTO", from_family="X", to_family="Y", duration=3)],
        transport_times=[TransportTimeModel(from_group="PHOTO", to_group="DIFF", duration=3)],
        lots=[
            LotModel(
                id="L1",
                product="P1",
                release_time=2,
                due_time=12,
                priority=2,
                operations=[
                    OperationModel(
                        id="PHOTO",
                        family="X",
                        max_queue_time=3,
                        eligible_machines=[MachineOptionModel(machine_id="M1", duration=4, preference_cost=1)],
                    ),
                    OperationModel(
                        id="DIFF", family="X", eligible_machines=[MachineOptionModel(machine_id="F1", duration=5)]
                    ),
                ],
            ),
            LotModel(
                id="L2",
                product="P2",
                due_time=30,
                operations=[
                    OperationModel(
                        id="PHOTO", family="Y", eligible_machines=[MachineOptionModel(machine_id="M1", duration=4)]
                    ),
                    OperationModel(
                        id="DIFF", family="Y", eligible_machines=[MachineOptionModel(machine_id="F1", duration=5)]
                    ),
                ],
            ),
            LotModel(
                id="L3",
                product="P1",
                due_time=30,
                operations=[
                    OperationModel(
                        id="DIFF", family="X", eligible_machines=[MachineOptionModel(machine_id="F1", duration=5)]
                    )
                ],
            ),
        ],
        assignments=assignments,
    )


FAB = [
    _a("L1", "PHOTO", "M1", 0, 4),
    _a("L1", "DIFF", "F1", 10, 15),
    _a("L2", "PHOTO", "M1", 2, 6),
    _a("L2", "DIFF", "F1", 11, 16),
    _a("L3", "DIFF", "F1", 12, 17),
]
FAB_TRANSPORT = [*FAB[:1], _a("L1", "DIFF", "F1", 6, 11), *FAB[2:]]


def _resources(assignments) -> SemiconductorScheduleModel:
    """tests/test_semiconductor_scheduling.py::test_fab_resource_pin_and_cycle_constraints_..."""
    return SemiconductorScheduleModel(
        machines=[MachineModel(id="M1", group="G1"), MachineModel(id="M2", group="G2")],
        auxiliary_resources=[
            AuxiliaryResourceModel(id="RETICLE", capacity=1, unavailable=[TimeWindowModel(start=3, end=5)])
        ],
        lots=[
            LotModel(
                id="L1",
                product="P1",
                due_time=20,
                max_cycle_time=6,
                operations=[
                    OperationModel(
                        id="O1",
                        family="F",
                        min_queue_time=4,
                        required_resources=["RETICLE"],
                        eligible_machines=[MachineOptionModel(machine_id="M1", duration=4)],
                    ),
                    OperationModel(
                        id="O2", family="F", eligible_machines=[MachineOptionModel(machine_id="M2", duration=2)]
                    ),
                ],
            ),
            LotModel(
                id="L2",
                product="P2",
                due_time=20,
                operations=[
                    OperationModel(
                        id="O1",
                        family="F",
                        required_resources=["RETICLE"],
                        eligible_machines=[MachineOptionModel(machine_id="M1", duration=4)],
                    )
                ],
            ),
        ],
        pinned_assignments=[_a("L1", "O1", "M1", 1, 5)],
        assignments=assignments,
    )


RESOURCES = [_a("L1", "O1", "M1", 0, 4), _a("L1", "O2", "M2", 6, 8), _a("L2", "O1", "M1", 2, 6)]
# The same instance, feasible: the pin kept, the reticle free, every rule met.
RESOURCES_ZERO = [_a("L1", "O1", "M1", 1, 5), _a("L1", "O2", "M2", 9, 11), _a("L2", "O1", "M1", 5, 9)]


def _zero(assignments) -> SemiconductorScheduleModel:
    """A feasible schedule on which every term but cycle time and makespan is 0."""
    return SemiconductorScheduleModel(
        machines=[
            MachineModel(id="M1", group="G1", unavailable=[TimeWindowModel(start=20, end=25)]),
            MachineModel(id="M2", group="G1"),
            MachineModel(id="F1", group="G2", batch_capacity=2, minimum_batch_size=2),
        ],
        auxiliary_resources=[
            AuxiliaryResourceModel(id="R", capacity=1, unavailable=[TimeWindowModel(start=30, end=35)])
        ],
        setup_times=[SetupTimeModel(machine_group="G1", from_family="A", to_family="B", duration=2)],
        transport_times=[TransportTimeModel(from_group="G1", to_group="G2", duration=1)],
        lots=[
            LotModel(
                id="L1",
                product="P",
                due_time=50,
                max_cycle_time=40,
                operations=[
                    OperationModel(
                        id="O1",
                        family="A",
                        min_queue_time=1,
                        max_queue_time=10,
                        required_resources=["R"],
                        eligible_machines=[MachineOptionModel(machine_id="M1", duration=4)],
                    ),
                    OperationModel(
                        id="O2", family="X", eligible_machines=[MachineOptionModel(machine_id="F1", duration=5)]
                    ),
                ],
            ),
            LotModel(
                id="L2",
                product="P",
                due_time=50,
                operations=[
                    OperationModel(
                        id="O1", family="B", eligible_machines=[MachineOptionModel(machine_id="M1", duration=3)]
                    ),
                    OperationModel(
                        id="O2", family="X", eligible_machines=[MachineOptionModel(machine_id="F1", duration=5)]
                    ),
                ],
            ),
            LotModel(
                id="L3",
                product="P",
                due_time=50,
                operations=[
                    OperationModel(
                        id="O1", family="B", eligible_machines=[MachineOptionModel(machine_id="M2", duration=7)]
                    )
                ],
            ),
        ],
        pinned_assignments=[_a("L1", "O1", "M1", 0, 4)],
        assignments=assignments,
    )


ZERO = [
    _a("L1", "O1", "M1", 0, 4),
    _a("L2", "O1", "M1", 6, 9),
    _a("L1", "O2", "F1", 10, 15),
    _a("L2", "O2", "F1", 10, 15),
    _a("L3", "O1", "M2", 0, 7),
]
# The eligibility positive: L3 runs 6 on M2 where it takes 7.
ZERO_WRONG_DURATION = [*ZERO[:4], _a("L3", "O1", "M2", 0, 6)]

FIXTURES = {
    "fab": (_fab, FAB),
    "fab-transport": (_fab, FAB_TRANSPORT),
    "resources": (_resources, RESOURCES),
    "resources-zero": (_resources, RESOURCES_ZERO),
    "zero": (_zero, ZERO),
    "zero-wrong-duration": (_zero, ZERO_WRONG_DURATION),
}


def _raw(build, assignments, name: str) -> float:
    return domain.constraint_costs(build(assignments))[name]["costs"][0]


@pytest.mark.parametrize("fixture", sorted(FIXTURES))
@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_the_engine_on_fixed_assignments(constraint, fixture) -> None:
    build, assignments = FIXTURES[fixture]
    problem = build([])
    expected = _raw(build, assignments, constraint.name)
    proxy: dict[str, int] = {}
    selection = [{"name": constraint.name}]
    if fixture == "zero-wrong-duration" and constraint.name != "machine_eligibility_cost":
        pytest.skip("CP-SAT cannot represent an ineligible assignment: only eligibility is checked on it")
    if constraint.name in STRUCTURAL:
        if expected > 0:
            with pytest.raises(RuntimeError, match="INFEASIBLE"):
                solve(problem, time_limit_seconds=5, constraints=selection, _fixed_assignments=assignments)
            return
        solve(problem, time_limit_seconds=5, constraints=selection, _fixed_assignments=assignments, _proxy_score=proxy)
        assert proxy == {"hard": 0, "soft": 0}
        return
    solve(problem, time_limit_seconds=5, constraints=selection, _fixed_assignments=assignments, _proxy_score=proxy)
    level = "hard" if constraint.name in {"max_queue_time_cost", "max_cycle_time_cost"} else "soft"
    weight = 10 if constraint.name == "weighted_tardiness_cost" else 1
    assert proxy[level] == -weight * expected
    assert proxy["soft" if level == "hard" else "hard"] == 0


def test_every_constraint_has_a_positive_and_a_zero_fixture() -> None:
    """The fixtures above cover both sides of every raw cost (cycle time and makespan are > 0
    whenever every operation is assigned, which CP-SAT always does)."""
    for constraint in get_constraints():
        values = [_raw(build, assignments, constraint.name) for build, assignments in FIXTURES.values()]
        assert any(value > 0 for value in values), constraint.name
        if constraint.name not in {"cycle_time_cost", "makespan_cost"}:
            assert any(value == 0 for value in values), constraint.name


def test_cpsat_defaults_are_the_engine_defaults() -> None:
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
        assert entry["kind"] == "either"  # every constraint is flippable in the engine
        assert entry["params"] == {"weight": None, "level": None, "exponent": 1.0, "offset": 0.0, "clip": True}


def test_demoting_max_queue_time_scores_like_the_engine() -> None:
    problem = _fab([])
    selection = [MaxQueueTimeCost(level="soft"), WeightedTardinessCost(weight=2), CycleTimeCost()]
    solved = solve(problem, time_limit_seconds=5, constraints=selection, _fixed_assignments=FAB)
    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert solved.score_hard == 0 and solved.score_soft == -(3 + 2 * 6 + 46)


@pytest.mark.parametrize("knobs", [{"level": "soft"}, {"weight": 2}, {"weight": 0}])
def test_structural_plugins_refuse_what_they_cannot_honour(knobs) -> None:
    with pytest.raises(ValueError, match="enforces this constraint structurally"):
        solve(_fab([]), time_limit_seconds=1, constraints=[MachineCapacityCost(**knobs)])


def test_a_cpsat_optimum_rescored_by_the_engine_is_hard_feasible() -> None:
    solved = solve(_zero([]), time_limit_seconds=5)
    assert solved.score_hard == 0
    assert (solved.score_hard, solved.score_soft) == tuple(domain.calculate_score(solved.to_core()))
    raw = domain.constraint_costs(solved)
    assert all(raw[name]["costs"] == [0.0] for name in STRUCTURAL)


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------

SELECTIONS = {
    "repeat": [MakespanCost(), MakespanCost(weight=2), CycleTimeCost(), CycleTimeCost()],
    "repeat-literal": [
        MakespanCost(offset=20, clip=False),
        MakespanCost(weight=3, offset=-2),
        WeightedTardinessCost(offset=2, clip=False),
        MachinePreferenceCost(offset=1, clip=False),
    ],
    "hard-offsets": [
        MaxQueueTimeCost(offset=1),
        MaxQueueTimeCost(weight=2, offset=5, clip=False),
        MaxCycleTimeCost(offset=1, clip=False),
        MinBatchSizeCost(level="hard", offset=1),
        MachineLoadImbalanceCost(offset=-1, clip=False),
    ],
    "with-structural": [
        MachineEligibilityCost(),
        LotRoutePrecedenceCost(),
        LotRoutePrecedenceCost(),
        MakespanCost(offset=10),
        CycleTimeCost(offset=5, clip=False),
    ],
    # Each penalized pick on its own level (a structural rule cannot move): a soft literal
    # repeat of the hard queue limit, the makespan on both levels, the tardiness and the
    # load imbalance promoted to hard, the cycle-time limit demoted to soft.
    "levels": [
        MaxQueueTimeCost(),
        MaxQueueTimeCost(level="soft", weight=3, offset=1, clip=False),
        MakespanCost(),
        MakespanCost(level=Level.HARD, offset=20),
        WeightedTardinessCost(level="hard", weight=1, offset=2, clip=False),
        CycleTimeCost(level=Level.SOFT, offset=5),
        MaxCycleTimeCost(level="soft", weight=2, offset=-1),
        MachineLoadImbalanceCost(level="hard", offset=1),
    ],
}


@pytest.mark.parametrize("fixture", sorted(set(FIXTURES) - {"zero-wrong-duration"}))
@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_match_the_engine_on_fixed_assignments(selection, fixture) -> None:
    """Every operation fixed: the CP-SAT objective of a selection with repeats and offsets is
    the engine's score of that schedule under the same picks (the C++ assembly's and the
    score() mirror's), exactly, per level. A selected structural rule the schedule breaks
    makes the model infeasible instead."""
    build, assignments = FIXTURES[fixture]
    core = build(assignments).to_core()
    raw = domain.constraint_costs(core)
    if any(spec.name in STRUCTURAL and raw[spec.name]["costs"][0] > 0 for spec in selection):
        with pytest.raises(RuntimeError, match="INFEASIBLE"):
            solve(build([]), time_limit_seconds=5, constraints=selection, _fixed_assignments=assignments)
        return
    proxy: dict[str, int] = {}
    solve(build([]), time_limit_seconds=5, constraints=selection, _fixed_assignments=assignments, _proxy_score=proxy)
    engine = score(domain.constraint_costs(core, selection), selection)
    own = domain.calculate_score(core, selection)  # the C++ assembly of the same picks
    assert (own.hard, own.soft) == (engine.hard, engine.soft)
    assert proxy == {"hard": engine.hard, "soft": engine.soft}
    assert score(raw, selection) == engine  # the default costs score the repeats too


def test_the_fixtures_exercise_the_selections() -> None:
    """At least one fixture keeps the structural picks feasible and every offset bites."""
    build, assignments = FIXTURES["zero"]
    raw = domain.constraint_costs(build(assignments).to_core())
    assert raw["lot_route_precedence_cost"]["costs"] == [0.0]
    assert raw["makespan_cost"]["costs"][0] > 10 and raw["cycle_time_cost"]["costs"][0] > 0


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_a_solve_with_repeats_and_offsets_reports_the_engine_score(selection) -> None:
    proxy: dict[str, int] = {}
    solved = solve(_zero([]), time_limit_seconds=10, constraints=selection, _proxy_score=proxy)
    engine = score(domain.constraint_costs(solved.to_core(), selection), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert (solved.score_hard, solved.score_soft) == (proxy["hard"], proxy["soft"])


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, level, offset, clip} dicts pick the same instances as specs."""
    specs = [MakespanCost(), MakespanCost(weight=3, offset=4, clip=False), MakespanCost(level="hard", offset=15)]
    dicts = [
        {"name": "makespan_cost"},
        {"name": "makespan_cost", "weight": 3, "offset": 4, "clip": False},
        {"name": "makespan_cost", "level": "hard", "offset": 15},
    ]
    by_specs = solve(_fab([]), time_limit_seconds=5, constraints=specs, _fixed_assignments=FAB)
    by_dicts = solve(_fab([]), time_limit_seconds=5, constraints=dicts, _fixed_assignments=FAB)
    assert (by_dicts.score_hard, by_dicts.score_soft) == (by_specs.score_hard, by_specs.score_soft)
    # The hard repeat charges the makespan 17 beyond 15.
    assert (by_specs.score_hard, by_specs.score_soft) == (-(17 - 15), -(17 + 3 * (17 - 4)))


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_like_the_engine(level) -> None:
    """The domain has no medium level: a penalized pick is refused in the engine's words, a
    repeat too; a structural rule refuses any level but hard anyway."""
    message = "constraint 'makespan_cost': this use case has no medium level"
    for picks in (
        [MakespanCost(level=level)],
        [MakespanCost(), MakespanCost(level=level)],
        [{"name": "makespan_cost"}, {"name": "makespan_cost", "level": "medium"}],
    ):
        with pytest.raises(ValueError, match=message):
            solve(_fab([]), time_limit_seconds=1, constraints=picks)
    with pytest.raises(ValueError, match="enforces this constraint structurally"):
        solve(_fab([]), time_limit_seconds=1, constraints=[MachineCapacityCost(level=level)])


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(_fab([]), time_limit_seconds=1, constraints=[MakespanCost(offset=0.5)])


def test_structural_plugins_refuse_an_offset_and_check_every_instance() -> None:
    """A structural plugin has no term to shape: the registry refuses its offset. A repeated
    structural pick is checked with its own knobs."""
    with pytest.raises(ValueError, match="'machine_capacity_cost' offset is not supported by this CP-SAT model"):
        solve(_fab([]), time_limit_seconds=1, constraints=[MachineCapacityCost(offset=1)])
    with pytest.raises(ValueError, match="enforces this constraint structurally"):
        solve(_fab([]), time_limit_seconds=1, constraints=[MachineCapacityCost(), MachineCapacityCost(weight=2)])
