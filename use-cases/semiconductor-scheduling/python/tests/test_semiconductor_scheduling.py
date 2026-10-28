from semiconductor_scheduling import constraint_costs as fab_costs
from semiconductor_scheduling import solve as solve_fab
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


def test_semiconductor_constraints_match_manual_calculation():
    costs = fab_costs(_fab_problem(with_assignments=True))
    assert sum(costs["machine_eligibility"]) == 0
    assert sum(costs["lot_route_precedence"]) == 2
    assert sum(costs["machine_capacity"]) == 2
    assert sum(costs["batch_capacity"]) == 3
    assert sum(costs["batch_family_compatibility"]) == 8
    assert sum(costs["setup_transitions"]) == 5
    assert sum(costs["maintenance_windows"]) == 3
    assert sum(costs["maximum_queue_time"]) == 3
    assert sum(costs["inter_tool_transport"]) == 0
    assert sum(costs["machine_preference"]) == 1
    assert sum(costs["minimum_batch_fill"]) == 3
    assert sum(costs["machine_load_balance"]) == 8
    assert sum(costs["weighted_tardiness"]) == 6
    assert sum(costs["total_cycle_time"]) == 46
    assert costs["makespan"] == [17]


def test_semiconductor_inter_tool_transport_matches_manual_shortfall():
    problem = _fab_problem(with_assignments=True)
    problem.assignments[1].start = 6
    problem.assignments[1].end = 11
    assert sum(fab_costs(problem)["inter_tool_transport"]) == 1


def test_semiconductor_meta_builds_a_feasible_reentrant_schedule():
    problem = _fab_problem(with_assignments=False)
    solve_fab(problem, time_limit_seconds=0.05, random_seed=3)
    costs = fab_costs(problem)
    for name in (
        "machine_eligibility",
        "lot_route_precedence",
        "machine_capacity",
        "batch_capacity",
        "batch_family_compatibility",
        "setup_transitions",
        "maintenance_windows",
        "maximum_queue_time",
        "inter_tool_transport",
    ):
        assert sum(costs[name]) == 0


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
    costs = fab_costs(problem)
    assert costs["minimum_queue_time"] == [2]
    assert costs["auxiliary_resource_capacity"] == [2]
    assert sum(costs["auxiliary_resource_availability"]) == 3
    assert costs["pinned_assignments"] == [1]
    assert costs["maximum_cycle_time"] == [2]
