"""What every use case's `constraints` module and constraint picks share.

The spec base (optimaru/_constraint.py) and the C++ pick reader
(optimaru/python/constraint_binding.hpp) are written once, so what they refuse and how they
say it is checked here for every use case. A use case's own test_constraint_specs.py keeps
what is particular to it: its catalog, and scores worked out by hand.
"""

import functools
import importlib
import pickle
import re
from types import SimpleNamespace

import pytest

import optimaru

from ._build import use_case_module
from ._problems import small_problem

# Its catalog holds more constraints than it scores by default: list_constraints() names all of
# them, get_constraints() the default eight.
CATALOG_BEYOND_THE_DEFAULTS = {"employee_scheduling"}

# Its solve() takes and returns the documents of its API; constraint_costs() is a function of its
# canonical core (canon.constraint_costs), which tests/school_timetabling covers.
COSTS_OFF_THE_CORE_ONLY = {"school_timetabling"}
EVERY_USE_CASE_WITH_COSTS = [name for name in optimaru.__all__ if name not in COSTS_OFF_THE_CORE_ONLY]

# Their calculate_score() takes the picks as well.
SCORES_WITH_PICKS = {
    "assembly_line_balancing",
    "bin_packing",
    "knapsack",
    "max_cut",
    "quadratic_assignment",
    "semiconductor_scheduling",
}


def _specs(use_case):
    """(optimaru.<use case>, its constraints module)."""
    module = use_case_module(use_case)
    return module, importlib.import_module(f"optimaru.{use_case}.constraints")


def _probe(use_case):
    """(module, constraints module, a spec class to probe the knobs with): the first spec of
    get_constraints() that is a penalty whose level may change."""
    module, constraints = _specs(use_case)
    rows = {row.name: row for row in module.constraint_info()}
    spec = next(
        type(spec)
        for spec in constraints.get_constraints()
        if rows[spec.name].level_flippable and not rows[spec.name].reward
    )
    return module, constraints, spec


@functools.cache
def _solved(use_case):
    """A complete solution as the core holds it: what constraint_costs() takes. One per use
    case for the whole run, so a test must not write to it."""
    solved = use_case_module(use_case).solve(small_problem(use_case), time_limit_seconds=0.2, random_seed=1)
    return solved.to_core() if hasattr(solved, "to_core") else solved


def _pick(name, **knobs):
    """A duck-typed pick: what the C++ reader accepts in place of a spec."""
    pick = SimpleNamespace(name=name, weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    return pick


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_constraint_info_rows_are_plain_values(use_case):
    module = use_case_module(use_case)
    rows = module.constraint_info()
    row = rows[0]
    assert isinstance(row, module.ConstraintInfo)
    assert list(row._asdict()) == [
        "name",
        "default_level",
        "default_weight",
        "level_flippable",
        "reward",
        "default_enabled",
    ]
    assert row.default_level in ("hard", "medium", "soft")
    assert tuple(row) == tuple(row._asdict().values())
    assert repr(row).startswith(f"ConstraintInfo(name='{row.name}'")
    assert pickle.loads(pickle.dumps(rows)) == rows
    assert module.ConstraintInfo.__module__ == module._core.__name__


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_a_spec_subclass_must_name_a_constraint(use_case):
    _, constraints = _specs(use_case)
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(constraints.Constraint):
            pass


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_every_spec_has_labels_and_descriptions_in_both_languages(use_case):
    _, constraints = _specs(use_case)
    for spec in constraints.get_constraints():
        for attribute in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(type(spec), attribute).strip(), f"{type(spec).__name__}.{attribute} is empty"


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_get_constraints_defaults_to_the_default_set(use_case):
    module, constraints = _specs(use_case)
    names = [spec.name for spec in constraints.get_constraints()]
    if use_case in CATALOG_BEYOND_THE_DEFAULTS:
        assert names == [row.name for row in module.constraint_info() if row.default_enabled]
    else:  # the default assembly, its opt-in constraints (default weight 0) included
        assert names == module.list_constraints()


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_an_empty_constraint_list_is_refused(use_case):
    """An empty list means "score nothing", which is most likely a mistake."""
    module, constraints = _specs(use_case)
    with pytest.raises(ValueError, match="must not be empty"):
        module.solve(small_problem(use_case), time_limit_seconds=0.2, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        constraints.get_constraints([])


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_an_unknown_constraint_name_is_refused(use_case):
    """A typo must not silently leave the cost function unchanged."""
    module, _, spec = _probe(use_case)
    with pytest.raises(ValueError, match="unknown constraint"):
        module.solve(small_problem(use_case), time_limit_seconds=0.2, constraints=[_pick(spec.name + "s")])


@pytest.mark.parametrize("use_case", EVERY_USE_CASE_WITH_COSTS)
def test_score_refuses_an_unknown_constraint_name(use_case):
    """A spec with a wrong name must not be silently ignored."""
    module, constraints = _specs(use_case)
    raw = module.constraint_costs(_solved(use_case))

    class Misnamed(constraints.Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        constraints.score(raw, [Misnamed()])


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_a_level_must_be_a_level(use_case):
    module, _, spec = _probe(use_case)
    with pytest.raises(TypeError, match="level must be a Level"):
        spec(level=True)
    for level in ("firm", "HARD"):
        with pytest.raises(ValueError, match=f"level must be 'hard', 'medium' or 'soft', not '{level}'"):
            spec(level=level)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        module.solve(small_problem(use_case), time_limit_seconds=0.2, constraints=[_pick(spec.name, level="firm")])


@pytest.mark.parametrize("use_case", optimaru.__all__)
@pytest.mark.parametrize(
    ("knobs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": float("inf")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"exponent": -1.0}, ValueError),
        ({"level": 1}, TypeError),
        ({"level": "firm"}, ValueError),
        ({"offset": float("nan")}, ValueError),
        ({"offset": "1"}, TypeError),
        ({"clip": 0}, TypeError),
    ],
)
def test_specs_validate_their_knobs(use_case, knobs, error):
    _, _, spec = _probe(use_case)
    with pytest.raises(error):
        spec(**knobs)


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_a_spec_has_no_hard_keyword(use_case):
    """The level is the `level` knob: `hard` is an unknown keyword, not a knob that is checked."""
    _, _, spec = _probe(use_case)
    for value in (1, True):
        with pytest.raises(TypeError, match="unexpected keyword argument 'hard'"):
            spec(hard=value)


@pytest.mark.parametrize("use_case", optimaru.__all__)
@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_the_core_refuses_the_types_the_python_base_refuses(use_case, knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    module, _, spec = _probe(use_case)
    with pytest.raises(TypeError, match=f"constraint '{spec.name}': {re.escape(message)}"):
        module.solve(small_problem(use_case), time_limit_seconds=0.1, constraints=[_pick(spec.name, **knobs)])
    with pytest.raises(TypeError, match=re.escape(message)):
        spec(**knobs)


@pytest.mark.parametrize("use_case", optimaru.__all__)
@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": -2.0}, "weight must be finite and >= 0"),
        ({"exponent": 0.0}, "exponent must be finite and > 0"),
        ({"offset": float("inf")}, "offset must be finite, not inf"),
        ({"offset": float("nan")}, "offset must be finite, not nan"),
    ],
)
def test_the_core_refuses_invalid_knobs_from_duck_typed_picks(use_case, knobs, message):
    """A pick that is not a spec skipped the spec's own checks: the core makes them again,
    on every path that reads picks."""
    module, _, spec = _probe(use_case)
    pick = _pick(spec.name, **knobs)
    message = re.escape(message)
    with pytest.raises(ValueError, match=message):
        module.solve(small_problem(use_case), time_limit_seconds=0.1, constraints=[pick])
    if use_case not in COSTS_OFF_THE_CORE_ONLY:
        with pytest.raises(ValueError, match=message):
            module.constraint_costs(_solved(use_case), [pick])
    if use_case in SCORES_WITH_PICKS:
        with pytest.raises(ValueError, match=message):
            module.calculate_score(_solved(use_case), [pick])
    with pytest.raises(ValueError, match=message):
        spec(**knobs)
