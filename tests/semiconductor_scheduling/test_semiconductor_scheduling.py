from optimaru.semiconductor_scheduling import constraint_costs as fab_costs
from optimaru.semiconductor_scheduling import constraint_items as fab_items
from optimaru.semiconductor_scheduling import solve as solve_fab
from optimaru.semiconductor_scheduling.models import (
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


# Also the small problem of tests/_problems.py.
def _fab_problem(*, with_assignments: bool) -> SemiconductorScheduleModel:
    lots = [
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
                    id="DIFF",
                    family="X",
                    eligible_machines=[MachineOptionModel(machine_id="F1", duration=5)],
                ),
            ],
        ),
        LotModel(
            id="L2",
            product="P2",
            release_time=0,
            due_time=30,
            operations=[
                OperationModel(
                    id="PHOTO",
                    family="Y",
                    eligible_machines=[MachineOptionModel(machine_id="M1", duration=4)],
                ),
                OperationModel(
                    id="DIFF",
                    family="Y",
                    eligible_machines=[MachineOptionModel(machine_id="F1", duration=5)],
                ),
            ],
        ),
        LotModel(
            id="L3",
            product="P1",
            release_time=0,
            due_time=30,
            operations=[
                OperationModel(
                    id="DIFF",
                    family="X",
                    eligible_machines=[MachineOptionModel(machine_id="F1", duration=5)],
                )
            ],
        ),
    ]
    assignments = []
    if with_assignments:
        assignments = [
            AssignmentModel(lot_id="L1", operation_id="PHOTO", machine_id="M1", start=0, end=4),
            AssignmentModel(lot_id="L1", operation_id="DIFF", machine_id="F1", start=10, end=15),
            AssignmentModel(lot_id="L2", operation_id="PHOTO", machine_id="M1", start=2, end=6),
            AssignmentModel(lot_id="L2", operation_id="DIFF", machine_id="F1", start=11, end=16),
            AssignmentModel(lot_id="L3", operation_id="DIFF", machine_id="F1", start=12, end=17),
        ]
    return SemiconductorScheduleModel(
        machines=[
            MachineModel(id="M1", group="PHOTO", unavailable=[TimeWindowModel(start=3, end=5)]),
            MachineModel(id="M2", group="PHOTO"),
            MachineModel(id="F1", group="DIFF", batch_capacity=2, minimum_batch_size=2),
        ],
        setup_times=[SetupTimeModel(machine_group="PHOTO", from_family="X", to_family="Y", duration=3)],
        transport_times=[TransportTimeModel(from_group="PHOTO", to_group="DIFF", duration=3)],
        lots=lots,
        assignments=assignments,
    )


def _raw(costs: dict, name: str) -> float:
    """A constraint's raw total (every constraint is global: one element)."""
    return sum(costs[name]["costs"])


def test_semiconductor_constraints_match_manual_calculation():
    costs = fab_costs(_fab_problem(with_assignments=True))
    assert _raw(costs, "machine_eligibility_cost") == 0
    assert _raw(costs, "lot_route_precedence_cost") == 2
    assert _raw(costs, "machine_capacity_cost") == 2
    assert _raw(costs, "batch_capacity_cost") == 3
    assert _raw(costs, "batch_family_conflict_cost") == 8
    assert _raw(costs, "missing_setup_time_cost") == 5
    assert _raw(costs, "machine_unavailability_cost") == 3
    assert _raw(costs, "max_queue_time_cost") == 3
    assert _raw(costs, "missing_transport_time_cost") == 0
    assert _raw(costs, "machine_preference_cost") == 1
    assert _raw(costs, "min_batch_size_cost") == 3
    assert _raw(costs, "machine_load_imbalance_cost") == 8
    assert _raw(costs, "weighted_tardiness_cost") == 6
    assert _raw(costs, "cycle_time_cost") == 46
    assert costs["makespan_cost"]["costs"] == [17]
    assert fab_items(_fab_problem(with_assignments=True))["makespan_cost"] == [17]


def test_semiconductor_missing_transport_time_matches_manual_shortfall():
    problem = _fab_problem(with_assignments=True)
    problem.assignments[1].start = 6
    problem.assignments[1].end = 11
    assert _raw(fab_costs(problem), "missing_transport_time_cost") == 1


def test_semiconductor_meta_builds_a_feasible_reentrant_schedule():
    problem = _fab_problem(with_assignments=False)
    problem = solve_fab(problem, time_limit_seconds=0.05, random_seed=3)
    costs = fab_costs(problem)
    for name in (
        "machine_eligibility_cost",
        "lot_route_precedence_cost",
        "machine_capacity_cost",
        "batch_capacity_cost",
        "batch_family_conflict_cost",
        "missing_setup_time_cost",
        "machine_unavailability_cost",
        "max_queue_time_cost",
        "missing_transport_time_cost",
    ):
        assert _raw(costs, name) == 0


def test_fab_resource_pin_and_cycle_constraints_match_manual_values():
    problem = SemiconductorScheduleModel(
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
                        id="O2",
                        family="F",
                        eligible_machines=[MachineOptionModel(machine_id="M2", duration=2)],
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
        pinned_assignments=[AssignmentModel(lot_id="L1", operation_id="O1", machine_id="M1", start=1, end=5)],
        assignments=[
            AssignmentModel(lot_id="L1", operation_id="O1", machine_id="M1", start=0, end=4),
            AssignmentModel(lot_id="L1", operation_id="O2", machine_id="M2", start=6, end=8),
            AssignmentModel(lot_id="L2", operation_id="O1", machine_id="M1", start=2, end=6),
        ],
    )
    items = fab_items(problem)
    assert items["min_queue_time_cost"] == [2]
    assert items["auxiliary_resource_capacity_cost"] == [2]
    assert sum(items["auxiliary_resource_unavailability_cost"]) == 3
    assert items["pinned_assignments_cost"] == [1]
    assert items["max_cycle_time_cost"] == [2]
    costs = fab_costs(problem)
    assert _raw(costs, "auxiliary_resource_unavailability_cost") == 3
    assert _raw(costs, "pinned_assignments_cost") == 1


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    import pytest

    def boom(_step, _score):
        raise RuntimeError("boom")

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        solve_fab(
            _fab_problem(with_assignments=False),
            time_limit_seconds=30.0,
            parallel_solver_count=4,
            on_new_best=boom,
        )
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path would call std::terminate."""
    import time

    import pytest

    from optimaru.semiconductor_scheduling import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            solve_fab(_fab_problem(with_assignments=False), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_stops_whatever_the_soft_total():
    """stop_at_first_feasible compares the hard level only (best_score_limit (0, -inf)).

    Every enabled soft penalty at weight 1e21 puts the soft score far below -1e18 at the
    first feasible solution: the solve still stops there instead of running its whole
    30 s. A fixed limit such as (0, -1e18) would never be reached by such a score.
    """
    import dataclasses
    import time

    import optimaru.semiconductor_scheduling as package
    from optimaru.semiconductor_scheduling.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        _fab_problem(with_assignments=False),
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18


def test_inputs_of_the_wrong_type_are_refused():
    import pytest

    import optimaru.semiconductor_scheduling as package

    with pytest.raises(TypeError, match="SemiconductorSchedule or a models.SemiconductorScheduleModel"):
        solve_fab({"lots": []})
    for refuses in (fab_costs, fab_items, package.calculate_score):
        with pytest.raises(TypeError):
            refuses([])
