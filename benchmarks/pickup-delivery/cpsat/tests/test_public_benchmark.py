import json

import pytest

from pickup_delivery_cpsat.public_benchmark import (
    BEST_KNOWN,
    DISTANCE_SCALE,
    IncompletePlan,
    main,
    make_request,
    parse_instance,
    rescore,
    run_case,
    synthetic_instance,
)

SAMPLE = """\
2 10 1
0 0 0 0 0 100 0 0 0
1 3 4 4 0 20 2 0 2
2 6 8 -4 0 40 3 1 0
"""


def test_parse_and_scale_li_lim_pdptw() -> None:
    instance = parse_instance(SAMPLE, name="tiny")
    assert len(instance.requests) == 1
    request, pairs = make_request(instance, seconds=1, seed=0, workers=1, request_limit=None)
    assert len(request.model_input.vehicles) == 1
    assert request.model_input.vehicles[0].fixed_cost == 0
    assert request.model_input.vehicles[0].return_to_depot
    assert request.model_input.vehicles[0].max_stops == 0
    assert request.model_input.driving_seconds[0][1] == 5 * DISTANCE_SCALE
    delivery = request.model_input.deliveries[0]
    assert delivery.pickup.max_end_time == 22 * DISTANCE_SCALE
    assert delivery.dropoff.max_end_time == 43 * DISTANCE_SCALE
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


def test_best_known_table_covers_the_100_customer_set() -> None:
    assert len(BEST_KNOWN) == 56
    assert BEST_KNOWN["lc101"] == (10, 828.94)
    assert all(vehicles > 0 and distance > 0 for vehicles, distance in BEST_KNOWN.values())


# The lr and lrc service time: 10 units per stop. Leaving the request out must
# cost something (the unassigned penalty counts whole service minutes), which
# is what DISTANCE_SCALE is for.
SERVICED = """\
2 10 1
0 0 0 0 0 1000 0 0 0
1 3 4 4 0 20 10 0 2
2 6 8 -4 0 200 10 1 0
"""


def test_run_case_reports_both_backends_in_li_lim_units() -> None:
    instance = parse_instance(SERVICED, name="tiny")
    request, _ = make_request(instance, seconds=1, seed=0, workers=1)
    delivery = request.model_input.deliveries[0]
    assert delivery.pickup.service_duration + delivery.dropoff.service_duration >= 60
    lines: list[str] = []
    result = run_case(
        instance,
        None,
        seconds=1,
        seed=0,
        workers=1,
        backends=("engine", "cpsat"),
        log=lines.append,
    )
    assert result["requests"] == result["total_requests"] == 1
    assert result["best_known"] is None  # not one of the published instances
    assert [run["solver"] for run in result["runs"]] == ["engine", "cp-sat"]
    for run in result["runs"]:
        # depot -> (3, 4) -> (6, 8) -> depot is 5 + 5 + 10.
        assert run["outcome"] == "ok" and run["status"] == ""
        assert run["vehicles"] == 1 and run["distance"] == 20
    assert (result["budget"], result["seed"], result["workers"]) == (1, 0, 1)
    assert any(line.startswith("tiny: 1/1 requests") for line in lines)


def test_json_output_lists_one_result_per_synthetic_size(capsys) -> None:
    assert main(["--synthetic", "2", "--synthetic", "3", "--seconds", "1", "--backend", "engine", "--json"]) == 0
    captured = capsys.readouterr()
    results = json.loads(captured.out)
    assert [result["requests"] for result in results] == [2, 3]
    assert all(result["runs"][0]["solver"] == "engine" for result in results)
    assert "objective:" in captured.err


def test_a_plan_that_leaves_a_stop_out_is_incomplete_and_a_broken_rule_is_not() -> None:
    instance = parse_instance(SERVICED, name="tiny")
    pickup, dropoff = instance.requests[0]
    with pytest.raises(IncompletePlan):
        rescore(instance, instance.requests, {"V1": [_Stop(pickup.id)]})
    with pytest.raises(ValueError) as error:
        rescore(instance, instance.requests, {"V1": [_Stop(dropoff.id), _Stop(pickup.id)]})
    assert not isinstance(error.value, IncompletePlan)


def test_one_choice_of_instances_per_run() -> None:
    for argv in (
        ["--all", "--instance", "lc101"],
        ["--all", "--synthetic", "5"],
        ["--instance", "lc101", "--synthetic", "5"],
        ["--synthetic", "5", "--requests", "3"],
    ):
        with pytest.raises(SystemExit) as exit_info:
            main(argv)
        assert exit_info.value.code == 2


def test_out_holds_the_results_after_every_instance(tmp_path, capsys) -> None:
    out = tmp_path / "runs" / "synthetic.json"
    assert main(["--synthetic", "2", "--seconds", "1", "--backend", "engine", "--out", str(out)]) == 0
    (result,) = json.loads(out.read_text())
    assert result["requests"] == 2 and result["runs"][0]["outcome"] == "ok"
    assert not out.with_name("synthetic.json.part").exists()
