"""st_timetabling.constraints.get_constraints — the default cost function in one line.

The existing `Assembly` chaining builder and spec classes are unchanged. This
checks that `get_constraints()` — the same entry point as every other use case
in the repository — returns exactly the fifteen default constraints, and that
the solve path accepts it as is.
"""

import pytest

import st_timetabling as st
from st_timetabling import canon
from st_timetabling.constraints import (
    Assembly,
    TeacherGap,
    get_constraints,
    list_constraints,
)


def test_get_constraints_defaults_to_the_full_registry():
    """The default list matches the core registry in names and order."""
    assert tuple(spec.name for spec in get_constraints()) == list_constraints()
    assert len(get_constraints()) == 15


def test_get_constraints_matches_the_default_assembly():
    """Must be the same set as `Assembly()` without arguments — only the entry point differs."""
    assert [spec.name for spec in get_constraints()] == [spec.name for spec in Assembly()]


def test_passing_a_list_uses_it_as_given():
    """A given list is used as is — only the listed constraints are scored."""
    picked = get_constraints([TeacherGap(weight=40)])
    assert [spec.name for spec in picked] == ["teacher_gap"]
    assert picked[0].weight == 40


def test_empty_list_is_rejected_not_treated_as_the_defaults():
    """An empty list is **not** the default — a filter that happens to come out empty must be an error.

    "Nothing selected" must not silently flip to "score everything"; the C++
    assembly path has the same contract (an empty list is a value_error)."""
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_returned_list_is_a_fresh_copy():
    """Changing the returned list must not affect the next call."""
    first = get_constraints()
    first.pop()
    assert len(get_constraints()) == 15


def test_specs_carry_the_documented_defaults():
    """Spec defaults are weight None / hard None / exponent 1.0."""
    spec = get_constraints()[0]
    assert spec.weight is None
    assert spec.hard is None
    assert spec.exponent == pytest.approx(1.0)


def test_default_assembly_matches_monolithic_score(problem):
    """A calculator assembled from the default list must score like the monolith."""
    converted = st.convert_problem(problem)
    canon.prepare(converted.table)
    calculator = canon.DecomposedCalculator(get_constraints())
    mono = canon.calculate_score(converted.table)
    plugin = calculator.score(converted.table)
    assert (plugin.hard, plugin.soft) == (mono.hard, mono.soft)


def test_solve_accepts_the_spec_list(problem):
    """`solve(constraints=get_constraints())` behaves like passing an Assembly."""
    result = st.solve(
        problem,
        constraints=get_constraints(),
        time_limit_seconds=10.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    assert result["notes"] == []
    assert len(result["lessons"]) == 40
    assert result["objective_value"] is not None
