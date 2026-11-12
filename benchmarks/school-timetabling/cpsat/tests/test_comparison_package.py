"""The comparison package `school_timetabling_cpsat` imports and its offline helpers work.

No network and no C++ binary: the modules are imported, every package-relative import is
resolved (also those inside functions, which run only during a solve), the track-2 generator
builds the S school, the track-1 generator builds its schools and its cross-check instance, and
the track-1 evaluator scores an assignment.
"""

import ast
import importlib
from pathlib import Path

import pytest

import school_timetabling_cpsat
from school_timetabling_cpsat import benchmark, cpsat_quickstart, evaluate, gen_canonical, gen_quickstart

PACKAGE_DIR = Path(school_timetabling_cpsat.__file__).parent
MODULES = sorted(path.stem for path in PACKAGE_DIR.glob("*.py") if path.stem != "__init__")


def test_the_five_modules_import():
    assert MODULES == ["benchmark", "cpsat_quickstart", "evaluate", "gen_canonical", "gen_quickstart"]
    assert callable(benchmark.main)
    assert callable(cpsat_quickstart.solve_instance)


@pytest.mark.parametrize("module", MODULES)
def test_every_relative_import_resolves(module):
    """Track 2, track 3 and the track-1 CP-SAT run import their siblings lazily
    (`from . import gen_canonical` inside the function): resolve each of them here."""
    tree = ast.parse((PACKAGE_DIR / f"{module}.py").read_text(encoding="utf-8"))
    found = 0
    for node in ast.walk(tree):
        if not isinstance(node, ast.ImportFrom) or node.level == 0:
            continue
        assert node.level == 1, f"{module}: {ast.unparse(node)} leaves the package"
        target = f"{school_timetabling_cpsat.__name__}.{node.module}" if node.module else None
        for alias in node.names:
            if target is None:
                importlib.import_module(f"{school_timetabling_cpsat.__name__}.{alias.name}")
            else:
                assert hasattr(importlib.import_module(target), alias.name), f"{module}: {ast.unparse(node)}"
            found += 1
    if module in ("benchmark", "cpsat_quickstart"):
        assert found, f"{module} imports its siblings relatively"


def test_the_canonical_generator_builds_the_small_school():
    problem = gen_canonical.generate("S")
    assert len(problem["class_groups"]) == gen_canonical.SIZES["S"]
    assert problem["teachers"] and problem["rooms"] and problem["courses"]


@pytest.mark.parametrize("size", sorted(gen_quickstart.PRESETS))
def test_the_track1_generator_builds_a_school(size):
    classes, teachers, rooms = gen_quickstart.PRESETS[size]
    instance = gen_quickstart.generate(classes, teachers, rooms, seed=1)
    assert len(instance["timeslots"]) == 40 and len(instance["rooms"]) == rooms
    lessons = instance["lessons"]
    assert len(lessons) == classes * 34
    assert all(lesson["timeslot"] == -1 and lesson["room"] == -1 for lesson in lessons)
    # One teacher per (class, subject), out of that subject's pool, and nobody over the 40-slot week.
    subjects = [subject for subject, _ in gen_quickstart.CURRICULUM]
    load: dict[str, int] = {}
    for lesson in lessons:
        assert int(lesson["teacher"][1:]) % len(subjects) == subjects.index(lesson["subject"])
        load[lesson["teacher"]] = load.get(lesson["teacher"], 0) + 1
    assert len({(l["studentGroup"], l["subject"], l["teacher"]) for l in lessons}) == classes * len(subjects)
    assert len(load) <= teachers and max(load.values()) <= 40
    # random.Random(seed) alone: the same seed is the same school, another seed another one.
    assert gen_quickstart.generate(classes, teachers, rooms, seed=1) == instance
    assert gen_quickstart.generate(classes, teachers, rooms, seed=2) != instance


@pytest.mark.parametrize(
    ("shape", "message"),
    [((12, 20, 5), "more than the 200 cells"), ((30, 10, 30), "more than the 40-slot week"), ((0, 12, 7), "one class")],
)
def test_the_track1_generator_refuses_a_school_that_cannot_be_timetabled(shape, message):
    with pytest.raises(ValueError, match=message):
        gen_quickstart.generate(*shape, seed=1)


def test_the_track1_cross_check_instance_scores_minus_three_twice():
    instance = gen_quickstart.crosscheck()
    fixed = [(lesson["timeslot"], lesson["room"]) for lesson in instance["lessons"]]
    result = evaluate.score_assignment(instance, fixed)
    assert evaluate.format_score(result) == "-3hard/-3soft"
    costs = ("room_conflict_cost", "teacher_conflict_cost", "class_conflict_cost")
    costs += ("teacher_room_change_cost", "teacher_time_efficiency_cost", "class_subject_repeat_cost")
    assert all(result[name] != 0 for name in costs)


def test_the_track1_writer_stores_the_instance_twice(tmp_path):
    instance = gen_quickstart.crosscheck()
    text_path = gen_quickstart.write(instance, tmp_path / "crosscheck.json")
    assert evaluate.load_json(tmp_path / "crosscheck.json") == instance
    tokens = text_path.read_text(encoding="utf-8").split()
    assert tokens[:5] == ["ST1", "timeslots", "6", "0", "1"]
    assert tokens[-6:] == ["L6", "MATH", "TC", "G2", "4", "0"]
    assert len(tokens) == 1 + 2 + 6 * 2 + 2 + 2 + 2 + 7 * 6
    with pytest.raises(ValueError, match="single token"):
        gen_quickstart.to_text({**instance, "rooms": ["Room 0", "R1"]})


def test_the_track1_evaluator_scores_an_assignment():
    instance = {
        "timeslots": [{"day": 0, "period": 0}, {"day": 0, "period": 1}],
        "lessons": [
            {"id": "a", "teacher": "t1", "studentGroup": "g1", "subject": "math"},
            {"id": "b", "teacher": "t1", "studentGroup": "g2", "subject": "art"},
        ],
    }
    # The same teacher twice in one slot, in two rooms: one teacher conflict, one room change.
    clash = evaluate.score_assignment(instance, [(0, 0), (0, 1)])
    assert evaluate.format_score(clash) == "-1hard/-1soft"
    assert clash["teacher_conflict_cost"] == 1
    # Consecutive periods in one room: no conflict, one adjacent same-teacher pair (a reward).
    apart = evaluate.score_assignment(instance, [(0, 0), (1, 0)])
    assert evaluate.format_score(apart) == "0hard/1soft"
    assert apart["teacher_time_efficiency_cost"] == -1
