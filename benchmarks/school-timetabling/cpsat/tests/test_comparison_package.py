"""The comparison package `school_timetabling_cpsat` imports and its offline helpers work.

No network and no C++ binary: the modules are imported, every package-relative import is
resolved (also those inside functions, which run only during a solve), the track-2 generator
builds the S school and the track-1 evaluator scores an assignment.
"""

import ast
import importlib
from pathlib import Path

import pytest

import school_timetabling_cpsat
from school_timetabling_cpsat import benchmark, cpsat_quickstart, evaluate, gen_canonical

PACKAGE_DIR = Path(school_timetabling_cpsat.__file__).parent
MODULES = sorted(path.stem for path in PACKAGE_DIR.glob("*.py") if path.stem != "__init__")


def test_the_four_modules_import():
    assert MODULES == ["benchmark", "cpsat_quickstart", "evaluate", "gen_canonical"]
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
