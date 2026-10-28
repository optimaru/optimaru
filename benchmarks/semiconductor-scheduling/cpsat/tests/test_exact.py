from semiconductor_scheduling import constraint_costs as fab_costs
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

from semiconductor_scheduling_cpsat import solve as solve_fab


def test_semiconductor_exact_handles_alternative_tools_reentry_batching_and_maintenance():
    def option(machine: str, duration: int, preference: int = 0):
        return MachineOptionModel(machine_id=machine, duration=duration, preference_cost=preference)

    result = solve_fab(
        SemiconductorScheduleModel(
            machines=[
                MachineModel(
                    id="LITHO-1",
                    group="LITHO",
                    unavailable=[TimeWindowModel(start=5, end=8)],
                ),
                MachineModel(id="LITHO-2", group="LITHO"),
                MachineModel(id="FURNACE-1", group="DIFF", batch_capacity=2, minimum_batch_size=2),
            ],
            setup_times=[
                SetupTimeModel(machine_group="LITHO", from_family="A", to_family="B", duration=2),
                SetupTimeModel(machine_group="LITHO", from_family="B", to_family="A", duration=1),
            ],
            transport_times=[
                TransportTimeModel(from_group="LITHO", to_group="DIFF", duration=1),
                TransportTimeModel(from_group="DIFF", to_group="LITHO", duration=2),
            ],
            lots=[
                LotModel(
                    id="LOT-A",
                    product="A",
                    release_time=0,
                    due_time=30,
                    priority=3,
                    operations=[
                        OperationModel(
                            id="PHOTO-1", family="A", eligible_machines=[option("LITHO-1", 4), option("LITHO-2", 5, 1)]
                        ),
                        OperationModel(
                            id="DIFF", family="A", max_queue_time=4, eligible_machines=[option("FURNACE-1", 6)]
                        ),
                        OperationModel(
                            id="PHOTO-2", family="A", eligible_machines=[option("LITHO-1", 3), option("LITHO-2", 4)]
                        ),
                    ],
                ),
                LotModel(
                    id="LOT-B",
                    product="B",
                    release_time=1,
                    due_time=32,
                    operations=[
                        OperationModel(
                            id="PHOTO-1", family="B", eligible_machines=[option("LITHO-1", 4), option("LITHO-2", 4)]
                        ),
                        OperationModel(id="DIFF", family="B", eligible_machines=[option("FURNACE-1", 6)]),
                        OperationModel(
                            id="PHOTO-2", family="B", eligible_machines=[option("LITHO-1", 3), option("LITHO-2", 3)]
                        ),
                    ],
                ),
            ],
        ),
        time_limit_seconds=2,
    )
    costs = fab_costs(result)
    assert len(result.assignments) == 6
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
    assert sum(costs["minimum_batch_fill"]) == 2


def test_exact_respects_auxiliary_resource_calendar_and_pinned_work():
    result = solve_fab(
        SemiconductorScheduleModel(
            machines=[MachineModel(id="M1", group="G1"), MachineModel(id="M2", group="G2")],
            auxiliary_resources=[
                AuxiliaryResourceModel(id="OPERATOR", capacity=1, unavailable=[TimeWindowModel(start=2, end=4)])
            ],
            lots=[
                LotModel(
                    id="A",
                    product="P",
                    due_time=20,
                    operations=[
                        OperationModel(
                            id="O",
                            family="F",
                            required_resources=["OPERATOR"],
                            eligible_machines=[MachineOptionModel(machine_id="M1", duration=2)],
                        )
                    ],
                ),
                LotModel(
                    id="B",
                    product="P",
                    due_time=20,
                    operations=[
                        OperationModel(
                            id="O",
                            family="F",
                            required_resources=["OPERATOR"],
                            eligible_machines=[MachineOptionModel(machine_id="M2", duration=2)],
                        )
                    ],
                ),
            ],
            pinned_assignments=[AssignmentModel(lot_id="A", operation_id="O", machine_id="M1", start=0, end=2)],
        ),
        time_limit_seconds=2,
    )
    by_lot = {assignment.lot_id: assignment for assignment in result.assignments}
    assert (by_lot["A"].machine_id, by_lot["A"].start, by_lot["A"].end) == ("M1", 0, 2)
    assert by_lot["B"].start >= 4
    assert result.score_hard == 0
