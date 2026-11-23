import argparse
import json
import os
from pathlib import Path

import pytest

from vrp_benchmark import solvers, vrptw
from vrp_benchmark.instance import rescore
from vrp_benchmark.run import BenchmarkResult, benchmark, main, vrptw_selection, write_outputs

# Customers 1 and 2 lie on opposite sides of the depot and must both be served by time 6,
# so they need two routes; customer 3 opens at 20, after customer 1 on the same route.
# Optimum: 0-1-3-0 (5 + 5 + 10) and 0-2-0 (5 + 5) = 30.0, in tenths 300.
SAMPLE = """\
NAME : T-3
TYPE : VRPTW
DIMENSION : 4
VEHICLES : 3
CAPACITY : 10
SERVICE_TIME : 1
EDGE_WEIGHT_TYPE : EUC_2D
NODE_COORD_SECTION
1 0 0
2 3 4
3 -3 -4
4 6 8
DEMAND_SECTION
1 0
2 1
3 1
4 1
TIME_WINDOW_SECTION
1 0 100
2 0 6
3 0 6
4 20 40
DEPOT_SECTION
1
-1
EOF
"""
OPTIMUM = 300


def test_parse_vrptw_works_in_tenths() -> None:
    instance = vrptw.parse_vrptw(SAMPLE, best_known_distance=OPTIMUM)
    assert instance.time_windows and instance.scale == 10
    assert instance.vehicle_count == 3 and instance.customer_count == 3
    assert instance.distances[0][1] == 50 and instance.distances[1][3] == 50
    assert instance.ready_times == (0, 0, 0, 200) and instance.due_times == (1000, 60, 60, 400)
    # The depot has no service time.
    assert instance.service_times == (0, 10, 10, 10)


def test_dimacs_distances_are_truncated_to_one_decimal() -> None:
    assert vrptw.dimacs_distance((0, 0), (1, 1)) == 14  # 1.414... -> 1.4
    assert vrptw.dimacs_distance((0, 0), (2, 1)) == 22  # 2.236... -> 2.2
    assert vrptw.dimacs_distance((0, 0), (3, 4)) == 50  # exact, not 49
    assert vrptw.parse_solution_cost("Route #1: 1 2\nCost 1637.7\n") == 16377
    assert vrptw.parse_solution_cost("Cost 828\n") == 8280
    assert vrptw.parse_solution_routes("Route #1: 1 3\nRoute #2: 2\nCost 30\n") == [[1, 3], [2]]
    with pytest.raises(ValueError):
        vrptw.parse_solution_cost("Cost 1.25\n")


def test_rescore_replays_the_schedule() -> None:
    instance = vrptw.parse_vrptw(SAMPLE)
    assert rescore(instance, [[1, 3], [2], []]) == rescore(instance, [[1, 3], [2]])
    best = rescore(instance, [[1, 3], [2]])
    assert best.feasible and best.distance == OPTIMUM
    # One route reaches customer 2 at 16, past its window's end at 6.
    late = rescore(instance, [[1, 2, 3]])
    assert not late.feasible and late.unassigned == 0
    # Customer 3 first: waiting for it to open makes customer 1 late.
    assert not rescore(instance, [[3, 1], [2]]).feasible


def test_engine_windows_bound_arrival_plus_service() -> None:
    from vrp_benchmark.solvers.engine import make_request

    request = make_request(vrptw.parse_vrptw(SAMPLE), seconds=1, seed=0)
    visits = request.model_input.visits
    assert [(v.min_start_time, v.max_end_time, v.service_duration) for v in visits] == [
        (0, 70, 10),
        (0, 70, 10),
        (200, 410, 10),
    ]
    assert all(v.max_end_time == 1000 and v.end_depot == "depot" for v in request.model_input.vehicles)


@pytest.mark.parametrize("backend", solvers.BACKENDS)
def test_every_backend_finds_the_optimum_of_a_tiny_vrptw_instance(backend: str) -> None:
    result = benchmark(backend, vrptw.parse_vrptw(SAMPLE, best_known_distance=OPTIMUM), seconds=1, seed=0)
    assert result.feasible and result.unassigned == 0
    assert result.distance == OPTIMUM and result.gap == 0 and result.scale == 10


def test_the_series_hold_every_instance_once() -> None:
    solomon, homberger = vrptw.solomon_entries(), vrptw.homberger_entries()
    assert len(solomon) == 56 and len({entry.name for entry in solomon}) == 56
    assert len(homberger) == 300 and len({entry.name for entry in homberger}) == 300
    assert solomon[0].path == "Solomon/C101" and solomon[-1].name == "RC208"
    assert homberger[0].path == "GH200/C1_2_1" and homberger[-1].path == "RC2_10_10"
    entries, sizes = vrptw_selection("homberger", 1)
    # One per class and size: every summary group still has instances.
    assert len(entries) == 30 and sizes == {"200": 60, "400": 60, "600": 60, "800": 60, "1000": 60}
    _, classes = vrptw_selection("solomon", 0)
    assert classes == {"C1": 9, "C2": 8, "R1": 12, "R2": 11, "RC1": 8, "RC2": 8}


def test_vrptw_usage_errors() -> None:
    for argv in (
        ["--series", "solomon", "--limit-per-bucket", "1"],
        ["--series", "homberger", "--jobs", "0"],
        ["--instance", "A-n32-k5", "--jobs", "2"],
    ):
        with pytest.raises(SystemExit) as exit_info:
            main(argv)
        assert exit_info.value.code == 2


def test_the_output_records_jobs_scale_and_class(tmp_path) -> None:
    args = argparse.Namespace(
        series="solomon", seconds=1.0, seed=0, only=None, jobs=3, out=tmp_path / "s.json", summary_markdown=None
    )
    result = BenchmarkResult("engine", "C101", 100, 10, 8273, 0.0, 0, 1.0, "", scale=10, instance_class="C1")
    write_outputs(args, [result], ("engine",), {"C1": 9, "C2": 8, "R1": 12, "R2": 11, "RC1": 8, "RC2": 8})
    data = json.loads(args.out.read_text())
    assert data["protocol"]["jobs"] == 3
    assert data["results"][0]["scale"] == 10 and data["results"][0]["instance_class"] == "C1"
    assert data["buckets"][0] == {
        "backend": "engine",
        "bucket": "C1",
        "runs": 1,
        "solved": 1,
        "catalog": 9,
        "mean_gap": 0.0,
    }


def test_a_failed_run_stops_a_parallel_series(monkeypatch) -> None:
    from vrp_benchmark import run

    args = argparse.Namespace(
        series="solomon",
        seconds=0.1,
        seed=0,
        only=None,
        jobs=2,
        out=None,
        summary_markdown=None,
        backend=("engine",),
    )
    jobs = [(f"T{i}", _broken_loader, "C1") for i in range(6)]
    with pytest.raises(ValueError, match="broken instance"):
        run.run_all(args, jobs, None)


def _broken_loader():
    raise ValueError("broken instance")


def test_a_route_back_after_the_depot_closes_is_late() -> None:
    # Depot closes at 25: 0-1-3 serves both on time, but customer 3's service ends at 21
    # and the depot is 10 away, so the vehicle is back at 31.
    instance = vrptw.parse_vrptw(SAMPLE.replace("1 0 100", "1 0 25").replace("4 20 40", "4 20 21"))
    assert not rescore(instance, [[1, 3], [2]]).feasible
    assert rescore(instance, [[1], [2]]).feasible


def test_a_parallel_series_keeps_the_series_order(tmp_path) -> None:
    from functools import partial

    from vrp_benchmark import run

    args = argparse.Namespace(
        series="solomon",
        seconds=0.2,
        seed=0,
        only=None,
        jobs=2,
        out=tmp_path / "s.json",
        summary_markdown=None,
        backend=("engine", "ortools"),
    )
    load = partial(vrptw.parse_vrptw, SAMPLE, best_known_distance=OPTIMUM)
    jobs = [("T-3", load, "C1"), ("T-3", load, "R1")]
    results = run.run_all(args, jobs, {cls: 1 for cls in vrptw.CLASSES})
    assert [(r.instance_class, r.backend) for r in results] == [
        ("C1", "engine"),
        ("C1", "ortools"),
        ("R1", "engine"),
        ("R1", "ortools"),
    ]
    assert all(r.distance == OPTIMUM for r in results)
    assert json.loads(args.out.read_text())["protocol"]["jobs"] == 2


def test_a_corrupt_cached_file_is_fetched_again(tmp_path, monkeypatch) -> None:
    # Named C101, three customers: the loader checks both against the parsed file.
    entry = vrptw.VrptwEntry("C101", "solomon", "C1", 3)
    base = tmp_path / "vrptw" / vrptw.MIRROR_COMMIT[:12] / entry.name
    base.parent.mkdir(parents=True)
    base.with_suffix(".vrp").write_text("<html>proxy error</html>")
    text = SAMPLE.replace("T-3", "C101")
    files = {".sol": "Route #1: 1 3\nRoute #2: 2\nCost 30\n", ".vrp": text}
    monkeypatch.setattr(vrptw, "_download", lambda url: files[url[-4:]].encode())
    instance = vrptw.load_vrptw(entry, tmp_path)
    assert instance.best_known_distance == OPTIMUM
    assert base.with_suffix(".vrp").read_text() == text


def test_a_cached_file_missing_a_section_is_fetched_again(tmp_path, monkeypatch) -> None:
    entry = vrptw.VrptwEntry("C101", "solomon", "C1", 3)
    base = tmp_path / "vrptw" / vrptw.MIRROR_COMMIT[:12] / entry.name
    base.parent.mkdir(parents=True)
    text = SAMPLE.replace("T-3", "C101")
    base.with_suffix(".vrp").write_text(text.split("DEPOT_SECTION")[0])  # cut short: KeyError, not ValueError
    files = {".sol": "Cost 30\n", ".vrp": text}
    monkeypatch.setattr(vrptw, "_download", lambda url: files[url[-4:]].encode())
    assert vrptw.load_vrptw(entry, tmp_path).customer_count == 3
    assert base.with_suffix(".vrp").read_text() == text


def test_a_file_of_another_instance_is_refused_and_not_cached(tmp_path, monkeypatch) -> None:
    entry = vrptw.VrptwEntry("C101", "solomon", "C1", 3)
    files = {".sol": "Cost 30\n", ".vrp": SAMPLE}  # named T-3, not C101
    monkeypatch.setattr(vrptw, "_download", lambda url: files[url[-4:]].encode())
    with pytest.raises(ValueError, match="expected C101"):
        vrptw.load_vrptw(entry, tmp_path)
    assert not (tmp_path / "vrptw" / vrptw.MIRROR_COMMIT[:12] / "C101.vrp").exists()


@pytest.mark.skipif(not os.environ.get("VRPTW_BKS_CHECK"), reason="downloads 712 files; set VRPTW_BKS_CHECK=1")
def test_every_best_known_solution_rescores_to_its_cost() -> None:
    # Guards a MIRROR_COMMIT bump and any change to the rescoring: the harness and the
    # best-known solutions agree on the DIMACS convention only if this holds for all 356.
    cache = Path(os.environ.get("VRPTW_CACHE", ".bench-cache"))
    for entry in vrptw.solomon_entries() + vrptw.homberger_entries():
        instance = vrptw.load_vrptw(entry, cache)
        solution = (cache / "vrptw" / vrptw.MIRROR_COMMIT[:12] / f"{entry.name}.sol").read_text()
        checked = rescore(instance, vrptw.parse_solution_routes(solution))
        assert checked.feasible and checked.unassigned == 0, entry.name
        assert checked.distance == instance.best_known_distance, entry.name
