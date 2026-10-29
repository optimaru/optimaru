from order_picking.models import (
    LocationModel,
    OrderModel,
    OrderPickingRequest,
    ProductModel,
    TrolleyModel,
    to_solution,
)
import pytest

from order_picking import calculate_score, constraint_costs
from order_picking.constraints import get_constraints
from order_picking_cpsat import catalog, solve


def test_smoke_and_constraint_selection():
    origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    request = OrderPickingRequest(
        timelimit=2,
        trolleys=[
            TrolleyModel(id="T1", bucket_count=2, bucket_capacity=100, location=origin)
        ],
        orders=[
            OrderModel(
                id="O1",
                items=[ProductModel(id="P1", volume=50, location=origin)],
            )
        ],
    )
    result = solve(request, constraints=["required_number_of_buckets"])
    assert result.feasible
    assert result.unassigned == []
    assert result.routes["T1"][0].pick_task_id == "O1-0"
    assert result.total_distance == 0
    assert len(catalog()) == 4


def test_default_score_matches_the_engine_on_the_same_nonzero_route():
    origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    pick = LocationModel(column="B", shelving_row=2, side="RIGHT", row=4)
    request = OrderPickingRequest(
        timelimit=2,
        trolleys=[
            TrolleyModel(id="T1", bucket_count=2, bucket_capacity=100, location=origin)
        ],
        orders=[
            OrderModel(id="O1", items=[ProductModel(id="P1", volume=50, location=pick)])
        ],
    )
    result = solve(request)
    assert result.total_distance > 0

    canonical = to_solution(request)
    task_by_id = {task.id: i for i, task in enumerate(canonical.pick_tasks)}
    canonical.trolleys[0].pick_task_indices = [
        task_by_id[stop.pick_task_id] for stop in result.routes["T1"]
    ]
    assert result.score == str(calculate_score(canonical))


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_cpsat_constraint_matches_shared_raw_cost(constraint):
    origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    locations = [
        LocationModel(column="B", shelving_row=2, side="RIGHT", row=3),
        LocationModel(column="C", shelving_row=1, side="LEFT", row=5),
        LocationModel(column="D", shelving_row=3, side="RIGHT", row=1),
    ]
    request = OrderPickingRequest(
        timelimit=2,
        trolleys=[
            TrolleyModel(id="T1", bucket_count=1, bucket_capacity=50, location=origin),
            TrolleyModel(id="T2", bucket_count=1, bucket_capacity=50, location=origin),
        ],
        orders=[
            OrderModel(
                id="O1",
                items=[
                    ProductModel(id="P1", volume=40, location=locations[0]),
                    ProductModel(id="P2", volume=40, location=locations[1]),
                ],
            ),
            OrderModel(
                id="O2",
                items=[ProductModel(id="P3", volume=40, location=locations[2])],
            ),
        ],
    )
    fixed_routes = [[0, 2], [1]]
    proxy: dict[str, int] = {}

    solve(
        request,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_routes=fixed_routes,
        _proxy_score=proxy,
    )

    canonical = to_solution(request)
    canonical.trolleys[0].pick_task_indices = fixed_routes[0]
    canonical.trolleys[1].pick_task_indices = fixed_routes[1]
    expected = -sum(constraint_costs(canonical)[constraint.name]["costs"])
    assert sum(proxy.values()) == expected
