"""scripts/publish_benchmarks.py: the rows and tables it derives from harness output.

The script is pure Python and reads only JSON, so these tests need no extension.
"""

import argparse
import importlib.util
import json
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
    assert "| t60_00 | 60 | 20 | **21** | no solution in time |" in block

    # A packing that breaks a rule is "infeasible", with no "in time": the solver did answer.
    broken = {"instance": "t60_01", "items": 60, "best_known": 20}
    broken["runs"] = [{"solver": "engine", "bins": 21}, {"solver": "cp-sat", "bins": 20, "feasible": False}]
    cells, texts = publish._count_cells(broken, "bins")
    assert cells["ortools"] == {"value": None, "status": "infeasible"} and texts["ortools"] == "infeasible"


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


def test_a_reward_table_stays_one_when_a_row_is_decided_on_a_hard_rule() -> None:
    # A roster scores a reward; on the largest instance one solver breaks hard rules.
    results = [_compared([0, 46080], [0, 32141]), _compared([0, 76800], [-45242, 70011])]
    section = publish.compare_dataset("employee-scheduling", results, 30)["sections"][0]

    assert section["metric"] == "reward"
    soft, hard = section["rows"]
    assert (soft["level"], soft["cells"]["engine"]["value"], soft["cells"]["ortools"]["value"]) == (
        "soft",
        46080.0,
        32141.0,
    )
    # The hard row shows the score on that level: no violation beside 45242 of them.
    assert (hard["level"], hard["cells"]["engine"]["value"], hard["cells"]["ortools"]["value"]) == (
        "hard",
        0.0,
        -45242.0,
    )
    assert str(hard["cells"]["engine"]["value"]) == "0.0" and hard["best"] == ["engine"]


def test_a_penalty_table_has_no_negative_value() -> None:
    # A tie at zero and a hard row do not make a penalty table a reward table.
    results = [_compared([0, 0], [0, 0]), _compared([0, -30], [-2, -10]), _compared([0, -7], [0, -9])]
    section = publish.compare_dataset("bed-allocation", results, 30)["sections"][0]
    assert section["metric"] == "penalty"
    values = [cell["value"] for row in section["rows"] for cell in row["cells"].values()]
    assert values == [0.0, 0.0, 0.0, 2.0, 7.0, 9.0]


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


def test_a_routing_file_of_another_budget_seed_or_of_chosen_instances_is_refused() -> None:
    def series(name, seconds=30.0, seed=0, only=None):
        return {
            "protocol": {"series": name, "seconds": seconds, "seed": seed, "only": only},
            "results": [_vrp("X-n101-k25", "engine", 100, 1.0), _vrp("X-n101-k25", "ortools", 100, 2.0)],
        }

    publish._check_vrp("vehicle-routing (Set X)", series("x"), ARGS)
    with pytest.raises(SystemExit, match="10 s, not --seconds 30"):
        publish._check_vrp("vehicle-routing (Set X)", series("x", seconds=10.0), ARGS)
    # A run of --only instances would read as the whole series in the tables and bucket means.
    with pytest.raises(SystemExit, match="chosen instances \\(X-n101-k25\\) is not published"):
        publish._check_vrp("vehicle-routing (Set X)", series("x", only=["X-n101-k25"]), ARGS)
    assert "seed 0, single-threaded" in publish.vrp_block(series("x"), series("a"), ARGS)
    with pytest.raises(SystemExit, match="different seeds"):
        publish.vrp_block(series("x"), series("a", seed=3), ARGS)


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


def test_a_pickup_delivery_file_of_one_backend_publishes_the_other_as_not_run() -> None:
    cases = [_pdp("lc101", 10, 53, (2, 155.4))]
    cases[0]["runs"] = [{"solver": "cp-sat", "vehicles": 2, "distance": 155.4, "status": ""}]
    block = publish.pdp_li_lim_block(cases, ARGS)
    assert "| lc101 | first 10 of 53 requests | not run | 2 / 155.4 |" in block
    row = publish.pdp_li_lim_dataset(cases, 30)["sections"][0]["rows"][0]
    assert row["cells"]["engine"] == {"value": None, "status": "not run"} and row["best"] == []


def _half(solver, instance="demo", **changes):
    """One instance of a `--solver` run, as the comparison runner writes it."""
    whole = _compared((0, -3), (0, -5))
    run = next(run for run in whole["runs"] if run["solver"] == solver)
    return {**whole, "instance": instance, "budget": 30.0, "seed": 1, "workers": 8, "runs": [run], **changes}


def _files(directory, **files):
    directory.mkdir(exist_ok=True)
    for name, results in files.items():
        (directory / f"{name.replace('__', '.').replace('_', '-')}.json").write_text(json.dumps(results))
    return directory


def test_the_two_solver_runs_of_a_use_case_are_joined_into_its_comparison(tmp_path) -> None:
    engine, cpsat = [_half("engine", "a"), _half("engine", "b")], [_half("cp-sat", "a"), _half("cp-sat", "b")]
    compared = publish.load_compared(_files(tmp_path / "compare", knapsack__engine=engine, knapsack__cp_sat=cpsat))

    assert list(compared) == ["knapsack"]
    assert [result["instance"] for result in compared["knapsack"]] == ["a", "b"]
    assert [[run["solver"] for run in result["runs"]] for result in compared["knapsack"]] == [["engine", "cp-sat"]] * 2
    # The joined result is the one a run of both solvers writes (read back from JSON, like it).
    both = {**_half("engine", "a"), "runs": _compared((0, -3), (0, -5))["runs"]}
    assert compared["knapsack"][0] == json.loads(json.dumps(both))
    assert "| a | 3 jobs | **0hard/-3soft** | 0hard/-5soft |" in publish.compare_block(compared["knapsack"], "footer")

    # `--workers` is CP-SAT's: the engine half may record another value, and the join keeps CP-SAT's.
    (joined,) = publish.join_solver_runs("knapsack", [_half("engine", workers=1)], [_half("cp-sat", workers=8)])
    assert joined["workers"] == 8


@pytest.mark.parametrize(
    ("cpsat", "message"),
    [
        ([_half("cp-sat", "a")], "different instances"),
        ([_half("cp-sat", "b"), _half("cp-sat", "a")], "different instances"),
        ([_half("cp-sat", "a", seed=2), _half("cp-sat", "b")], "a differs in seed"),
        ([_half("cp-sat", "a"), _half("cp-sat", "b", size="4 jobs")], "b differs in size"),
        ([_half("cp-sat", "a"), _half("cp-sat", "b", budget=10.0)], "b differs in budget"),
        ([_half("cp-sat", "a", case="another-case"), _half("cp-sat", "b")], "a differs in case"),
        ([_half("engine", "a"), _half("cp-sat", "b")], "a in the cp-sat file is not a run of cp-sat alone"),
        ([_half("cp-sat", "a"), {"instance": "b", "error": "Traceback"}], "b failed in one of the two runs"),
    ],
)
def test_solver_runs_that_do_not_belong_together_are_not_joined(cpsat, message) -> None:
    engine = [_half("engine", "a"), _half("engine", "b")]
    with pytest.raises(SystemExit, match=message):
        publish.join_solver_runs("knapsack", engine, cpsat)


def test_a_comparison_without_a_run_of_each_solver_is_refused(tmp_path) -> None:
    # Published as it is, the missing solver would read as one that found no solution.
    with pytest.raises(SystemExit, match=r"demo has runs of \['engine'\], not of both"):
        publish.load_compared(_files(tmp_path / "whole", knapsack=[_half("engine")]))
    with pytest.raises(SystemExit, match=r"knapsack\.cp-sat\.json is missing"):
        publish.load_compared(_files(tmp_path / "half", knapsack__engine=[_half("engine")]))
    with pytest.raises(SystemExit, match="keep one form"):
        publish.load_compared(
            _files(
                tmp_path / "both",
                knapsack=[_compared((0, -3), (0, -5))],
                knapsack__engine=[_half("engine")],
                knapsack__cp_sat=[_half("cp-sat")],
            )
        )
    with pytest.raises(SystemExit, match="expected <slug>.json"):
        publish.load_compared(_files(tmp_path / "other", knapsack__other=[_half("engine")]))
    # A failed instance has no runs to count; it is reported as failed, as before.
    failed = [{"case": "knapsack", "instance": "demo", "error": "Traceback"}]
    assert publish.load_compared(_files(tmp_path / "failed", knapsack=failed)) == {"knapsack": failed}


def _school(classes, engine, cpsat, **changes):
    def run(solver, outcome):
        status, violations, penalty = outcome
        return {"solver": solver, "status": status, "hard_violations": violations, "penalty": penalty, "seconds": 30.0}

    result = {"instance": f"canonical_{classes}", "classes": classes, "teachers": 4 * classes, "rooms": classes}
    result |= {"courses": 11 * classes, "budget": 30.0, "seed": 1, "workers": 8}
    result["runs"] = [run("cp-sat", cpsat), run("engine", engine)]
    return result | changes


def test_school_timetabling_rows_come_from_the_track_2_results() -> None:
    results = [
        _school(3, ("FEASIBLE", 0, 120), ("OPTIMAL", 0, 118)),
        _school(6, ("FEASIBLE", 0, 231), ("FEASIBLE", 0, 320)),
        _school(9, ("FEASIBLE", 0, 500), ("UNKNOWN", None, None)),
        _school(12, ("FEASIBLE", 0, 1151), ("FEASIBLE", 2, 900)),
    ]
    block = publish.timetabling_block(results, ARGS)
    assert "| 3 | 12 | 33 | 120 | **118** |" in block
    assert "| 6 | 24 | 66 | **231** | 320 |" in block
    assert "| 9 | 36 | 99 | **500** | no solution in time |" in block
    # A timetable that breaks a hard rule has no penalty to compare, whatever its preferences cost.
    assert "| 12 | 48 | 132 | **1,151** | infeasible (2 hard violations) |" in block
    assert "seed 1." in block

    dataset = publish.timetabling_dataset(results, 30)
    assert dataset["code"] == "school-timetabling/cpsat"
    assert dataset["command"] == "uv run school-timetabling-compare --track2 --sizes 3,6,9,12 --seconds 30 --workers 8"
    rows = dataset["sections"][0]["rows"]
    assert [(row["instance"], row["size"]) for row in rows][:2] == [
        ("canonical_3", "3 classes / 12 teachers / 33 courses"),
        ("canonical_6", "6 classes / 24 teachers / 66 courses"),
    ]
    assert [row["best"] for row in rows] == [["ortools"], ["engine"], ["engine"], ["engine"]]
    assert rows[2]["cells"]["ortools"] == {"value": None, "status": "no solution"}
    assert rows[3]["cells"]["ortools"] == {"value": None, "status": "infeasible"}

    # The 20 schools of the runner's ladder are named as it names them.
    ladder = [_school(classes, ("FEASIBLE", 0, 1), ("FEASIBLE", 0, 1)) for classes in range(3, 61, 3)]
    assert "--sizes ladder --seconds 30" in publish.timetabling_dataset(ladder, 30)["command"]

    # A solver without a run is "not run"; a run that failed is not published.
    alone = _school(3, ("FEASIBLE", 0, 120), ("FEASIBLE", 0, 118))
    alone["runs"] = [run for run in alone["runs"] if run["solver"] == "cp-sat"]
    cells, texts = publish._timetabling_cells(alone)
    assert cells["engine"] == {"value": None, "status": "not run"} and texts["engine"] == "not run"
    with pytest.raises(SystemExit, match="canonical_3: a run cannot be published: error: boom"):
        publish._timetabling_cells(_school(3, ("error: boom", None, None), ("FEASIBLE", 0, 118)))
    with pytest.raises(SystemExit, match="10 s, not --seconds 30"):
        publish._check_protocol(
            "school-timetabling (track 2)", [_school(3, ("FEASIBLE", 0, 1), ("FEASIBLE", 0, 1), budget=10.0)], ARGS
        )


def test_generated_pickup_delivery_rows_are_one_per_size_and_instance_seed() -> None:
    cases = [
        _pdp("synthetic-10-s0", 10, 10, (4, 600.0), (4, 590.04)),
        _pdp("synthetic-10-s1", 10, 10, (4, 610.0), "no solution: status=UNKNOWN"),
        _pdp("synthetic-25-s0", 25, 25, (8, 1636.5), (8, 1653.3)),
        _pdp("synthetic-25-s1", 25, 25, (9, 1700.0), (9, 1700.0)),
        _pdp("synthetic-400-s0", 400, 400, (51, 19982.0)),
    ]
    block = publish.pdp_synthetic_block(cases)
    assert "(`--synthetic N`, instance seeds 0 and 1):" in block
    assert "| 10 | 0 | 4 / 600 | **4 / 590** |" in block
    assert "| 10 | 1 | **4 / 610** | no solution in time |" in block
    assert "| 400 | 0 | 51 / 19,982 | not run |" in block
    assert "CP-SAT is not run at 400 requests" in block

    dataset = publish.pdp_synthetic_dataset(cases, 30)
    # One command per group of sizes that share their seeds and their backends.
    assert dataset["command"].splitlines() == [
        "uv run pickup-delivery-public-bench --synthetic 10 --synthetic 25 --synthetic-seeds 0,1 "
        "--seconds 30 --workers 8",
        "uv run pickup-delivery-public-bench --synthetic 400 --seconds 30 --backend engine",
    ]
    rows = dataset["sections"][0]["rows"]
    assert [row["instance"] for row in rows] == [case["instance"] for case in cases]
    assert rows[0]["size"] == "10 requests / 25 vehicles"
    assert rows[-1]["cells"]["ortools"] == {"value": None, "status": "not run"} and rows[-1]["best"] == []
    assert rows[3]["best"] == ["engine", "ortools"]
    # One seed, the runner's default: the command names none.
    assert "(`--synthetic N`, instance seed 0):" in publish.pdp_synthetic_block(cases[::2])
    assert "--synthetic-seeds" not in publish.pdp_synthetic_dataset(cases[::2], 30)["command"]


def test_generated_rcpsp_instances_are_a_second_table_of_the_data_set() -> None:
    def makespan(instance, activities, engine, cpsat):
        result = {"instance": instance, "tasks": activities + 2, "resources": 4, "critical_path": 20, "seed": 0}
        result["runs"] = [{"solver": "engine", "makespan": engine}, {"solver": "cp-sat", "makespan": cpsat}]
        return result

    public = [makespan("Pat1", 30, 66, 62)]
    generated = [
        makespan(f"synthetic-{size}-s{seed}", size, 100 + seed, 101) for size in (30, 60) for seed in (0, 1, 2)
    ]

    dataset = publish.rcpsp_dataset(public, 30, generated)
    assert [(section["id"], len(section["rows"])) for section in dataset["sections"]] == [
        ("instances", 1),
        ("generated", 6),
    ]
    assert dataset["sections"][1]["rows"][3]["size"] == "60 activities / 4 resources"
    assert dataset["command"].splitlines() == [
        "uv run project-job-scheduling-public-bench --all --seconds 30 --workers 8",
        "uv run project-job-scheduling-public-bench --synthetic 30 --synthetic 60 --synthetic-seeds 0,1,2 "
        "--seconds 30 --workers 8",
    ]
    block = publish.rcpsp_block(public, ARGS, generated)
    assert "| Pat1 | 30 | 20 | 66 | **62** |" in block
    assert "(`--synthetic N`, instance seeds 0, 1 and 2):" in block
    assert "| synthetic-60-s1 | 60 | 20 | **101** | **101** |" in block
    assert block.count("| Instance | Activities |") == 2 and block.count("Measured ") == 1

    # Without generated results: the RG30 table alone, as before.
    alone = publish.rcpsp_dataset(public, 30)
    assert [section["id"] for section in alone["sections"]] == ["instances"]
    assert alone["command"] == "uv run project-job-scheduling-public-bench --all --seconds 30 --workers 8"
    assert "Generated" not in publish.rcpsp_block(public, ARGS)
    # Sizes and seeds are read from the names the runner gives its generated instances.
    with pytest.raises(SystemExit, match="Pat2: a generated instance is named synthetic-<size>-s<seed>"):
        publish.rcpsp_block(public, ARGS, [makespan("Pat2", 30, 1, 1)])
    # The two tables share one footer, so one seed.
    with pytest.raises(SystemExit, match="different seeds"):
        publish.rcpsp_block(public, ARGS, [makespan("synthetic-30-s0", 30, 1, 1) | {"seed": 4}])
