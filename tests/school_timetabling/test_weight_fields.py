"""A soft constraint without a field in the fast path's `canon.ObjectiveWeights`.

A new soft component joins the catalog before it goes onto the default path, and gets its
`ObjectiveWeights` field only then (extending.md, step 5). In between it must not break a
solve: with ``constraints=`` it is scored at its own weight, and on the default path the
monolithic calculators do not score it, as in the other use cases before that step.

Giving a constraint a field takes C++, so these tests hide the field of one constraint that
has one (`non_preferred_period_cost`) from the Python side instead, where the weights are read.
"""

import pytest

import optimaru.school_timetabling as st
from optimaru.school_timetabling import canon, convert
from optimaru.school_timetabling.constraints import NonPreferredPeriodCost, default_weights, get_constraints

HIDDEN = "non_preferred_period_cost"


@pytest.fixture
def without_field(monkeypatch):
    """The Python side as it is while HIDDEN has no ObjectiveWeights field."""
    fields = convert._WEIGHT_FIELDS - {HIDDEN}
    monkeypatch.setattr(convert, "_WEIGHT_FIELDS", fields)
    monkeypatch.setattr(st, "_WEIGHT_FIELDS", fields)


def _weighted(result, weights):
    return sum(weights[name] * count for name, count in result["penalties"].items())


def test_a_constraint_with_a_field_reads_it():
    """The fields are soft constraints of the catalog (import checks it), and each is read as it is."""
    assert convert._WEIGHT_FIELDS <= set(default_weights())
    assert {"teacher_gap_cost", HIDDEN} <= convert._WEIGHT_FIELDS
    custom = canon.ObjectiveWeights(teacher_gap_cost=7, non_preferred_period_cost=4)
    weights = convert._soft_weights(custom)
    assert weights == {name: getattr(custom, name, weights[name]) for name in default_weights()}
    assert (weights["teacher_gap_cost"], weights[HIDDEN]) == (7, 4)


def test_a_constraint_without_a_field_takes_its_default(without_field):
    custom = canon.ObjectiveWeights(teacher_gap_cost=7, non_preferred_period_cost=4)
    weights = convert._soft_weights(custom)
    assert weights[HIDDEN] == default_weights()[HIDDEN]  # not the field's 4: the fast path has no such field
    assert weights["teacher_gap_cost"] == 7  # a constraint with a field keeps it
    assert list(weights) == list(default_weights())


def test_a_selection_maps_names_and_refuses_unknown_ones():
    weights = convert._soft_weights({"teacher_gap_cost": 2, HIDDEN: 9})
    assert weights == dict.fromkeys(default_weights(), 0.0) | {"teacher_gap_cost": 2.0, HIDDEN: 9.0}
    with pytest.raises(ValueError, match="unknown soft weight"):
        convert._soft_weights({"teacher_gap": 1})  # a misspelt name is not taken for a missing field


def test_both_paths_solve_without_the_field(problem, without_field):
    # With constraints=, the pick's own weight scores it.
    picks = [*[spec for spec in get_constraints() if spec.name != HIDDEN], NonPreferredPeriodCost(weight=4)]
    result = st.solve(problem, time_limit_seconds=1.0, random_seed=3, constraints=picks)
    assert result["status"] == "FEASIBLE"
    assert result["objective_value"] == _weighted(result, dict(default_weights(), **{HIDDEN: 4}))
    # The default path: the rescore weighs it at its component default.
    result = st.solve(problem, time_limit_seconds=1.0, random_seed=3)
    assert result["status"] == "FEASIBLE"
    assert result["objective_value"] == _weighted(result, default_weights())


def test_a_weight_the_fast_path_cannot_apply_raises(problem, without_field):
    # The full default dict (what a server sends) is accepted: the hidden weight is at its default.
    result = st.solve(problem, time_limit_seconds=0.5, random_seed=3, weights=default_weights())
    assert result["lessons"]
    with pytest.raises(ValueError, match="not on the default path"):
        st.solve(problem, time_limit_seconds=0.1, weights={HIDDEN: 3})
