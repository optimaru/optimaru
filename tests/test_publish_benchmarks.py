"""scripts/publish_benchmarks.py: the rows and tables it derives from harness output.

The script is pure Python and reads only JSON, so these tests need no extension.
"""

import argparse
import importlib.util
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("publish_benchmarks", ROOT / "scripts" / "publish_benchmarks.py")
publish = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(publish)

ARGS = argparse.Namespace(date="2026-01-02", machine="one test machine", seconds=30.0, workers=8)


def _compared(engine, cpsat, names=("hard", "soft")):
    def run(solver, score, status="FEASIBLE"):
        text = None if score is None else "/".join(f"{int(v)}{n}" for v, n in zip(score, names))
        return {
            "solver": solver,
            "score": score,
            "score_names": None if score is None else list(names),
            "score_text": text,
            "status": status,
        }

    return {
        "case": "demo-case",
        "instance": "demo",
        "size": "3 jobs",
        "runs": [run("engine", engine), run("cp-sat", cpsat, "FEASIBLE" if cpsat else "UNKNOWN")],
        "caveat": "",
    }


def test_planning_row_is_decided_on_the_first_level_that_differs() -> None:
    row = publish.planning_row(_compared([0, -30], [-2, -10]))
    assert row["level"] == "hard" and row["best"] == ["engine"]
    assert row["cells"] == {
        "engine": {"value": 0.0, "detail": "0hard/-30soft"},
        "ortools": {"value": 2.0, "detail": "-2hard/-10soft"},
    }


def test_planning_row_of_a_tie_and_of_a_missing_solution() -> None:
    tie = publish.planning_row(_compared([0, -7], [0, -7]))
    assert tie["level"] == "soft" and tie["best"] == ["engine", "ortools"]
    alone = publish.planning_row(_compared([0, -7], None))
    assert alone["level"] == "soft" and alone["best"] == ["engine"]
    assert alone["cells"]["ortools"] == {"value": None, "status": "no solution"}


def test_lowest_counts_only_rows_where_both_solvers_ran() -> None:
    assert publish._lowest({"engine": {"value": 3}, "ortools": {"value": 3}}) == ["engine", "ortools"]
    assert publish._lowest({"engine": {"value": 3}, "ortools": {"value": 2}}) == ["ortools"]
    # No solution in time is a loss; a solver that was not run is no comparison.
    assert publish._lowest({"engine": {"value": 3}, "ortools": {"value": None, "status": "no solution"}}) == ["engine"]
    assert publish._lowest({"engine": {"value": 3}, "ortools": {"value": None, "status": "not run"}}) == []
    assert publish._lowest({"engine": {"value": 3}}) == []


def _vrp(instance, backend, customers, gap, *, unassigned=0, feasible=True, distance=1000, best=990):
    return {
        "instance": instance,
        "backend": backend,
        "customer_count": customers,
        "gap": gap,
        "unassigned": unassigned,
        "feasible": feasible,
        "distance": distance,
        "best_known_distance": best,
    }


def test_vrp_dataset_has_gaps_incomplete_runs_and_bucket_means() -> None:
    data = {
        "protocol": {"series": "x", "seconds": 30.0, "seed": 0},
        "results": [
            _vrp("X-n101-k25", "engine", 100, 1.004, distance=27868, best=27591),
            _vrp("X-n101-k25", "ortools", 100, None, unassigned=2),
            _vrp("X-n106-k14", "engine", 105, 2.0),
            _vrp("X-n106-k14", "ortools", 105, 1.0),
            _vrp("X-n401-k29", "engine", 400, 3.0),
            _vrp("X-n401-k29", "ortools", 400, 5.0),
        ],
    }
    dataset = publish.vrp_dataset("cvrplib-x", data)
    section = dataset["sections"][0]
    first, second, _ = section["rows"]
    assert first["size"] == "100 customers" and first["reference"] == "27,591"
    assert first["cells"]["engine"] == {"value": 1.0, "detail": "27,868"}
    assert first["cells"]["ortools"] == {"value": None, "status": "incomplete", "detail": "2 unassigned"}
    assert first["best"] == ["engine"] and second["best"] == ["ortools"]
    # 100-200: the engine's mean is over two runs, OR-Tools' over the one it completed.
    small, medium = section["chart"]
    assert small["instance"] == "100–200 customers" and small["size"] == "2 instances"
    assert small["cells"]["engine"] == {"value": 1.5, "detail": "2 of 2 complete"}
    assert small["cells"]["ortools"] == {"value": 1.0, "detail": "1 of 2 complete"}
    assert medium["instance"] == "400–600 customers"
    assert "--series x" in dataset["command"] and dataset["opponent"] == "routing"

    block = publish.vrp_block(data, None, ARGS)
    assert "| X-n101-k25 | 100 | 27,591 | **1.00%** | 2 unassigned |" in block
    assert "| 100-200 | 2 | 1.50% (2/2) | 1.00% (1/2) |" in block


def _pdp(instance, requests, total, engine, cpsat=None, best=None):
    def run(solver, outcome):
        if isinstance(outcome, str):
            return {"solver": solver, "vehicles": None, "distance": None, "status": outcome}
        return {"solver": solver, "vehicles": outcome[0], "distance": outcome[1], "status": ""}

    runs = [run("engine", engine)] + ([] if cpsat is None else [run("cp-sat", cpsat)])
    return {
        "instance": instance,
        "requests": requests,
        "total_requests": total,
        "vehicles": 25,
        "best_known": best and {"vehicles": best[0], "distance": best[1]},
        "runs": runs,
    }


def test_pdp_li_lim_dataset_splits_whole_instances_from_the_head_to_head_runs() -> None:
    cases = [
        _pdp("lc101", 10, 53, (2, 155.42), (2, 155.42)),
        _pdp("lc101", 53, 53, (11, 892.0), "rejected: returned routes do not cover every selected stop", (10, 828.94)),
        _pdp("lr102", 55, 55, (18, 1500.0), best=(17, 1487.57)),
        _pdp("lr101", 25, 53, (12, 1015.9), "rejected: time window violated at node 7"),
        _pdp("lr101", 10, 53, (8, 553.0), "no solution: CP-SAT did not find a solution (status=UNKNOWN)"),
    ]
    whole, head = publish.pdp_li_lim_dataset(cases, 30)["sections"]
    assert whole["id"] == "all" and whole["solvers"] == ["engine"]
    assert [row["instance"] for row in whole["rows"]] == ["lc101", "lr102"]
    assert whole["rows"][0]["reference"] == "10 vehicles · 828.94" and whole["rows"][0]["ref"] == 828.94
    assert whole["rows"][0]["cells"] == {"engine": {"value": 892.0, "detail": "11 vehicles"}}
    assert whole["rows"][0]["best"] == []

    assert head["id"] == "head-to-head"
    assert [(row["instance"], row["size"]) for row in head["rows"]] == [
        ("lc101", "first 10 of 53 requests"),
        ("lc101", "all 53 requests"),
        ("lr101", "first 10 of 53 requests"),
        ("lr101", "first 25 of 53 requests"),
    ]
    statuses = [row["cells"]["ortools"].get("status") for row in head["rows"]]
    assert statuses == [None, "incomplete", "no solution", "infeasible"]
    assert [row["best"] for row in head["rows"]] == [["engine", "ortools"], ["engine"], ["engine"], ["engine"]]


def test_count_rows_mark_a_solver_that_was_not_run() -> None:
    results = [
        {
            "instance": "u120_00",
            "items": 120,
            "best_known": 48,
            "runs": [{"solver": "engine", "bins": 48}, {"solver": "cp-sat", "bins": 49}],
        },
        {"instance": "u250_00", "items": 250, "best_known": 99, "runs": [{"solver": "engine", "bins": 100}]},
        {
            "instance": "t60_00",
            "items": 60,
            "best_known": 20,
            "runs": [{"solver": "engine", "bins": 21}, {"solver": "cp-sat", "bins": None, "status": "UNKNOWN"}],
        },
    ]
    rows = publish.bpp_dataset(results, 30)["sections"][0]["rows"]
    assert rows[0]["best"] == ["engine"] and rows[0]["reference"] == "48" and rows[0]["ref"] == 48
    assert rows[1]["cells"]["ortools"] == {"value": None, "status": "not run"} and rows[1]["best"] == []
    assert rows[2]["cells"]["ortools"] == {"value": None, "status": "no solution"} and rows[2]["best"] == ["engine"]
    block = publish.bpp_block(results, ARGS)
    assert "| u120_00 | 120 | 48 | **48** | 49 |" in block
    assert "| u250_00 | 250 | 99 | 100 | not run (size cap) |" in block


def test_replace_block_keeps_the_text_around_the_markers(tmp_path) -> None:
    readme = tmp_path / "README.md"
    readme.write_text(
        "intro\n<!-- results:start -->\nold\n<!-- results:end -->\ntail\n<!-- qaplib:start -->\n<!-- qaplib:end -->\n"
    )
    publish._replace_block(readme, "new table\n")
    publish._replace_block(readme, "## QAPLIB", "qaplib")
    assert readme.read_text() == (
        "intro\n<!-- results:start -->\nnew table\n<!-- results:end -->\ntail\n"
        "<!-- qaplib:start -->\n## QAPLIB\n<!-- qaplib:end -->\n"
    )
    with pytest.raises(ValueError):
        publish._replace_block(readme, "x", "missing")


def test_homepage_file_round_trips_and_keeps_data_sets_a_run_did_not_measure(tmp_path) -> None:
    target = tmp_path / "benchmarks.ts"
    target.write_text("// an older file without data sets\n")
    assert publish._current_datasets(target) == {}
    kept = {"slug": "qaplib", "date": "2025-12-01", "sections": []}
    publish.write_homepage(target, {"ortools": "9.15"}, [kept])
    text = target.read_text()
    assert text.startswith("// GENERATED") and "export const datasets: Dataset[] = [" in text
    assert publish._current_datasets(target) == {"qaplib": kept}


def test_a_use_case_that_scores_a_reward_is_shown_as_scores() -> None:
    reward = publish.compare_dataset(
        "knapsack", [_compared([0, 34333], [0, 34336]), _compared([0, 1569], [0, 1569])], 30
    )
    section = reward["sections"][0]
    assert section["metric"] == "reward"
    assert [row["cells"]["engine"]["value"] for row in section["rows"]] == [34333.0, 1569.0]
    assert section["rows"][0]["best"] == ["ortools"] and section["rows"][1]["best"] == ["engine", "ortools"]
    assert reward["command"] == "uv run knapsack-compare --seconds 30 --workers 8"

    penalty = publish.compare_dataset("bed-allocation", [_compared([0, -30], [-2, -10]), _compared([0, 0], [0, 0])], 30)
    assert penalty["sections"][0]["metric"] == "penalty"
    assert penalty["sections"][0]["rows"][0]["cells"]["ortools"]["value"] == 2.0


def test_pickup_delivery_cells_follow_the_recorded_outcome() -> None:
    def run(outcome, status):
        return {"solver": "cp-sat", "vehicles": None, "distance": None, "outcome": outcome, "status": status}

    # The outcome decides, whatever the words of the status are.
    assert publish._pdp_cell(run("incomplete", "rejected: reworded"))[0] == {"value": None, "status": "incomplete"}
    assert publish._pdp_cell(run("infeasible", "rejected: time window violated at node 7"))[0]["status"] == "infeasible"
    assert publish._pdp_cell(run("no solution", "no solution: status=UNKNOWN"))[1] == "no solution in time"
    with pytest.raises(SystemExit, match="cannot be published"):
        publish._pdp_cell(run("mismatch", "mismatch: the solver reports 5, the independent scorer 6"))


def test_results_run_with_another_budget_or_worker_count_are_refused() -> None:
    engine, cpsat = {"solver": "engine"}, {"solver": "cp-sat"}
    publish._check_protocol("x", [{"instance": "a", "runs": [engine, cpsat]}], ARGS)  # nothing recorded
    publish._check_protocol("x", [{"instance": "a", "budget": 30.0, "workers": 8, "runs": [engine, cpsat]}], ARGS)
    # An engine-only run has no CP-SAT workers to state.
    publish._check_protocol("x", [{"instance": "a", "budget": 30.0, "workers": 1, "runs": [engine]}], ARGS)
    with pytest.raises(SystemExit, match="10 s, not --seconds 30"):
        publish._check_protocol("x", [{"instance": "a", "budget": 10.0, "runs": [engine]}], ARGS)
    with pytest.raises(SystemExit, match="1 workers, not --workers 8"):
        publish._check_protocol("x", [{"instance": "a", "budget": 30.0, "workers": 1, "runs": [engine, cpsat]}], ARGS)


def _qap(instance, objective, best):
    gap = None if best is None else 100 * (objective - best) / best
    run = {"solver": "engine", "objective": objective, "gap_to_qaplib_percent": gap}
    return {"instance": instance, "size": 30, "qaplib_best_known": best, "runs": [run]}


def test_qaplib_tables_leave_out_what_has_no_reference() -> None:
    results = [_qap("esc32a", 140, None), _qap("nug30", 6256, 6124), _qap("lipa30b", 151426, 151426)]
    block = publish.qaplib_block(results, ARGS)
    assert "no solution file for esc32a" in block
    assert "| esc32a | 30 | — | 140 | — |" in block and "| nug30 | 30 | 6,124 | 6,256 | 2.16% |" in block
    assert "Mean gap 1.08%, median 1.08%, worst 2.16%." in block
    rows = publish.qaplib_dataset(results, 30)["sections"][0]["rows"]
    assert [row["instance"] for row in rows] == ["nug30", "lipa30b"]
    assert rows[0]["cells"] == {"engine": {"value": 2.16, "detail": "6,256"}} and rows[0]["reference"] == "6,124"
    # Nothing but instances without a reference: a table, and no statistics to divide by zero.
    assert "Mean gap" not in publish.qaplib_block([_qap("esc32a", 140, None)], ARGS)


def test_sample_and_size_cap_in_the_prose_come_from_the_results() -> None:
    def station(instance, tasks, engine, cpsat=None):
        runs = [{"solver": "engine", "stations": engine}]
        if cpsat is not None:
            runs.append({"solver": "cp-sat", "stations": cpsat})
        return {"instance": instance, "tasks": tasks, "best_known": engine, "runs": runs}

    sets = {
        "large": [station("instance_n=100_1", 100, 24, 23), station("instance_n=100_500", 100, 14, 14)],
        "very-large": [station("instance_n=1000_1", 1000, 137), station("instance_n=1000_500", 1000, 555)],
    }
    block = publish.salbp_block(sets, ARGS)
    assert "SALBP-1: instances 1 and 500 of each data set." in block
    assert "(`--exact-max-size`, 100 tasks)" in block
    assert "| large | instance_n=100_1 | 100 | 24 | 24 | **23** |" in block
    assert "| very-large | instance_n=1000_1 | 1000 | 137 | 137 | not run (size cap) |" in block
    command = publish.salbp_dataset(sets, 30)["command"]
    assert command.endswith("--dataset large --instance instance_n=100_1 --instance instance_n=100_500 --seconds 30")

    def bins(instance, items):
        return {"instance": instance, "items": items, "best_known": 20, "runs": [{"solver": "engine", "bins": 21}]}

    sampled = [bins(f"{name}_{number}", 60) for name in ("u120", "t60") for number in ("00", "10", "19")]
    assert "Falkenauer: instances 00, 10 and 19 of each class." in publish.bpp_block(sampled, ARGS)
    assert "--sample-per-class 3 --seconds 30" in publish.bpp_dataset(sampled, 30)["command"]
    uneven = [bins("u120_00", 120), bins("u120_04", 120), bins("t60_00", 60)]
    assert "Falkenauer: 3 instances." in publish.bpp_block(uneven, ARGS)
    assert "--instance u120_00 --instance u120_04 --instance t60_00" in publish.bpp_dataset(uneven, 30)["command"]


def test_a_routing_file_run_with_another_budget_is_refused_and_a_partial_run_says_so() -> None:
    data = {
        "protocol": {"series": "x", "seconds": 10.0, "seed": 0, "only": ["X-n101-k25"]},
        "results": [_vrp("X-n101-k25", "engine", 100, 1.0), _vrp("X-n101-k25", "ortools", 100, 2.0)],
    }
    with pytest.raises(SystemExit, match="10 s, not --seconds 30"):
        publish._check_vrp("vehicle-routing (Set X)", data, ARGS)
    data["protocol"]["seconds"] = 30.0
    publish._check_vrp("vehicle-routing (Set X)", data, ARGS)
    assert publish.vrp_dataset("cvrplib-x", data)["command"].endswith("--catalog-source mirror --only X-n101-k25")
    data["protocol"]["only"] = None
    assert publish.vrp_dataset("cvrplib-x", data)["command"].endswith("--catalog-source mirror")


def test_the_footer_states_the_seed_the_results_record() -> None:
    def makespan(instance, seed=None):
        result = {"instance": instance, "tasks": 32, "resources": 4, "critical_path": 20}
        result["runs"] = [{"solver": "engine", "makespan": 66}, {"solver": "cp-sat", "makespan": 62}]
        return result if seed is None else result | {"seed": seed}

    # Results written before a harness recorded its seed: the harness default.
    assert "seed 0." in publish.rcpsp_block([makespan("Pat1")], ARGS)
    assert "seed 7." in publish.rcpsp_block([makespan("Pat1", 7), makespan("Pat2", 7)], ARGS)
    with pytest.raises(SystemExit, match="different seeds"):
        publish.rcpsp_block([makespan("Pat1", 0), makespan("Pat2", 7)], ARGS)


def test_a_run_whose_score_disagrees_with_the_scorer_is_not_published() -> None:
    run = {"solver": "cp-sat", "makespan": None, "outcome": "mismatch", "status": "mismatch: hard -1, excess 0"}
    result = {"instance": "Pat1", "tasks": 32, "resources": 4, "critical_path": 20, "runs": [run]}
    with pytest.raises(SystemExit, match="Pat1: a run cannot be published"):
        publish._count_cells(result, "makespan")
