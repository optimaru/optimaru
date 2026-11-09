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

import copy
import dataclasses
import importlib.util
import pickle
import struct
import sys
from dataclasses import FrozenInstanceError, dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

import numpy as np
import pytest

_PATH = Path(__file__).resolve().parents[3] / "python" / "optimaru" / "_constraint.py"
_SPEC = importlib.util.spec_from_file_location(
    "optimaru_constraint_base_under_test", _PATH
)
base = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = base  # dataclasses resolve their module through sys.modules
_SPEC.loader.exec_module(base)

Constraint = base.Constraint
Level = base.Level
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
        "offset": 0.0,  # constraint_costs() always reports offset / clip
        "clip": True,
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
        ([*DEFAULTS[:1], OverloadCost(level="soft"), *DEFAULTS[2:]], (-401, -27.5)),
        (
            [
                *DEFAULTS[:1],
                OverloadCost(level="soft"),
                *DEFAULTS[2:5],
                IdleWorkerCost(level="soft"),
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
            [*DEFAULTS[:1], OverloadCost(level="hard"), *DEFAULTS[2:]],
            (-5, -1026.5),
        ),  # 1 hard per raw unit
        (
            [*DEFAULTS[:1], OverloadCost(level="soft"), *DEFAULTS[2:]],
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
        costs_a(None), [OverloadCost(level="soft"), IdleWorkerCost(level="soft")]
    ) == (0, -2.0)


def test_disagreeing_folds_are_refused():
    costs = costs_a(BELOW_HARD)
    costs["task_cost_cost"]["medium_fold"] = {"kind": "into_soft", "unit": 100.0}
    with pytest.raises(ValueError, match="different medium fold"):
        score_costs(costs, [ConflictCost()])


# --- guards ---------------------------------------------------------------------


def test_a_reward_refuses_hard_true():
    with pytest.raises(ValueError, match="is a reward"):
        score_costs(costs_a(INTO_SOFT), [PreferenceReward(level="hard")])
    # level="soft" re-states its level: allowed.
    assert score_costs(costs_a(INTO_SOFT), [PreferenceReward(level="soft")]) == (0, 1.5)


def test_a_non_flippable_constraint_keeps_its_level():
    with pytest.raises(ValueError, match="is always hard; level='soft' is not allowed"):
        score_costs(costs_a(INTO_SOFT), [ForbiddenWorkerCost(level="soft")])
    assert score_costs(costs_a(INTO_SOFT), [ForbiddenWorkerCost(level="hard")]) == (
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
    [(1e19, 1.0, "hard"), (1.0, 200.0, "soft"), (1.0, 200.0, "hard")],
)
def test_out_of_range_contributions_raise_overflow_error(weight, exponent, level):
    """A hard contribution of 2**62 or more, or a non-finite one, cannot be scored."""
    costs = {"conflict_cost": _entry([1e3], level=level, name="conflict_cost")}
    with pytest.raises(OverflowError, match="conflict_cost"):
        score_costs(costs, [ConflictCost(weight=weight, exponent=exponent)])


@pytest.mark.parametrize("level", ["hard", "soft", "medium"])
@pytest.mark.parametrize("exponent", [400.0, 2000.0, 1e300])
def test_weight_zero_contributes_exactly_zero_at_any_exponent(level, exponent):
    """An opt-in constraint at weight 0 is 0 without shaping its cost (no 0 * inf = nan).

    The engine asserts the same (constraint_level_test.cpp (l)).
    """
    fold = BELOW_HARD if level == "medium" else None
    costs = {
        "conflict_cost": _entry([1e3, -1e3, 0.0], level=level, name="conflict_cost", fold=fold),
        "task_cost_cost": _entry([2.0], level="soft", name="task_cost_cost", fold=fold),
    }
    result = score_costs(costs, [ConflictCost(weight=0.0, exponent=exponent), TaskCostCost()])
    assert result == (0, -2.0)
    assert score_costs(costs, [ConflictCost(weight=0, exponent=exponent)]) == (0, 0.0)


def test_unset_knobs_mean_the_default_even_for_costs_of_another_selection():
    """constraint_costs(x, picks) carries the picks' weight / level and the defaults.

    employee-scheduling and school-timetabling compute costs for a selection: an
    entry there holds that selection's weight and level, and ``default_weight`` /
    ``default_level`` (what weight=None / level=None mean). Re-pricing with
    another selection must read the defaults, not the first selection's knobs.
    """
    costs = {
        "conflict_cost": {
            **_entry([2.0, 1.0], level="soft", weight=5.0, name="conflict_cost"),
            "default_level": "hard",
            "default_weight": 1.0,
        },
        "overload_cost": {
            **_entry([1.0], level="hard", name="overload_cost", fold=INTO_SOFT),
            "default_level": "medium",
            "default_weight": 1.0,
        },
    }
    costs["conflict_cost"]["medium_fold"] = costs["overload_cost"]["medium_fold"]
    # Unset knobs: conflict back at hard x1 (3 hard), overload back at medium (1 x 1000 soft).
    assert score_costs(costs, [ConflictCost(), OverloadCost()]) == (-3, -1000.0)
    # The first selection's knobs, stated explicitly, reproduce its score.
    assert score_costs(costs, [ConflictCost(weight=5.0, level="soft"), OverloadCost(level="hard")]) == (-1, -15.0)
    # An entry without the default keys (hand-made) reads level / weight.
    hand_made = {"conflict_cost": _entry([2.0, 1.0], level="soft", weight=5.0, name="conflict_cost")}
    assert score_costs(hand_made, [ConflictCost()]) == (0, -15.0)
    # ... and one with neither level key is refused, naming what it needs.
    del hand_made["conflict_cost"]["level"]
    with pytest.raises(ValueError, match="needs 'default_level' or 'level'"):
        score_costs(hand_made, [ConflictCost()])
    # ... and so is one with neither weight key, when the pick leaves its weight unset.
    hand_made["conflict_cost"]["level"] = "soft"
    del hand_made["conflict_cost"]["weight"]
    with pytest.raises(ValueError, match="'conflict_cost' needs 'default_weight' or 'weight'"):
        score_costs(hand_made, [ConflictCost()])
    assert score_costs(hand_made, [ConflictCost(weight=5.0)]) == (0, -15.0)


def test_a_non_flippable_check_uses_the_default_level():
    costs = {
        "forbidden_worker_cost": {
            **_entry([1.0], level="hard", flippable=False, name="forbidden_worker_cost"),
            "default_level": "hard",
            "default_weight": 1.0,
        }
    }
    with pytest.raises(ValueError, match="is always hard; level='soft' is not allowed"):
        score_costs(costs, [ForbiddenWorkerCost(level="soft")])
    assert score_costs(costs, [ForbiddenWorkerCost(level="hard")]) == (-1, 0.0)


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
        ({"level": 1}, TypeError, "level must be a Level, its name or None"),
        ({"level": True}, TypeError, "level must be a Level, its name or None"),
        ({"level": "yes"}, ValueError, "level must be 'hard', 'medium' or 'soft'"),
        ({"exponent": 0}, ValueError, "exponent must be finite and > 0"),
        ({"exponent": False}, TypeError, "exponent must be a number"),
        ({"offset": float("nan")}, ValueError, "offset must be finite, not nan"),
        ({"offset": float("-inf")}, ValueError, "offset must be finite, not -inf"),
        ({"offset": "10"}, TypeError, "offset must be a number, not '10'"),
        ({"offset": True}, TypeError, "offset must be a number, not True"),
        ({"offset": None}, TypeError, "offset must be a number, not None"),
        ({"clip": 1}, TypeError, "clip must be True or False, not 1"),
        ({"clip": None}, TypeError, "clip must be True or False, not None"),
        ({"clip": "no"}, TypeError, "clip must be True or False, not 'no'"),
    ],
)
def test_specs_validate_their_knobs(kwargs, error, message):
    with pytest.raises(error, match=message):
        ConflictCost(**kwargs)


def test_numpy_scalars_are_accepted_like_python_ones():
    numpy = pytest.importorskip("numpy")
    spec = ConflictCost(weight=numpy.float64(2.0))
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
    # A name may be picked several times: each pick is an instance.
    picks = [ConflictCost(), ConflictCost(weight=2)]
    assert select(picks, DEFAULTS) == picks
    assert select([ConflictCost(), ConflictCost()], DEFAULTS) == [ConflictCost(), ConflictCost()]


def test_offset_and_clip_are_keyword_only_and_default_to_the_plain_shaping():
    spec = ConflictCost(2, None, 3.0)
    assert (spec.weight, spec.level, spec.exponent, spec.offset, spec.clip) == (2, None, 3.0, 0.0, True)
    with pytest.raises(TypeError):
        ConflictCost(2, None, 3.0, 5.0)  # offset is keyword-only
    assert ConflictCost(offset=-2.5, clip=False).offset == -2.5
    assert DayPatternCost(None, None, 1.0, "A,OFF").pattern == "A,OFF"  # parameters stay positional
    assert "offset" not in DayPatternCost().params() and "clip" not in DayPatternCost().params()


def test_params_are_the_non_knob_fields():
    assert DayPatternCost(pattern="A,OFF", weight=2).params() == {
        "pattern": "A,OFF",
        "worker_indices": None,
    }
    assert ConflictCost().params() == {}


@dataclass(frozen=True)
class ScopedLimitCost(Constraint):
    name = "scoped_limit_cost"
    limit: Any = 0
    scope: Any = None


@pytest.mark.parametrize(
    ("first", "second"),
    [
        # equal parameters, whatever their types
        ({"limit": 1}, {"limit": 1.0}),
        ({"limit": 2}, {"limit": np.int64(2)}),
        ({"scope": [1, 2]}, {"scope": (1.0, np.int32(2))}),
        ({"scope": np.array([1, 2])}, {"scope": [1, 2]}),
        ({"scope": True}, {"scope": np.bool_(True)}),
        # different parameters
        ({"limit": 1}, {"limit": True}),
        ({"limit": 1}, {"limit": 1.5}),
        ({"scope": [1, 2]}, {"scope": [2, 1]}),
        ({"scope": {"a": 1}}, {"scope": {"a": 2}}),
    ],
)
def test_two_picks_of_a_parametrized_constraint_are_two_instances(first, second):
    """Equal or not, two picks are two instances, each scored from its own entry.

    select() keeps both, they get the keys ``name`` and ``name#2``, and
    score_costs reads each instance's own raw costs (a parametrized
    constraint's instances may differ, so the plain entry does not stand in
    for the second one).
    """
    picks = [ScopedLimitCost(**first), ScopedLimitCost(**second, weight=2)]
    assert select(picks, DEFAULTS) == picks
    assert instance_keys(picks) == ["scoped_limit_cost", "scoped_limit_cost#2"]
    costs = {
        "scoped_limit_cost": _entry([1.0, 2.0], level="soft", name="scoped_limit_cost"),
        "scoped_limit_cost#2": _entry([5.0, 0.0], level="soft", name="scoped_limit_cost"),
    }
    assert score_costs(costs, picks) == (0, -(3.0 + 2 * 5.0))
    with pytest.raises(ValueError, match="picked more often than the costs hold it"):
        score_costs({"scoped_limit_cost": costs["scoped_limit_cost"]}, picks)


def test_the_picked_more_often_message():
    """A parametrized constraint picked twice, costs that hold it once: the exact message."""
    costs = {"day_pattern_cost": _entry([1.0], level="soft", name="day_pattern_cost")}
    picks = [DayPatternCost(pattern="A"), DayPatternCost(pattern="B")]
    with pytest.raises(ValueError) as refused:
        score_costs(costs, picks)
    assert str(refused.value) == (
        "constraint 'day_pattern_cost' is picked more often than the costs hold it "
        "('day_pattern_cost#2' is missing); compute constraint_costs() with the same constraint list"
    )
    assert score_costs(costs, picks[:1]) == (0, -1.0)


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


def test_a_parameterless_repeat_reads_the_plain_entry_with_its_own_knobs():
    """Every instance of a constraint without parameters has the same raw costs.

    So costs computed for the defaults (the name once) can score
    ``[TaskCostCost(), TaskCostCost(exponent=2, offset=5)]``: the second pick
    reads the plain entry with its own knobs (constraint_level_test.cpp (p):
    28 + max(0, x - 5)^2 = 28 + 136).
    """
    picks = [TaskCostCost(), TaskCostCost(exponent=2.0, offset=5.0)]
    assert instance_keys(picks) == ["task_cost_cost", "task_cost_cost#2"]
    assert score_costs(costs_a(), picks) == (0, -164.0)
    # Costs that do hold the k-th instance are read as they are.
    held = {**costs_a(), "task_cost_cost#2": _entry([1.0, 0.0, 0.0, 0.0], level="soft", name="task_cost_cost")}
    assert score_costs(held, [TaskCostCost(), TaskCostCost(weight=3)]) == (0, -(28.0 + 3.0))
    # Identical picks count twice.
    assert score_costs(costs_a(), [TaskCostCost(), TaskCostCost()]) == (0, -56.0)


def test_the_repeated_assembly_of_the_engine_test_by_hand():
    """constraint_level_test.cpp (p) repeated<SoftFoldTasks>(): -4hard / -6155.5soft (IntoSoft 1000)."""
    picks = [
        *DEFAULTS,
        TaskCostCost(exponent=2.0, offset=5.0),
        TaskCostCost(weight=0.5, offset=10.0, clip=False),
        ConflictCost(level="soft", offset=1.0, clip=False),
        IdleWorkerCost(offset=1.0, exponent=2.0, clip=False),
        OverloadCost(offset=-1.0, weight=2.0),
    ]
    assert score_costs(costs_a(INTO_SOFT), picks) == (-4, -6155.5)


# --- offset / clip --------------------------------------------------------------


def test_shape_with_an_offset_threshold_and_literal():
    """The engine's shapeCost, step for step (constraint_level_test.cpp (o))."""
    assert shape(15.0, 1.0, 5.0) == 10.0
    assert shape(3.0, 1.0, 5.0) == 0.0
    assert shape(0.0, 2.0, 5.0) == 0.0
    assert shape(-8.0, 2.0, 5.0) == -9.0
    assert shape(15.0, 3.0, 10.0, clip=False) == 125.0
    assert shape(7.0, 3.0, 10.0, clip=False) == -27.0
    assert shape(0.0, 1.0, 10.0, clip=False) == -10.0
    assert shape(4.0, 2.0, -1.0) == 25.0
    assert shape(0.0, 2.0, -1.0) == 0.0
    assert shape(-4.0, 1.0, -1.0) == -5.0
    assert shape(0.0, 2.0, -1.0, clip=False) == 1.0


@pytest.mark.parametrize("cost", [-7.5, -1.0, -0.0, 0.0, 0.3, 1.0, 2.0, 17.25, 1e10])
@pytest.mark.parametrize("exponent", [0.5, 1.0, 2.0, 3.0])
@pytest.mark.parametrize("clip", [True, False])
def test_offset_zero_is_the_plain_power_bit_for_bit(cost, exponent, clip):
    plain = cost if exponent == 1.0 or cost == 0.0 else (-((-cost) ** exponent) if cost < 0.0 else cost**exponent)
    shifted = shape(cost, exponent, 0.0, clip)
    assert struct.pack("<d", shifted) == struct.pack("<d", plain)


def _tasks_only(costs):
    return {name: entry for name, entry in costs.items() if name in ("task_cost_cost", "preference_reward", "conflict_cost")}


@pytest.mark.parametrize(
    ("pick", "soft"),
    [
        (TaskCostCost(offset=5.0), 16.0),  # [10, 6, 0, 0]
        (TaskCostCost(offset=5.0, clip=False), 8.0),  # [10, 6, -5, -3]: the empty worker is rewarded too
        (TaskCostCost(offset=5.0, exponent=2.0), 136.0),
        (TaskCostCost(offset=5.0, exponent=2.0, clip=False), 102.0),  # 100 + 36 - 25 - 9
        (TaskCostCost(offset=-1.0), 31.0),  # [16, 12, 0, 3]
        (TaskCostCost(offset=-1.0, clip=False), 32.0),  # [16, 12, 1, 3]
    ],
)
def test_unit_offsets_by_hand(pick, soft):
    """Per unit, every number asserted for the engine in constraint_level_test.cpp (o)."""
    assert score_costs(_tasks_only(costs_a()), [pick]) == (0, -soft)


def test_offsets_on_a_reward_and_on_a_hard_constraint():
    costs = _tasks_only(costs_a())
    assert score_costs(costs, [TaskCostCost(), PreferenceReward(offset=1.0)]) == (0, -27.0)
    assert score_costs(costs, [TaskCostCost(), PreferenceReward(offset=1.0, clip=False)]) == (0, -24.5)
    # Threshold on hard: [1.5, 0.5, 0, 0] rounded per unit, half away from zero: 2 + 1.
    assert score_costs(costs, [ConflictCost(offset=0.5)]) == (-3, 0.0)
    # The literal formula keeps its numbers on soft (hard refuses it):
    # [1.5, 0.5, -0.5, -0.5] = 1 and [1, 0, -1, -1] = -1, not rounded.
    assert score_costs(costs, [ConflictCost(level="soft", offset=0.5, clip=False)]) == (0, -1.0)
    assert score_costs(costs, [ConflictCost(level="soft", offset=1.0, clip=False)]) == (0, 1.0)


def test_a_global_is_shaped_on_its_total():
    costs = {"idle_worker_cost": _entry([2.0], level="soft", is_global=True, name="idle_worker_cost")}
    assert score_costs(costs, [IdleWorkerCost(offset=1.0, exponent=2.0)]) == (0, -1.0)
    assert score_costs(costs, [IdleWorkerCost(offset=3.0, exponent=2.0, clip=False)]) == (0, 1.0)
    assert score_costs(costs, [IdleWorkerCost(offset=3.0)]) == (0, 0.0)


def test_literal_offsets_charge_only_the_units_a_scoped_constraint_evaluates():
    """constraint_costs() names a scoped constraint's ``units``; the other cells are never evaluated."""
    scoped = _entry([3.0, 0.0, 0.0, 0.0], level="soft", name="task_cost_cost")
    scoped["units"] = [0, 2]
    pick = TaskCostCost(offset=1.0, clip=False)
    assert score_costs({"task_cost_cost": scoped}, [pick]) == (0, -1.0)  # (3 - 1) + (0 - 1)
    unscoped = _entry([3.0, 0.0, 0.0, 0.0], level="soft", name="task_cost_cost")
    assert score_costs({"task_cost_cost": unscoped}, [pick]) == (0, 1.0)  # 2 - 1 - 1 - 1
    # Threshold mode and offset 0 do not depend on it (the unevaluated cells are 0).
    assert score_costs({"task_cost_cost": scoped}, [TaskCostCost(offset=1.0)]) == (0, -2.0)
    assert score_costs({"task_cost_cost": scoped}, [TaskCostCost()]) == (0, -3.0)


def test_weight_zero_contributes_exactly_zero_at_any_offset():
    costs = _tasks_only(costs_a())
    assert score_costs(costs, [TaskCostCost(weight=0, offset=50.0, clip=False, exponent=400.0)]) == (0, 0.0)


@pytest.mark.parametrize(("level", "fold"), [("soft", None), ("medium", INTO_SOFT)])
def test_a_literal_overflow_keeps_the_sign_of_the_shifted_cost(level, fold):
    """(1 - 1000)^201 is -inf in the engine's pow(); the mirror reports the same.

    Python's float power raises OverflowError instead, so ``_contribution``
    takes ``copysign(inf, cost - offset)``: the sign of the NEGATIVE base,
    not of the raw cost 1.
    """
    costs = {"task_cost_cost": _entry([1.0], level=level, name="task_cost_cost", fold=fold)}
    with pytest.raises(OverflowError) as refused:
        score_costs(costs, [TaskCostCost(offset=1000.0, exponent=201.0, clip=False)])
    assert str(refused.value) == (
        "constraint 'task_cost_cost': its contribution weight * cost^exponent is not finite (-inf); "
        "lower its weight or exponent"
    )
    with pytest.raises(OverflowError, match=r"is not finite \(inf\)"):
        score_costs(costs, [TaskCostCost(offset=-1000.0, exponent=201.0)])
    with pytest.raises(OverflowError, match=r"is not finite \(inf\)"):
        score_costs(costs, [TaskCostCost(offset=-1000.0, exponent=201.0, clip=False)])


def test_on_hard_the_literal_refusal_comes_before_the_overflow():
    costs = {"conflict_cost": _entry([1.0], level="hard", name="conflict_cost")}
    with pytest.raises(ValueError, match="cannot be hard"):
        score_costs(costs, [ConflictCost(offset=1000.0, exponent=201.0, clip=False)])


# --- clip=False with an offset stays off the hard score ---------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)
MEDIUM_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be medium here, where medium counts toward "
    "the hard score; use clip=True (a threshold) or level='soft'"
)


@pytest.mark.parametrize("offset", [2.0, -2.0])
@pytest.mark.parametrize(
    ("pick", "fold", "message"),
    [
        (lambda o: ConflictCost(offset=o, clip=False), INTO_SOFT, HARD_REFUSAL),  # default-level hard
        (lambda o: ConflictCost(offset=o, clip=False), None, HARD_REFUSAL),
        (lambda o: ConflictCost(level="hard", offset=o, clip=False), INTO_SOFT, HARD_REFUSAL),
        (lambda o: TaskCostCost(level="hard", offset=o, clip=False), INTO_SOFT, HARD_REFUSAL),
        (lambda o: ForbiddenWorkerCost(offset=o, clip=False), INTO_SOFT, HARD_REFUSAL),  # non-flippable
        (lambda o: OverloadCost(level="hard", offset=o, clip=False), BELOW_HARD, HARD_REFUSAL),
        (lambda o: OverloadCost(offset=o, clip=False), BELOW_HARD, MEDIUM_REFUSAL),  # default-level medium
        (lambda o: TaskCostCost(level="medium", offset=o, clip=False), BELOW_HARD, MEDIUM_REFUSAL),
        (lambda o: IdleWorkerCost(offset=o, clip=False), BELOW_HARD, MEDIUM_REFUSAL),  # a global
    ],
)
def test_the_literal_offset_is_refused_where_it_counts_toward_hard(pick, fold, message, offset):
    """Positive: a hard reward (slack cancels violations). Negative: a cost without a violation."""
    spec = pick(offset)
    with pytest.raises(ValueError) as refused:
        score_costs(costs_a(fold), [spec])
    assert str(refused.value) == message.format(q=repr(spec.name))
    # The same pick with the threshold, or on soft, scores.
    score_costs(costs_a(fold), [type(spec)(level=spec.level, offset=offset)])
    if spec.name != "forbidden_worker_cost":
        score_costs(costs_a(fold), [type(spec)(level="soft", offset=offset, clip=False)])


@pytest.mark.parametrize(
    ("picks", "fold", "expected"),
    [
        # clip=True on hard: conflict [2, 1, 0, 0] beyond 1 -> [1, 0, 0, 0].
        ([ConflictCost(offset=1.0)], None, (-1, 0.0)),
        ([ConflictCost(offset=-1.0)], None, (-5, 0.0)),  # [3, 2, 0, 0]
        # clip=False with offset 0 on hard: the plain shaping.
        ([ConflictCost(clip=False)], None, (-3, 0.0)),
        ([ForbiddenWorkerCost(offset=0.0, clip=False)], INTO_SOFT, (-1, 0.0)),
        # clip=False with an offset on soft: [1, 0, -1, -1] and [3, 2, 1, 1].
        ([ConflictCost(level="soft", offset=1.0, clip=False)], None, (0, 1.0)),
        ([ConflictCost(level="soft", offset=-1.0, clip=False)], None, (0, -7.0)),
        # ... and on medium where it folds into soft: [0, -1, -1, -1] x 1000.
        ([OverloadCost(offset=1.0, clip=False)], INTO_SOFT, (0, 3000.0)),
        # A threshold on medium below hard: overload [1, 0, 0, 0] beyond 0.5 rounds to 1.
        ([OverloadCost(offset=0.5)], BELOW_HARD, (-1, 0.0)),
    ],
)
def test_the_allowed_offset_combinations(picks, fold, expected):
    assert score_costs(costs_a(fold), picks) == pytest.approx(expected)


def test_a_repeat_is_checked_on_its_own_level():
    """The rule applies per instance: a soft literal copy next to the hard default is fine."""
    picks = [ConflictCost(), ConflictCost(level="soft", offset=1.0, clip=False)]
    assert score_costs(costs_a(), picks) == (-3, 1.0)
    with pytest.raises(ValueError, match=r"constraint 'conflict_cost': clip=False .* cannot be hard"):
        score_costs(costs_a(), [ConflictCost(), ConflictCost(offset=1.0, clip=False)])


def test_costs_without_clip_cannot_score_the_literal_formula():
    """An entry without "clip" (built by hand) does not name a scoped constraint's units."""
    old = {"task_cost_cost": _entry([15.0, 11.0, 0.0, 2.0], level="soft", name="task_cost_cost")}
    del old["task_cost_cost"]["offset"], old["task_cost_cost"]["clip"]
    with pytest.raises(ValueError) as refused:
        score_costs(old, [TaskCostCost(offset=5.0, clip=False)])
    assert str(refused.value) == (
        "constraint 'task_cost_cost': these costs carry no 'clip' and do not say which units the "
        "constraint evaluates; compute them with constraint_costs() before scoring clip=False with an offset"
    )
    # Everything that does not depend on the evaluated units still scores.
    assert score_costs(old, [TaskCostCost()]) == (0, -28.0)
    assert score_costs(old, [TaskCostCost(offset=5.0)]) == (0, -16.0)
    assert score_costs(old, [TaskCostCost(clip=False)]) == (0, -28.0)
    # constraint_costs() entries carry "clip" and score it: [10, 6, -5, -3].
    assert score_costs(_tasks_only(costs_a()), [TaskCostCost(offset=5.0, clip=False)]) == (0, -8.0)


# --- a numeric knob counts as the double the engine reads --------------------------

_TINY = [Fraction(1, 10**400), np.longdouble("1e-4000")]  # != 0, but 0.0 as a double


@pytest.mark.parametrize("offset", _TINY, ids=["Fraction", "longdouble"])
def test_an_offset_that_is_zero_as_a_double_is_no_offset(offset):
    """The engine reads float(offset) (py::float_) and sees 0.0, so the pick is allowed there.

    The mirror checks and shapes the same double: the literal pick on hard is not
    refused, and it scores exactly as offset 0.
    """
    assert offset != 0 and float(offset) == 0.0
    pick = ConflictCost(offset=offset, clip=False)  # default-level hard
    assert score_costs(costs_a(), [pick]) == score_costs(costs_a(), [ConflictCost()]) == (-3, 0.0)
    # The literal-offset rule on its own sees the same double.
    base._check_literal_offset("conflict_cost", "hard", offset, False, None)
    assert base._check_knobs("conflict_cost", None, None, 1.0, offset, False) == (None, 1.0, 0.0)


def test_numeric_knobs_are_shaped_as_doubles():
    """A numpy.longdouble or numpy.float32 knob is scored as the double the engine scores."""
    costs = _tasks_only(costs_a())  # task_cost_cost [15, 11, 0, 2] on soft
    for kind in (np.longdouble, np.float32):
        pick = TaskCostCost(weight=kind("0.1"), offset=kind("0.1"), exponent=kind("1.5"), clip=False)
        as_doubles = TaskCostCost(
            weight=float(kind("0.1")), offset=float(kind("0.1")), exponent=float(kind("1.5")), clip=False
        )
        result = score_costs(costs, [pick])
        assert type(result.soft) is float
        assert result == score_costs(costs, [as_doubles])


def test_knob_ranges_are_checked_on_the_double():
    """The engine's checks see float(value): an exponent that is 0.0 as a double is refused, a
    weight that is -0.0 is not."""
    with pytest.raises(ValueError) as refused:
        TaskCostCost(exponent=Fraction(1, 10**400))
    assert str(refused.value).startswith("constraint 'task_cost_cost': exponent must be finite and > 0, not Fraction(1, ")
    pick = TaskCostCost(weight=Fraction(-1, 10**400))  # -0.0 as a double: weight 0
    assert score_costs(_tasks_only(costs_a()), [pick]) == (0, 0.0)


# --- the sign rule: a raw cost that can be negative on this problem stays off hard -------

SIGN_REFUSAL = "constraint {q} can have a negative raw cost on this problem (a bonus) and cannot be {where}"
MEDIUM_HERE = "medium here, where medium counts toward the hard score"


def _signed_costs(fold=None):
    """costs_a() where task_cost_cost (a unit constraint) and idle_worker_cost (a global) are
    marked "signed", as constraint_costs() marks an objective over data that can be negative."""
    costs = costs_a(fold)
    for name in ("task_cost_cost", "idle_worker_cost"):
        costs[name] = {**costs[name], "signed": True}
    return costs


@pytest.mark.parametrize(
    ("pick", "fold", "where"),
    [
        (TaskCostCost(level="hard"), None, "hard"),
        (TaskCostCost(level="hard", offset=5.0), None, "hard"),  # a threshold does not help
        (IdleWorkerCost(level="hard"), INTO_SOFT, "hard"),
        (IdleWorkerCost(), BELOW_HARD, MEDIUM_HERE),  # its default level, medium below hard
        (TaskCostCost(level="medium"), BELOW_HARD, MEDIUM_HERE),
    ],
)
def test_a_signed_entry_is_refused_where_it_counts_toward_hard(pick, fold, where):
    with pytest.raises(ValueError) as refused:
        score_costs(_signed_costs(fold), [pick])
    assert str(refused.value) == SIGN_REFUSAL.format(q=repr(pick.name), where=where)
    # Not signed, the same pick scores; signed, the pick on soft (or into-soft medium) scores.
    score_costs(costs_a(fold), [pick])
    score_costs(_signed_costs(fold), [type(pick)(level="soft", offset=pick.offset)])
    score_costs(_signed_costs(INTO_SOFT), [type(pick)(level="medium")])


def test_the_engine_reads_every_pick_before_it_resets_or_scores():
    """The engine refuses a pick's knobs when it reads the pick, the sign rule when it resets
    (in its calculator's order: unit constraints, then globals; first instances in the
    assembly's order, then repeats) and an overflow when it scores. score() follows that order,
    so the same selection fails with the same error whatever the pick order."""
    costs = _signed_costs(INTO_SOFT)
    literal = ConflictCost(offset=1.0, clip=False)  # refused when the engine reads it
    overflowing = TaskCostCost(weight=1e300, exponent=10.0)  # refused when it scores
    for picks in ([overflowing, literal], [IdleWorkerCost(level="hard"), literal]):
        with pytest.raises(ValueError, match="clip=False with a non-zero offset cannot be hard"):
            score_costs(costs, picks)
    with pytest.raises(ValueError) as refused:
        score_costs(costs, [IdleWorkerCost(level="hard"), TaskCostCost(level="hard")])
    assert str(refused.value) == SIGN_REFUSAL.format(q="'task_cost_cost'", where="hard")  # the unit one first
    costs["conflict_cost"] = {**costs["conflict_cost"], "signed": True}  # hard by default
    for picks in (
        [TaskCostCost(level="hard"), ConflictCost()],  # first instances in the assembly's order
        [TaskCostCost(), TaskCostCost(level="hard"), ConflictCost()],  # a repeat after them
    ):
        with pytest.raises(ValueError) as refused:
            score_costs(costs, picks)
        assert str(refused.value) == SIGN_REFUSAL.format(q="'conflict_cost'", where="hard")
    with pytest.raises(OverflowError):
        score_costs(costs, [overflowing])


def test_duck_typed_picks_may_leave_offset_and_clip_unset():
    """name / weight / level / exponent are required; offset / clip default to 0.0 / True."""

    class Pick:
        name = "task_cost_cost"
        weight = None
        level = None
        exponent = 1.0

    assert score_costs(_tasks_only(costs_a()), [Pick()]) == (0, -28.0)

    class Literal(Pick):
        name = "conflict_cost"
        offset = 1.0
        clip = False

    with pytest.raises(ValueError, match="cannot be hard"):
        score_costs(_tasks_only(costs_a()), [Literal()])

    class PickWithoutLevel:  # carries no `level` attribute
        name = "task_cost_cost"
        weight = None
        hard = False
        exponent = 1.0

    with pytest.raises(AttributeError, match="level"):
        score_costs(_tasks_only(costs_a()), [PickWithoutLevel()])


def test_shape_preserves_the_sign():
    assert shape(3.0, 2.0) == 9.0
    assert shape(-3.0, 2.0) == -9.0
    assert shape(0.0, 0.5) == 0.0
    assert shape(5.0, 1.0) == 5.0


def test_score_is_a_named_tuple():
    result = score_costs(costs_a(INTO_SOFT), [ConflictCost()])
    assert isinstance(result, Score)
    assert (result.hard, result.soft) == (-3, 0.0)


def test_a_level_can_be_named_or_a_member():
    for chosen in (Level.SOFT, "soft"):
        assert score_costs(costs_a(INTO_SOFT), [OverloadCost(level=chosen)]) == score_costs(
            costs_a(INTO_SOFT), [OverloadCost(level=Level.SOFT)]
        )


def test_a_level_is_a_string_for_json_selections():
    assert Level.MEDIUM == "medium"
    assert [level.value for level in Level] == ["hard", "medium", "soft"]


def test_a_spec_survives_copies_pickles_and_replace():
    pick = DayPatternCost(2.0, "hard", 1.5, offset=1.0, pattern="A,OFF")
    assert pickle.loads(pickle.dumps(pick)) == pick
    assert copy.copy(pick) == pick and copy.deepcopy(pick) == pick
    assert dataclasses.replace(pick, weight=3.0) == DayPatternCost(3.0, "hard", 1.5, offset=1.0, pattern="A,OFF")
    for unknown in ({"colour": "red"}, {"hard": True}):
        with pytest.raises(TypeError, match=f"unexpected keyword argument '{next(iter(unknown))}'"):
            OverloadCost(**unknown)
    with pytest.raises(FrozenInstanceError):
        pick.weight = 1.0


def test_a_name_must_not_hold_a_hash():
    """'#' separates a name from its instance number in the constraint_costs() keys."""
    with pytest.raises(ValueError) as raised:

        class Hashed(Constraint):
            name = "load#2"

    assert str(raised.value) == (
        "constraint 'load#2': a name must not contain '#', which constraint_costs() keys repeated "
        "picks with ('<name>#<k>')"
    )
    with pytest.raises(ValueError, match="must not contain '#'"):

        class Base(Constraint, abstract=True):
            name = "#"

    class Plain(Constraint):  # every other character is fine
        name = "load_2"

    assert instance_keys([Plain(), Plain()]) == ["load_2", "load_2#2"]


def test_a_soft_constraint_can_move_onto_medium():
    soft = score_costs(costs_a(INTO_SOFT), [TaskCostCost()])
    medium = score_costs(costs_a(INTO_SOFT), [TaskCostCost(level=Level.MEDIUM)])
    assert medium.soft == pytest.approx(soft.soft * INTO_SOFT[1])


def test_medium_folds_below_hard_where_the_domain_does():
    assert score_costs(costs_a(BELOW_HARD), [TaskCostCost(level="medium")]).hard < 0


def test_medium_below_hard_rounds_each_unit_like_hard():
    costs = {"task_cost_cost": costs_a(BELOW_HARD)["task_cost_cost"]}
    assert score_costs(costs, [TaskCostCost(level="medium", weight=0.01)]).hard == 0
    assert score_costs(costs, [TaskCostCost(level="medium", weight=0.1)]).hard == -(2 + 1)


def test_medium_needs_a_domain_with_a_medium_level():
    with pytest.raises(ValueError, match="this use case has no medium level"):
        score_costs(costs_a(), [OverloadCost(level="medium")])


def test_a_reward_may_be_medium_only_where_medium_stays_out_of_hard():
    def flippable(fold):
        costs = costs_a(fold)
        costs["preference_reward"]["level_flippable"] = True
        return costs

    moved = score_costs(flippable(INTO_SOFT), [PreferenceReward(level="medium")])
    assert moved.soft == pytest.approx(0.5 * 3.0 * INTO_SOFT[1])
    with pytest.raises(ValueError, match="cannot be medium here"):
        score_costs(flippable(BELOW_HARD), [PreferenceReward(level="medium")])


def test_a_structural_constraint_keeps_its_level_against_medium():
    with pytest.raises(ValueError, match="level='medium' is not allowed"):
        score_costs(costs_a(INTO_SOFT), [ForbiddenWorkerCost(level="medium")])
