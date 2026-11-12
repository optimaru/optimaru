"""The track-2 instance generator and the JSON the comparison writes for the published table."""

import json
import sys

import pytest

from school_timetabling_cpsat import benchmark, gen_canonical
from teacher_scheduling import TimetableProblem


def test_a_size_is_a_preset_or_a_class_count() -> None:
    assert [gen_canonical.class_count(size) for size in ("S", "M", "L", "9", "60")] == [6, 12, 24, 9, 60]
    for size in ("XL", "0", "10", "-3", "4.5"):
        with pytest.raises(ValueError):
            gen_canonical.class_count(size)
    # A preset is the school of its class count.
    assert gen_canonical.generate("S") == gen_canonical.generate("6")


def test_the_ladder_is_twenty_schools() -> None:
    sizes = gen_canonical.expand_sizes("ladder")
    assert len(sizes) == 20 and sizes[:2] == ["3", "6"] and sizes[-1] == "60"
    assert gen_canonical.expand_sizes("s, 9,LADDER")[:3] == ["S", "9", "3"]
    with pytest.raises(ValueError, match="unknown size 'XL'"):
        gen_canonical.expand_sizes("S,XL")


@pytest.mark.parametrize("classes", gen_canonical.LADDER)
def test_every_school_of_the_ladder_is_a_valid_problem(classes: int) -> None:
    # generate() checks the loads itself: every class fills its week, no teacher is over the cap.
    problem = gen_canonical.generate(str(classes))
    assert len(problem["class_groups"]) == classes
    assert len(problem["teachers"]) >= classes  # one homeroom teacher per class
    TimetableProblem.model_validate(problem)


def test_track_2_writes_its_results_as_json(tmp_path, monkeypatch) -> None:
    out = tmp_path / "runs" / "track2.json"
    argv = ["school-timetabling-compare", "--track2", "--sizes", "3", "--seconds", "2", "--workers", "2"]
    monkeypatch.setattr(sys, "argv", [*argv, "--out-dir", str(tmp_path / "out"), "--out", str(out)])
    assert benchmark.main() == 0

    (result,) = json.loads(out.read_text())
    assert result["instance"] == "canonical_3"
    assert (result["classes"], result["teachers"], result["rooms"], result["courses"]) == (3, 11, 5, 39)
    assert (result["budget"], result["seed"], result["workers"]) == (2.0, 1, 2)
    cpsat, engine = result["runs"]
    assert (cpsat["solver"], engine["solver"]) == ("cp-sat", "engine")
    # The engine places every lesson of the smallest school at once; its penalty is the rescored one.
    assert engine["hard_violations"] == 0 and engine["penalty"] >= 0
    # A CP-SAT run without a timetable has neither a violation count nor a penalty.
    assert (cpsat["penalty"] is None) == (cpsat["hard_violations"] is None)
    assert (cpsat["penalty"] is None) == (cpsat["status"] not in ("OPTIMAL", "FEASIBLE"))
    assert (tmp_path / "out" / "RESULTS_track2.md").exists()
