from pickup_delivery_cpsat.public_benchmark import (
    make_request,
    parse_instance,
    rescore,
    synthetic_instance,
)

SAMPLE = """\
2 10 1
0 0 0 0 0 100 0 0 0
1 3 4 4 0 20 2 0 2
2 6 8 -4 0 40 3 1 0
"""


def test_parse_and_scale_hexaly_pdptw() -> None:
    instance = parse_instance(SAMPLE, name="tiny")
    assert len(instance.requests) == 1
    request, pairs = make_request(
        instance, seconds=1, seed=0, workers=1, request_limit=None
    )
    assert len(request.model_input.vehicles) == 1
    assert request.model_input.vehicles[0].fixed_cost == 0
    assert request.model_input.vehicles[0].return_to_depot
    assert request.model_input.vehicles[0].max_stops == 0
    assert request.model_input.driving_seconds[0][1] == 5
    delivery = request.model_input.deliveries[0]
    assert delivery.pickup.max_end_time == 22
    assert delivery.dropoff.max_end_time == 43
    assert pairs[0][0].delivery_id == pairs[0][1].id


def test_synthetic_instance_is_seeded_and_servable_one_request_per_route() -> None:
    instance = synthetic_instance(40, seed=3)
    assert instance == synthetic_instance(40, seed=3)
    assert instance != synthetic_instance(40, seed=4)
    assert len(instance.requests) == 40
    assert instance.vehicle_count == 14
    # Every request fits a route of its own: the generator's feasibility promise.
    for pickup, dropoff in instance.requests:
        routes = {"V1": [_Stop(pickup.id), _Stop(dropoff.id)]}
        used, distance = rescore(instance, ((pickup, dropoff),), routes)
        assert used == 1 and distance > 0


class _Stop:
    def __init__(self, stop_id: int) -> None:
        self.stop_id = str(stop_id)
