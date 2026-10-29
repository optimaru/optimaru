from __future__ import annotations

import json
from types import SimpleNamespace

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
        lambda package, problem: benchmark.Score(
            (0, float(problem.value)), ("hard", "soft")
        ),
    )

    assert benchmark.run_cli(FakeCase, ["--json", "--seconds", "0.01"]) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload[0]["case"] == "fake"
    assert payload[0]["runs"][0]["score"] == [0, 1.0]
    assert payload[0]["runs"][1]["score"] == [0, 2.0]


def test_a_failed_instance_fails_the_run(capsys) -> None:
    class Broken(FakeCase):
        @staticmethod
        def solve_engine(problem, seconds: float, seed: int) -> None:
            raise ValueError("boom")

    assert benchmark.run_cli(Broken, ["--json", "--seconds", "0.01"]) == 1


def test_a_stalled_cpsat_run_is_stopped_at_the_deadline() -> None:
    import time

    import _stalled_case

    started = time.perf_counter()
    run = benchmark._solve_cpsat_bounded(_stalled_case, "only", 0.1, 1, 1, deadline=3.0)
    assert time.perf_counter() - started < 30
    assert run.score is None
    assert run.status == "no solution in time"
