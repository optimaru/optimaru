from pickup_delivery_cpsat.public_benchmark import make_request, parse_instance

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
