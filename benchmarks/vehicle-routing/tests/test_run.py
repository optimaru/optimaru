import argparse

import pytest

from vrp_benchmark import solvers
from vrp_benchmark.instance import parse_instance, rescore
from vrp_benchmark.run import (
    SIZE_BUCKETS,
    BenchmarkResult,
    benchmark,
    bucket_table,
    main,
    markdown_summary,
    parse_backends,
)

SAMPLE = """\
NAME : A-n4-k2
COMMENT : (test, Min no of trucks: 2, Optimal value: 14)
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
# With capacity 3 the optimum is {1,2} + {3}: (5 + 4 + 1) + (2 + 2) = 14.
OPTIMUM = 14


def test_parse_instance() -> None:
    instance = parse_instance(SAMPLE)
    assert instance.vehicle_count == 2
    assert instance.customer_count == 3
    assert instance.best_known_distance == OPTIMUM
    assert instance.proven_optimal
    assert instance.distances[0][1] == 5


def test_rescore_counts_round_trips_and_flags_overload() -> None:
    instance = parse_instance(SAMPLE)
    assert rescore(instance, [[1, 2], [3]]).distance == OPTIMUM
    over = rescore(instance, [[1, 2, 3], []])
    assert not over.feasible and over.unassigned == 0
    partial = rescore(instance, [[1], []])
    assert partial.feasible and partial.unassigned == 2
    with pytest.raises(ValueError):
        rescore(instance, [[1], [1]])
    with pytest.raises(ValueError):
        rescore(instance, [[1], [2], [3]])  # three routes, two vehicles


def test_engine_request_mirrors_the_instance() -> None:
    from vrp_benchmark.solvers.engine import make_request

    request = make_request(parse_instance(SAMPLE), seconds=1, seed=3)
    assert request.model_input.driving_seconds[0][2] == 5
    assert request.model_input.driving_seconds[1][2] == 5
    assert [visit.demand for visit in request.model_input.visits] == [2, 1, 3]
    assert all(vehicle.return_to_depot for vehicle in request.model_input.vehicles)


def test_parse_backends() -> None:
    assert parse_backends("all") == solvers.BACKENDS
    assert parse_backends("engine, ortools") == ("engine", "ortools")
    assert parse_backends("ortools,engine,ortools") == ("ortools", "engine")
    with pytest.raises(ValueError):
        parse_backends("pyvrp")


def test_usage_errors_come_before_any_download() -> None:
    with pytest.raises(SystemExit) as exit_info:
        main(["--backend", "cpsat"])
    assert exit_info.value.code == 2
    with pytest.raises(SystemExit) as exit_info:
        main(["--series", "x", "--backend", "ortools", "--fail-on-incomplete"])
    assert exit_info.value.code == 2


@pytest.mark.parametrize("backend", solvers.BACKENDS)
def test_every_backend_finds_the_optimum_of_a_tiny_instance(backend: str) -> None:
    result = benchmark(backend, parse_instance(SAMPLE), seconds=1, seed=0)
    assert result.feasible and result.unassigned == 0
    assert result.distance == OPTIMUM
    assert result.gap == 0


def test_markdown_summary_explains_missing_gaps() -> None:
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
        result("engine", gap=1.5),
        result("ortools", unassigned=3),
    ]
    args = argparse.Namespace(seconds=1, seed=0, limit_per_bucket=0, catalog_source="mirror")
    backends = ("engine", "ortools")
    rows = bucket_table(results, backends, {label: 1 for label, _, _ in SIZE_BUCKETS})
    table = markdown_summary(results, rows, backends, args)
    row = next(line for line in table.splitlines() if line.startswith("| X-n101"))
    assert row == "| X-n101-k25 | 100 | 1.50% | 3 unassigned |"
