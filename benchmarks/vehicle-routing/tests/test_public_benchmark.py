import pytest

from vrp_benchmark.public_benchmark import make_request, parse_instance

SAMPLE = """\
NAME : A-n4-k2
COMMENT : (test, Min no of trucks: 2, Optimal value: 12)
TYPE : CVRP
DIMENSION : 4
EDGE_WEIGHT_TYPE : EUC_2D
CAPACITY : 3
NODE_COORD_SECTION
1 0 0
2 3 4
3 0 1
4 2 0
DEMAND_SECTION
1 0
2 2
3 1
4 3
DEPOT_SECTION
1
-1
EOF
"""


def test_parse_and_expand_hexaly_cvrp() -> None:
    instance = parse_instance(SAMPLE)
    assert instance.vehicle_count == 2
    assert instance.best_known_distance == 12
    assert instance.proven_optimal
    assert instance.distances[0][1] == 5
    request = make_request(instance, seconds=1, seed=3, workers=1)
    assert request.model_input.driving_seconds[0][2] == 5
    assert request.model_input.driving_seconds[1][2] == 5
    assert [visit.demand for visit in request.model_input.visits] == [2, 1, 3]
    assert all(vehicle.fixed_cost == 0 for vehicle in request.model_input.vehicles)
    assert all(vehicle.return_to_depot for vehicle in request.model_input.vehicles)


def test_parse_backends_accepts_lists_and_aliases() -> None:
    from vrp_benchmark.public_benchmark import parse_backends

    assert parse_backends("both") == ("engine", "cpsat")
    assert parse_backends("engine, pyvrp") == ("engine", "pyvrp")
    assert parse_backends("all") == ("engine", "cpsat", "pyvrp")
    assert parse_backends("pyvrp,engine,pyvrp") == ("pyvrp", "engine")
    with pytest.raises(ValueError):
        parse_backends("ortools")


def test_unknown_backend_is_a_usage_error_before_any_download() -> None:
    from vrp_benchmark.public_benchmark import main

    with pytest.raises(SystemExit) as exit_info:
        main(["--backend", "ortools"])
    assert exit_info.value.code == 2

    with pytest.raises(SystemExit) as exit_info:
        main(["--series", "x", "--backend", "pyvrp", "--fail-on-unassigned"])
    assert exit_info.value.code == 2


def test_pyvrp_backend_routes_rescore_to_its_own_cost() -> None:
    pytest.importorskip("pyvrp")
    from vrp_benchmark.public_benchmark import rescore, solve_pyvrp

    instance = parse_instance(SAMPLE)
    outcome = solve_pyvrp(instance, seconds=0.5, seed=0)
    routes, reported, summary = outcome.routes, outcome.distance, outcome.summary
    used, distance, unassigned = rescore(instance, routes)
    # The harness rebuilds the distance from the same rounded matrix PyVRP
    # was given, so the two totals agree exactly and nobody is left out.
    # (The sample's "Optimal value: 12" comment is fictional; with capacity 3
    # the optimum is {1,2} + {3}: 5 + 4 + 1 and 2 + 2 = 14.)
    assert distance == reported == 14
    assert unassigned == 0
    assert used <= instance.vehicle_count
    assert outcome.feasible and summary.startswith("pyvrp feasible")


def test_pyvrp_backend_reports_an_infeasible_best_instead_of_raising() -> None:
    pytest.importorskip("pyvrp")
    from vrp_benchmark.public_benchmark import _benchmark, solve_pyvrp

    # Capacity 2 cannot carry the demand-3 customer: PyVRP's best stays
    # over capacity and its cost() is math.inf.
    instance = parse_instance(SAMPLE.replace("CAPACITY : 3", "CAPACITY : 2"))
    outcome = solve_pyvrp(instance, seconds=0.2, seed=0)
    assert not outcome.feasible and outcome.summary == "pyvrp infeasible cost=inf"
    result = _benchmark("pyvrp", instance, seconds=0.2, seed=0, workers=1)
    assert result.gap is None and result.unassigned == 0
    assert not result.feasible
    assert result.score.startswith("pyvrp infeasible")


def test_markdown_summary_distinguishes_missing_bks_from_infeasible() -> None:
    import argparse

    from vrp_benchmark.public_benchmark import (
        SIZE_BUCKETS,
        BenchmarkResult,
        _bucket_table,
        _markdown_summary,
    )

    def result(backend: str, **overrides) -> BenchmarkResult:
        fields = dict(
            backend=backend,
            instance="X-n101-k25",
            customer_count=100,
            used_vehicles=25,
            distance=1000,
            gap=None,
            unassigned=0,
            elapsed=1.0,
            score="",
        )
        fields.update(overrides)
        return BenchmarkResult(**fields)

    results = [
        result("engine"),  # fully assigned, but the catalog has no BKS
        result("pyvrp", feasible=False),
        result("cpsat", unassigned=3),
        result("other", gap=1.5),
    ]
    args = argparse.Namespace(
        seconds=1, seed=0, workers=1, limit_per_bucket=0, catalog_source="mirror"
    )
    backends = ("engine", "pyvrp", "cpsat", "other")
    rows = _bucket_table(results, backends, {label: 1 for label, _, _ in SIZE_BUCKETS})
    table = _markdown_summary(results, rows, backends, args)
    row = next(line for line in table.splitlines() if line.startswith("| X-n101"))
    assert row == "| X-n101-k25 | 100 | n/a | infeasible | 3 unassigned | 1.50% |"
