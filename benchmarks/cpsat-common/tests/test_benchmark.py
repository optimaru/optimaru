from __future__ import annotations

import json
from types import SimpleNamespace

import pytest

from cpsat_plugins import benchmark


class FakeCase:
    NAME = "fake"
    PACKAGE = "fake_package"
    CPSAT_EXACT = True
    CPSAT_CAVEAT = ""

    @staticmethod
    def instances():
        return {"small": lambda: SimpleNamespace(score=SimpleNamespace(hard=0, soft=0))}

    @staticmethod
    def describe(problem) -> str:
        return "one item"

    @staticmethod
    def solve_engine(problem, seconds: float, seed: int) -> None:
        problem.value = 1

    @staticmethod
    def solve_cpsat(problem, seconds: float, seed: int, workers: int):
        problem.value = 2
        return problem


def test_local_runner_compares_one_case(monkeypatch, capsys) -> None:
    monkeypatch.setattr(
        benchmark,
        "score_solution",
        lambda package, problem: benchmark.Score((0, float(problem.value)), ("hard", "soft")),
    )

    assert benchmark.run_cli(FakeCase, ["--json", "--seconds", "0.01"]) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload[0]["case"] == "fake"
    assert payload[0]["runs"][0]["score"] == [0, 1.0]
    assert payload[0]["runs"][1]["score"] == [0, 2.0]


def test_one_solver_alone_gives_its_run_of_the_comparison(monkeypatch, capsys) -> None:
    monkeypatch.setattr(
        benchmark,
        "score_solution",
        lambda package, problem: benchmark.Score((0, float(problem.value)), ("hard", "soft")),
    )

    runs = {}
    for solver in ("engine", "cp-sat", "both"):
        argv = ["--json", "--seconds", "0.01", "--seed", "3", "--workers", "2", "--solver", solver]
        assert benchmark.run_cli(FakeCase, argv) == 0
        (result,) = json.loads(capsys.readouterr().out)
        # Every result says what it was given, whichever solvers ran.
        assert (result["size"], result["budget"], result["seed"], result["workers"]) == ("one item", 0.01, 3, 2)
        runs[solver] = [(run["solver"], run["score"]) for run in result["runs"]]

    assert runs["engine"] == [("engine", [0, 1.0])]
    assert runs["cp-sat"] == [("cp-sat", [0, 2.0])]
    assert runs["both"] == runs["engine"] + runs["cp-sat"]


def test_a_failed_instance_fails_the_run(capsys) -> None:
    class Broken(FakeCase):
        @staticmethod
        def solve_engine(problem, seconds: float, seed: int) -> None:
            raise ValueError("boom")

    assert benchmark.run_cli(Broken, ["--json", "--seconds", "0.01", "--seed", "5", "--workers", "3"]) == 1
    (result,) = json.loads(capsys.readouterr().out)
    # A failed result says what it was given, like the others.
    assert "boom" in result["error"] and (result["seed"], result["workers"]) == (5, 3)


@pytest.mark.parametrize("solvers", ["engine", (), ("cpsat",), ("engine", "ortools")])
def test_a_solver_the_runner_does_not_know_is_refused(solvers) -> None:
    # A bare string would pass a substring test and give a result with no runs.
    with pytest.raises(ValueError, match="solvers is a sequence of engine and/or cp-sat"):
        benchmark.run_instance(FakeCase, "small", FakeCase.instances()["small"], 0.01, 1, 1, solvers=solvers)


def test_a_stalled_cpsat_run_is_stopped_at_the_deadline() -> None:
    import time

    import _stalled_case

    started = time.perf_counter()
    run = benchmark._solve_cpsat_bounded(_stalled_case, "only", 0.1, 1, 1, deadline=3.0)
    assert time.perf_counter() - started < 30
    assert run.score is None
    assert run.status == "no solution in time"


def test_a_folded_medium_level_is_shown_on_its_own():
    from cpsat_plugins.benchmark import Score, _unfold_medium

    def fold(kind, unit):
        return {"some_cost": {"medium_fold": {"kind": kind, "unit": unit}}}

    into_soft = _unfold_medium(Score((0, -9_000_000_020.0), ("hard", "soft")), fold("into_soft", 1e9))
    assert (into_soft.levels, into_soft.names) == ((0, -9, -20.0), ("hard", "medium", "soft"))
    below_hard = _unfold_medium(Score((-2_000_003, -5.0), ("hard", "soft")), fold("below_hard", 1e6))
    assert below_hard.levels == (-2, -3, -5.0)
    # Order is kept: a better folded score is still the better unfolded one.
    worse = _unfold_medium(Score((0, -10_000_000_000.0), ("hard", "soft")), fold("into_soft", 1e9))
    assert worse < into_soft
    # The solver's self-report is checked against the engine's folded levels.
    from cpsat_plugins.benchmark import SolverRun

    assert SolverRun("engine", into_soft, 1.0, self_report=(0, -9_000_000_020.0)).crosscheck == ""
    assert "MISMATCH" in SolverRun("engine", into_soft, 1.0, self_report=(0, -9_001_000_020.0)).crosscheck
    plain = Score((0, -5.0), ("hard", "soft"))
    assert _unfold_medium(plain, {"some_cost": {}}) is plain


def test_a_ladder_has_one_instance_per_seed_of_a_scale() -> None:
    ladder = benchmark.seeded(
        {
            "small": (lambda seed: ("small", seed), (4, 9, 2)),
            "demo": lambda: "the demo",
            "large": (lambda seed: ("large", seed), (0,)),
        }
    )
    # The first seed keeps the scale's name, so a ladder that gains seeds keeps its rows.
    assert list(ladder) == ["small", "small-2", "small-3", "demo", "large"]
    assert [build() for build in ladder.values()] == [
        ("small", 4),
        ("small", 9),
        ("small", 2),
        "the demo",
        ("large", 0),
    ]
    # A name two scales would both give is refused, not overwritten.
    with pytest.raises(ValueError, match="duplicate instance 'small-2'"):
        benchmark.seeded({"small": (lambda seed: seed, (1, 2)), "small-2": lambda: "another"})
