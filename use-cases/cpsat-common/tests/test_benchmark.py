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
