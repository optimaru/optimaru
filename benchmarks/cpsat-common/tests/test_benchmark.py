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


class FakeTimefoldCase(FakeCase):
    TIMEFOLD = "fake"

    @staticmethod
    def to_timefold(problem) -> dict:
        return {"value": getattr(problem, "value", None)}

    @staticmethod
    def from_timefold(problem, result: dict) -> None:
        problem.value = result["assignment"]["value"]


def test_timefold_column_and_referee_scores(monkeypatch, capsys) -> None:
    from cpsat_plugins import timefold

    monkeypatch.setattr(timefold, "available", lambda: True)
    monkeypatch.setattr(
        timefold, "solve", lambda use_case, payload, seconds, seed: {"score": "0hard/-3soft", "assignment": {"value": 3}}
    )
    monkeypatch.setattr(timefold, "score", lambda use_case, payload: {"score": f"0hard/-{payload['value']}soft"})
    monkeypatch.setattr(
        benchmark,
        "score_solution",
        lambda package, problem: benchmark.Score((0, -float(problem.value)), ("hard", "soft")),
    )

    assert benchmark.run_cli(FakeTimefoldCase, ["--json", "--seconds", "0.01"]) == 0
    runs = json.loads(capsys.readouterr().out)[0]["runs"]
    assert [run["solver"] for run in runs] == ["engine", "cp-sat", "timefold"]
    assert [run["timefold_score"] for run in runs] == ["0hard/-1soft", "0hard/-2soft", "0hard/-3soft"]

    assert benchmark.run_cli(FakeTimefoldCase, ["--json", "--seconds", "0.01", "--no-timefold"]) == 0
    assert len(json.loads(capsys.readouterr().out)[0]["runs"]) == 2


def test_a_failed_instance_fails_the_run(capsys) -> None:
    class Broken(FakeCase):
        @staticmethod
        def solve_engine(problem, seconds: float, seed: int) -> None:
            raise ValueError("boom")

    assert benchmark.run_cli(Broken, ["--json", "--seconds", "0.01"]) == 1
