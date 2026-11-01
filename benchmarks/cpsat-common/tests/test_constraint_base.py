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
import struct
import sys
from dataclasses import FrozenInstanceError, dataclass
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
    ``default_level`` (what weight=None / hard=None mean). Re-pricing with
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
    assert score_costs(costs, [ConflictCost(weight=5.0, hard=False), OverloadCost(hard=True)]) == (-1, -15.0)
    # An entry without the default keys (hand-made, older package) reads level / weight.
    legacy = {"conflict_cost": _entry([2.0, 1.0], level="soft", weight=5.0, name="conflict_cost")}
    assert score_costs(legacy, [ConflictCost()]) == (0, -15.0)


def test_a_non_flippable_check_uses_the_default_level():
    costs = {
        "forbidden_worker_cost": {
            **_entry([1.0], level="hard", flippable=False, name="forbidden_worker_cost"),
            "default_level": "hard",
            "default_weight": 1.0,
        }
    }
    with pytest.raises(ValueError, match="is always hard; hard=False is not allowed"):
        score_costs(costs, [ForbiddenWorkerCost(hard=False)])
    assert score_costs(costs, [ForbiddenWorkerCost(hard=True)]) == (-1, 0.0)


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
    # Since 0.3.0 a name may be picked several times: each pick is an instance.
    picks = [ConflictCost(), ConflictCost(weight=2)]
    assert select(picks, DEFAULTS) == picks
    assert select([ConflictCost(), ConflictCost()], DEFAULTS) == [ConflictCost(), ConflictCost()]


def test_the_two_new_knobs_are_keyword_only_and_default_to_the_0_2_0_shaping():
    spec = ConflictCost(2, None, 3.0)
    assert (spec.weight, spec.hard, spec.exponent, spec.offset, spec.clip) == (2, None, 3.0, 0.0, True)
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


def test_repeated_picks_are_kept_with_or_without_the_old_repeatable_flag():
    """``repeatable`` changes nothing since 0.3.0; the same parameters twice are two instances."""
    first, second = (
        DayPatternCost(pattern="A,OFF"),
        DayPatternCost(pattern="B,OFF", worker_indices=(1,)),
    )
    same = DayPatternCost(pattern="A,OFF", weight=3)
    for flag in (True, False):
        assert select([first, second], DEFAULTS, repeatable=flag) == [first, second]
        assert select([first, same], DEFAULTS, repeatable=flag) == [first, same]


@dataclass(frozen=True)
class ScopedLimitCost(Constraint):
    name = "scoped_limit_cost"
    limit: Any = 0
    scope: Any = None


@pytest.mark.parametrize(
    ("first", "second"),
    [
        ({"limit": 1}, {"limit": 1.0}),
        ({"limit": 2}, {"limit": np.int64(2)}),
        ({"limit": 2.5}, {"limit": np.float64(2.5)}),
        ({"scope": [1, 2]}, {"scope": (1.0, np.int32(2))}),
        ({"scope": {"a": [1, {2}]}}, {"scope": {"a": (1.0, frozenset({2.0}))}}),
        ({"scope": np.array([1, 2])}, {"scope": [1, 2]}),
        ({"scope": True}, {"scope": np.bool_(True)}),
    ],
)
def test_parameters_compare_by_value(first, second):
    """x=1 and x=1.0 (or numpy.int64(1)) are the same parameters, at any depth (``_normalized``).

    select() no longer refuses equal parameters; the helper stays for callers
    that compare picks (employee-scheduling's bindings until 0.3.0), and the
    picks are kept.
    """
    picks = [ScopedLimitCost(**first), ScopedLimitCost(**second)]
    assert base._normalized(picks[0].params()) == base._normalized(picks[1].params())
    assert select(picks, DEFAULTS) == picks


@pytest.mark.parametrize(
    ("first", "second"),
    [
        ({"limit": 1}, {"limit": True}),
        ({"limit": 0}, {"limit": False}),
        ({"scope": [1]}, {"scope": [True]}),
        ({"limit": 1}, {"limit": 1.5}),
        ({"scope": "12"}, {"scope": ["1", "2"]}),
        ({"scope": [1, 2]}, {"scope": [2, 1]}),
        ({"scope": {"a": 1}}, {"scope": {"a": 2}}),
        ({"limit": 2**53 + 1}, {"limit": float(2**53 + 1)}),
    ],
)
def test_different_parameters_stay_different(first, second):
    """A bool is not the number it equals; order, strings and exact values still count."""
    picks = [ScopedLimitCost(**first), ScopedLimitCost(**second)]
    assert base._normalized(picks[0].params()) != base._normalized(picks[1].params())
    assert select(picks, DEFAULTS) == picks


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
    """constraint_level_test.cpp (p) repeated<SoftFoldTasks>(): -3hard / -6156.5soft (IntoSoft 1000)."""
    picks = [
        *DEFAULTS,
        TaskCostCost(exponent=2.0, offset=5.0),
        TaskCostCost(weight=0.5, offset=10.0, clip=False),
        ConflictCost(offset=1.0, clip=False),
        IdleWorkerCost(offset=1.0, exponent=2.0, clip=False),
        OverloadCost(offset=-1.0, weight=2.0),
    ]
    assert score_costs(costs_a(INTO_SOFT), picks) == (-3, -6156.5)


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
def test_offset_zero_is_the_0_2_0_shape_bit_for_bit(cost, exponent, clip):
    old = cost if exponent == 1.0 or cost == 0.0 else (-((-cost) ** exponent) if cost < 0.0 else cost**exponent)
    new = shape(cost, exponent, 0.0, clip)
    assert struct.pack("<d", new) == struct.pack("<d", old)


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
    # [1.5, 0.5, -0.5, -0.5] rounded per unit, half away from zero: 2 + 1 - 1 - 1.
    assert score_costs(costs, [ConflictCost(offset=0.5, clip=False)]) == (-1, 0.0)
    assert score_costs(costs, [ConflictCost(offset=1.0, clip=False)]) == (1, 0.0)


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


def test_a_literal_overflow_keeps_the_sign_of_the_shifted_cost():
    """(1 - 1000)^201 is -inf in the engine's pow(); the mirror reports the same."""
    costs = {"task_cost_cost": _entry([1.0], level="soft", name="task_cost_cost")}
    with pytest.raises(OverflowError, match=r"is not finite \(-inf\)"):
        score_costs(costs, [TaskCostCost(offset=1000.0, exponent=201.0, clip=False)])
    with pytest.raises(OverflowError, match=r"is not finite \(inf\)"):
        score_costs(costs, [TaskCostCost(offset=-1000.0, exponent=201.0)])


def test_duck_typed_picks_may_leave_offset_and_clip_unset():
    class Pick:
        name = "task_cost_cost"
        weight = None
        hard = None
        exponent = 1.0

    assert score_costs(_tasks_only(costs_a()), [Pick()]) == (0, -28.0)


def test_shape_preserves_the_sign():
    assert shape(3.0, 2.0) == 9.0
    assert shape(-3.0, 2.0) == -9.0
    assert shape(0.0, 0.5) == 0.0
    assert shape(5.0, 1.0) == 5.0


def test_score_is_a_named_tuple():
    result = score_costs(costs_a(INTO_SOFT), [ConflictCost()])
    assert isinstance(result, Score)
    assert (result.hard, result.soft) == (-3, 0.0)
