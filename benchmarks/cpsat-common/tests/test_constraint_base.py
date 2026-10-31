"""Unit tests for the shared constraint-spec base, ``python/optimaru/_constraint.py``.

The base is pure Python and has no package of its own, so its tests live
here, where CI already runs a pytest job (benchmark-cpsat: cpsat-common)
and where its CP-SAT counterpart (``ObjectiveBuilder``) is tested. The file
is loaded straight from the repository, so the canonical copy is what is
tested; ``scripts/check_constraint_base.py`` keeps the per-package copies
byte-identical to it.

The raw costs below are those of the engine's own level test
(``test/constraint_level_test.cpp``, assignment A on the four-worker toy
domain), and every expected score is a number that test asserts for the
engine: the base must reproduce the engine's arithmetic, the medium fold
included, for any selection.
"""

from __future__ import annotations

import importlib.util
import sys
from dataclasses import FrozenInstanceError, dataclass
from pathlib import Path

import pytest

_PATH = Path(__file__).resolve().parents[3] / "python" / "optimaru" / "_constraint.py"
_SPEC = importlib.util.spec_from_file_location(
    "optimaru_constraint_base_under_test", _PATH
)
base = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = base  # dataclasses resolve their module through sys.modules
_SPEC.loader.exec_module(base)

Constraint = base.Constraint
Score = base.Score
score_costs = base.score_costs
select = base.select
instance_keys = base.instance_keys
shape = base.shape


class ConflictCost(Constraint):
    name = "conflict_cost"


class OverloadCost(Constraint):
    name = "overload_cost"


class IdleWorkerCost(Constraint):
    name = "idle_worker_cost"


class TaskCostCost(Constraint):
    name = "task_cost_cost"


class PreferenceReward(Constraint):
    name = "preference_reward"


class ForbiddenWorkerCost(Constraint):
    name = "forbidden_worker_cost"


DEFAULTS = (
    ConflictCost(),
    OverloadCost(),
    TaskCostCost(),
    PreferenceReward(),
    ForbiddenWorkerCost(),
    IdleWorkerCost(),
)


def _entry(
    costs,
    *,
    level,
    weight=1.0,
    reward=False,
    flippable=True,
    is_global=False,
    fold=None,
    name="",
):
    entry = {
        "costs": costs,
        "weight": weight,
        "exponent": 1.0,
        "hard": level == "hard",
        "global": is_global,
        "level": level,
        "reward": reward,
        "level_flippable": flippable,
        "name": name,
        "label": "",
    }
    if fold is not None:
        entry["medium_fold"] = {"kind": fold[0], "unit": fold[1]}
    return entry


def costs_a(fold=None):
    """constraint_costs() of assignment A = [0,0,0,1,1,3]: what the C++ dict looks like.

    On a domain with a medium fold EVERY entry carries it (not only the medium ones).
    """
    rows = {
        "conflict_cost": ([2.0, 1.0, 0.0, 0.0], {"level": "hard"}),
        "overload_cost": ([1.0, 0.0, 0.0, 0.0], {"level": "medium"}),
        "task_cost_cost": ([15.0, 11.0, 0.0, 2.0], {"level": "soft"}),
        "preference_reward": (
            [-3.0, 0.0, 0.0, 0.0],
            {"level": "soft", "weight": 0.5, "reward": True, "flippable": False},
        ),
        "forbidden_worker_cost": (
            [0.0, 0.0, 0.0, 1.0],
            {"level": "hard", "flippable": False},
        ),
        "idle_worker_cost": ([1.0], {"level": "medium", "is_global": True}),
    }
    return {
        name: _entry(values, fold=fold, name=name, **meta)
        for name, (values, meta) in rows.items()
    }


BELOW_HARD = ("below_hard", 100.0)
INTO_SOFT = ("into_soft", 1000.0)


# --- the medium fold ----------------------------------------------------------


@pytest.mark.parametrize(
    ("selection", "expected"),
    [
        # Every expectation is asserted for the engine in constraint_level_test.cpp.
        (DEFAULTS, (-(4 * 100 + 2), -26.5)),
        (
            [ConflictCost(), ForbiddenWorkerCost()],
            (-400, 0.0),
        ),  # no medium term selected
        ([*DEFAULTS[:1], OverloadCost(hard=False), *DEFAULTS[2:]], (-401, -27.5)),
        (
            [
                *DEFAULTS[:1],
                OverloadCost(hard=False),
                *DEFAULTS[2:5],
                IdleWorkerCost(hard=False),
            ],
            (-400, -28.5),
        ),
        (
            [*DEFAULTS[:1], OverloadCost(weight=0.4), *DEFAULTS[2:]],
            (-401, -26.5),
        ),  # medium rounds per unit
    ],
)
def test_below_hard_folds_the_whole_score(selection, expected):
    """below_hard reports hard * unit + medium whatever the selection holds."""
    assert tuple(
        score_costs(costs_a(BELOW_HARD), select(list(selection), DEFAULTS))
    ) == pytest.approx(expected)


@pytest.mark.parametrize(
    ("selection", "expected"),
    [
        (DEFAULTS, (-4, -(26.5 + 2 * 1000.0))),
        (
            [*DEFAULTS[:1], OverloadCost(hard=True), *DEFAULTS[2:]],
            (-5, -1026.5),
        ),  # 1 hard per raw unit
        (
            [*DEFAULTS[:1], OverloadCost(hard=False), *DEFAULTS[2:]],
            (-4, -1027.5),
        ),  # 1 soft per raw unit
        (
            [*DEFAULTS[:1], OverloadCost(weight=3), *DEFAULTS[2:]],
            (-4, -4026.5),
        ),  # weight in medium units
        ([ConflictCost(), ForbiddenWorkerCost()], (-4, 0.0)),
    ],
)
def test_into_soft_folds_medium_into_soft(selection, expected):
    assert tuple(
        score_costs(costs_a(INTO_SOFT), select(list(selection), DEFAULTS))
    ) == pytest.approx(expected)


def test_a_medium_term_needs_a_fold():
    with pytest.raises(ValueError, match="carry no medium_fold"):
        score_costs(costs_a(None), [OverloadCost()])
    # ... while the same raw costs demoted to soft score fine.
    assert score_costs(
        costs_a(None), [OverloadCost(hard=False), IdleWorkerCost(hard=False)]
    ) == (0, -2.0)


def test_disagreeing_folds_are_refused():
    costs = costs_a(BELOW_HARD)
    costs["task_cost_cost"]["medium_fold"] = {"kind": "into_soft", "unit": 100.0}
    with pytest.raises(ValueError, match="different medium fold"):
        score_costs(costs, [ConflictCost()])


# --- guards ---------------------------------------------------------------------


def test_a_reward_refuses_hard_true():
    with pytest.raises(ValueError, match="is a reward"):
        score_costs(costs_a(INTO_SOFT), [PreferenceReward(hard=True)])
    # hard=False re-states its level: allowed.
    assert score_costs(costs_a(INTO_SOFT), [PreferenceReward(hard=False)]) == (0, 1.5)


def test_a_non_flippable_constraint_keeps_its_level():
    with pytest.raises(ValueError, match="is always hard; hard=False is not allowed"):
        score_costs(costs_a(INTO_SOFT), [ForbiddenWorkerCost(hard=False)])
    assert score_costs(costs_a(INTO_SOFT), [ForbiddenWorkerCost(hard=True)]) == (
        -1,
        0.0,
    )


def test_hard_rounds_per_unit_half_away_from_zero():
    # Raw [2, 1, 0, 0] at weight 2.5: 5.0 + 2.5 -> 5 + 3 (Python's round(2.5) would give 2).
    assert score_costs(costs_a(INTO_SOFT), [ConflictCost(weight=2.5)]).hard == -(5 + 3)


def test_rounding_matches_llround_at_the_double_edges():
    """value + 0.5 rounds in floating point; the exact formula does not."""
    round_half_away = base._round_half_away
    assert round_half_away(0.49999999999999994) == 0
    assert round_half_away(-0.49999999999999994) == 0
    assert round_half_away(4503599627370497.0) == 4503599627370497
    assert round_half_away(-4503599627370497.0) == -4503599627370497
    assert [round_half_away(v) for v in (0.5, -0.5, 1.5, -2.5, 2.4999)] == [1, -1, 2, -3, 2]
    # Through score_costs: weight 1/98 on a raw 49 is 0.49999999999999994 -> 0 (the engine's 0).
    costs = {"conflict_cost": _entry([49.0], level="hard", name="conflict_cost")}
    assert score_costs(costs, [ConflictCost(weight=1 / 98)]).hard == 0


# --- the range of a score (level.hpp "The range of a score") --------------------


def test_the_hard_total_saturates_instead_of_wrapping():
    """Five contributions of 4e18 sum past int64: -(2**63 - 1), never a positive score."""
    costs = {"conflict_cost": _entry([1.0] * 5, level="hard", name="conflict_cost")}
    assert score_costs(costs, [ConflictCost(weight=4e18)]).hard == -(2**63 - 1)
    assert score_costs(costs, [ConflictCost(weight=1e18)]).hard == -5 * 10**18


def test_the_below_hard_fold_saturates():
    """task-assigning's 1e9 fold: 21 hard units at weight 1e9 fold past int64."""
    costs = {"conflict_cost": _entry([1.0] * 21, level="hard", name="conflict_cost", fold=("below_hard", 1e9))}
    assert score_costs(costs, [ConflictCost(weight=1e9)]).hard == -(2**63 - 1)
    assert score_costs(costs, [ConflictCost(weight=1.0)]).hard == -21 * 10**9


@pytest.mark.parametrize(
    ("weight", "exponent", "level"),
    [(1e19, 1.0, "hard"), (1.0, 200.0, "soft"), (1.0, 200.0, "hard"), (0.0, 400.0, "soft")],
)
def test_out_of_range_contributions_raise_overflow_error(weight, exponent, level):
    """A hard contribution of 2**62 or more, or a non-finite one, cannot be scored."""
    costs = {"conflict_cost": _entry([1e3], level=level, name="conflict_cost")}
    with pytest.raises(OverflowError, match="conflict_cost"):
        score_costs(costs, [ConflictCost(weight=weight, exponent=exponent)])


def test_an_unknown_name_is_refused():
    class Typo(Constraint):
        name = "confict_cost"

    with pytest.raises(
        ValueError, match="unknown constraint 'confict_cost'; available: "
    ):
        score_costs(costs_a(INTO_SOFT), [Typo()])


# --- knob validation ------------------------------------------------------------


@pytest.mark.parametrize(
    ("kwargs", "error", "message"),
    [
        ({"weight": -1}, ValueError, "weight must be finite and >= 0"),
        ({"weight": float("nan")}, ValueError, "weight must be finite and >= 0"),
        ({"weight": True}, TypeError, "weight must be a number or None"),
        ({"weight": "2"}, TypeError, "weight must be a number or None"),
        ({"hard": 1}, TypeError, "hard must be True, False or None"),
        ({"hard": "yes"}, TypeError, "hard must be True, False or None"),
        ({"exponent": 0}, ValueError, "exponent must be finite and > 0"),
        ({"exponent": False}, TypeError, "exponent must be a number"),
    ],
)
def test_specs_validate_their_knobs(kwargs, error, message):
    with pytest.raises(error, match=message):
        ConflictCost(**kwargs)


def test_numpy_scalars_are_accepted_like_python_ones():
    numpy = pytest.importorskip("numpy")
    spec = ConflictCost(weight=numpy.float64(2.0), hard=numpy.True_)
    assert score_costs(costs_a(INTO_SOFT), [spec]).hard == -6
    with pytest.raises(TypeError, match="weight must be a number or None"):
        ConflictCost(weight=numpy.True_)


def test_specs_are_frozen_and_need_a_name():
    with pytest.raises(FrozenInstanceError):
        ConflictCost().weight = 2  # type: ignore[misc]
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass

    class _Family(Constraint, abstract=True):
        """An intermediate base needs no name."""

    class Member(_Family):
        name = "member_cost"

    assert Member().name == "member_cost"


# --- select, parameters and repeated instances ------------------------------------


@dataclass(frozen=True)
class DayPatternCost(Constraint):
    name = "day_pattern_cost"
    pattern: str = ""
    worker_indices: tuple[int, ...] | None = None


def test_select_contract():
    assert select(None, DEFAULTS) == list(DEFAULTS)
    with pytest.raises(ValueError, match="must not be empty"):
        select([], DEFAULTS)
    with pytest.raises(ValueError, match="picked more than once"):
        select([ConflictCost(), ConflictCost(weight=2)], DEFAULTS)


def test_params_are_the_non_knob_fields():
    assert DayPatternCost(pattern="A,OFF", weight=2).params() == {
        "pattern": "A,OFF",
        "worker_indices": None,
    }
    assert ConflictCost().params() == {}


def test_repeatable_selections_allow_one_name_with_different_parameters():
    first, second = (
        DayPatternCost(pattern="A,OFF"),
        DayPatternCost(pattern="B,OFF", worker_indices=(1,)),
    )
    assert select([first, second], DEFAULTS, repeatable=True) == [first, second]
    with pytest.raises(ValueError, match="picked more than once"):
        select([first, second], DEFAULTS)  # not repeatable
    with pytest.raises(ValueError, match="with the same parameters"):
        select(
            [first, DayPatternCost(pattern="A,OFF", weight=3)],
            DEFAULTS,
            repeatable=True,
        )


def test_instance_keys_and_scoring_repeated_instances():
    """constraint_costs() keys the k-th instance of a name '<name>#<k>' (python::instanceKeys)."""
    picks = [
        DayPatternCost(pattern="A"),
        ConflictCost(),
        DayPatternCost(pattern="B", weight=2),
    ]
    assert instance_keys(picks) == [
        "day_pattern_cost",
        "conflict_cost",
        "day_pattern_cost#2",
    ]
    costs = {
        "day_pattern_cost": _entry([1.0, 2.0], level="soft", name="day_pattern_cost"),
        "conflict_cost": _entry([1.0], level="hard", name="conflict_cost"),
        "day_pattern_cost#2": _entry([4.0], level="soft", name="day_pattern_cost"),
    }
    assert score_costs(costs, picks) == (-1, -(3.0 + 2 * 4.0))
    with pytest.raises(ValueError, match="picked more often than the costs hold it"):
        score_costs(costs, [*picks, DayPatternCost(pattern="C")])


def test_shape_preserves_the_sign():
    assert shape(3.0, 2.0) == 9.0
    assert shape(-3.0, 2.0) == -9.0
    assert shape(0.0, 0.5) == 0.0
    assert shape(5.0, 1.0) == 5.0


def test_score_is_a_named_tuple():
    result = score_costs(costs_a(INTO_SOFT), [ConflictCost()])
    assert isinstance(result, Score)
    assert (result.hard, result.soft) == (-3, 0.0)
