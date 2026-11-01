"""The constraint-spec base shared by every optimaru use-case package.

Single source of truth: ``python/optimaru/_constraint.py`` in the optimaru
repository. Each use-case package carries a byte-identical copy as
``<package>/_constraint.py``, so it builds and imports on its own
(``use-cases/<slug>/python``) as well as inside the ``optimaru`` wheel, on
every platform, without symlinks. ``scripts/check_constraint_base.py`` fails
when a copy drifts; ``--write`` re-syncs the copies. Edit this file only.

A package's ``constraints.py`` builds on it::

    from ._constraint import Constraint, Score, score_costs, select

    class FacilityCapacityCost(Constraint):
        name = "facility_capacity_cost"
        label_en = "Facility capacity"
        label_ko = "시설 용량"
        description_en = "..."
        description_ko = "..."

    facility_capacity_cost = FacilityCapacityCost()
    _DEFAULTS = (facility_capacity_cost, ...)

    def get_constraints(constraints=None):
        return select(constraints, _DEFAULTS)

    def score(costs, constraints=None):
        return score_costs(costs, get_constraints(constraints))

Every spec takes the same three knobs, and only those:

- ``weight``   ``None`` = the component's default weight (C++ kDefaultWeight,
               in units of its level); otherwise a finite number >= 0.
- ``level``    ``None`` = the component's default level; otherwise a
               :class:`Level` or its name (``"hard"``, ``"medium"``, ``"soft"``).
               ``Level.MEDIUM`` needs a domain with a medium level. A reward
               refuses hard, and refuses medium where medium folds below hard;
               a constraint whose level is not flippable refuses any change.
- ``exponent`` sign-preserving power shaping, > 0 (1.0 = none): per unit for a
               unit constraint, on the single total for a global one.

Defaults (weight, level, flippability, reward) live in C++ only and are
reported by ``<package>.constraint_info()``; there are no default ClassVars
here. Domain parameters are extra dataclass fields on a subclass (decorate it
with ``@dataclass(frozen=True)`` and call ``super().__post_init__()`` from
its own ``__post_init__``, if it has one).

A domain that may state one parametrized constraint several times
(employee-scheduling: one ``DayPatternCost`` per pattern, scoped rules) passes
``repeatable=True`` to ``select``; ``constraint_costs()`` then keys the k-th
instance of a name ``"<name>#<k>"`` and ``instance_keys`` / ``score_costs``
follow the same rule. Two instances count as the same when their parameters
are equal by value (``1``, ``1.0`` and ``numpy.int64(1)`` are one value;
``True`` is not ``1``; lists, tuples, sets and dicts compare element by
element).
"""

import math
import numbers
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, fields
from enum import StrEnum
from typing import Any, ClassVar, NamedTuple

__all__ = ["Constraint", "Level", "Score", "instance_keys", "score_costs", "select", "shape"]

_KNOBS = ("weight", "level", "exponent")


class Level(StrEnum):
    """The score level a constraint is charged on. Hard dominates medium, medium dominates soft."""

    HARD = "hard"
    MEDIUM = "medium"
    SOFT = "soft"


@dataclass(frozen=True)
class Constraint:
    """Spec selecting one constraint: weight / level / shaping.

    Subclasses set the ClassVars: ``name`` (the snake_case registry name the
    core reports in ``list_constraints()``; required), ``label_en`` /
    ``label_ko`` (a short title) and ``description_en`` / ``description_ko``
    (one or two sentences on what one unit of raw cost means). An
    intermediate base that should not need a name passes ``abstract=True``::

        class _RouteLimit(Constraint, abstract=True): ...
    """

    weight: float | None = None  # None = the component's default weight
    level: Level | str | None = None  # None = the component's default level
    exponent: float = 1.0

    name: ClassVar[str] = ""
    label_en: ClassVar[str] = ""
    label_ko: ClassVar[str] = ""
    description_en: ClassVar[str] = ""
    description_ko: ClassVar[str] = ""

    def __init_subclass__(cls, *, abstract: bool = False, **kwargs):
        super().__init_subclass__(**kwargs)
        if not abstract and not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry `name`")

    def __post_init__(self):
        _check_knobs(self.name, self.weight, self.level, self.exponent)

    def params(self) -> dict[str, Any]:
        """The domain parameters: every dataclass field but the three knobs."""
        return {field.name: getattr(self, field.name) for field in fields(self) if field.name not in _KNOBS}


def _check_knobs(name: str, weight: Any, level: Any, exponent: Any) -> None:
    if weight is not None:
        if isinstance(weight, bool) or not isinstance(weight, numbers.Real):
            raise TypeError(f"constraint {name!r}: weight must be a number or None, not {weight!r}")
        if not math.isfinite(weight) or weight < 0:
            raise ValueError(f"constraint {name!r}: weight must be finite and >= 0, not {weight!r}")
    if level is not None:
        if not isinstance(level, str):
            raise TypeError(f"constraint {name!r}: level must be a Level, its name or None, not {level!r}")
        if level not in tuple(Level):
            raise ValueError(f"constraint {name!r}: level must be 'hard', 'medium' or 'soft', not {level!r}")
    if isinstance(exponent, bool) or not isinstance(exponent, numbers.Real):
        raise TypeError(f"constraint {name!r}: exponent must be a number, not {exponent!r}")
    if not math.isfinite(exponent) or exponent <= 0:
        raise ValueError(f"constraint {name!r}: exponent must be finite and > 0, not {exponent!r}")


def _is_bool(value: Any) -> bool:
    """bool, or numpy's bool scalar (``numpy.bool_``), without importing numpy."""
    if isinstance(value, bool):
        return True
    kind = type(value)
    return kind.__module__ == "numpy" and kind.__name__ in ("bool_", "bool")


def _params_of(spec: Any) -> dict[str, Any]:
    params = getattr(spec, "params", None)
    return params() if callable(params) else {}


def _normalized(value: Any) -> Any:
    """A hashable stand-in for a parameter value that compares by value.

    Numbers compare by value whatever their type (``1 == 1.0 ==
    numpy.int64(1)``), but a bool stays distinct from the number it equals
    (``True`` is not ``1``); sequences (list and tuple alike), sets and
    mappings are compared element by element; strings and bytes are atoms.
    Anything else is used as it is when hashable, else by ``repr()``.
    """
    if value is None or isinstance(value, (str, bytes)):
        return value
    if _is_bool(value):
        return ("bool", bool(value))
    if isinstance(value, numbers.Integral):
        return ("number", int(value))
    if isinstance(value, numbers.Real):
        return ("number", float(value))  # 1.0 == 1 and hash(1.0) == hash(1)
    if isinstance(value, Mapping):
        return ("map", frozenset((_normalized(key), _normalized(item)) for key, item in value.items()))
    if isinstance(value, (set, frozenset)):
        return ("set", frozenset(_normalized(item) for item in value))
    if isinstance(value, Sequence):
        return ("sequence", tuple(_normalized(item) for item in value))
    tolist = getattr(value, "tolist", None)  # numpy arrays and scalars, without importing numpy
    if callable(tolist):
        converted = tolist()
        if converted is not value:
            return _normalized(converted)
    try:
        hash(value)
    except TypeError:
        return ("repr", repr(value))
    return value


def select(constraints: Iterable[Any] | None, defaults: Sequence[Any], *, repeatable: bool = False) -> list[Any]:
    """The ``get_constraints`` contract: ``None`` = ``defaults``; a list replaces them.

    Only the listed constraints are scored. An empty list is an error, not
    "the defaults" (a filter that came back empty is a mistake), and so is
    naming the same constraint twice (the later pick would win in silence).
    The same rules as the C++ ``applyConstraints``.

    ``repeatable=True`` is for a domain whose factory adds a parametrized
    constraint once per pick (employee-scheduling): the same name may then
    appear several times as long as the domain parameters (``params()``)
    differ; the same name with the same parameters twice is still an error.
    Parameters are compared by value (``_normalized``): ``x=1``, ``x=1.0``
    and ``x=numpy.int64(1)`` are the same parameters, ``x=True`` is not.
    """
    if constraints is None:
        return list(defaults)
    picked = list(constraints)
    if not picked:
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    seen: set[Any] = set()
    for spec in picked:
        key = (spec.name, _normalized(_params_of(spec))) if repeatable else spec.name
        if key in seen:
            if repeatable:
                raise ValueError(
                    f"constraint {spec.name!r} picked more than once with the same parameters; "
                    "state each instance once with the metadata you want"
                )
            raise ValueError(
                f"constraint {spec.name!r} picked more than once; state it once with the metadata you want"
            )
        seen.add(key)
    return picked


def instance_keys(constraints: Iterable[Any]) -> list[str]:
    """The ``constraint_costs()`` key of each pick, in order.

    The name itself, and ``"<name>#<k>"`` for the k-th pick (k >= 2) of a name
    that a ``repeatable`` selection states several times: the rule the C++
    ``constraint_costs()`` (python::instanceKeys) keys its entries by, given
    the same list in the same order.
    """
    seen: dict[str, int] = {}
    keys = []
    for spec in constraints:
        count = seen[spec.name] = seen.get(spec.name, 0) + 1
        keys.append(spec.name if count == 1 else f"{spec.name}#{count}")
    return keys


class Score(NamedTuple):
    """Engine-level (hard, soft) from ``score()``. Penalties, so <= 0 without rewards.

    A domain with a medium level folds it the way its C++ Score adapter does
    (into soft, or below hard), so this compares directly with the engine.
    """

    hard: int
    soft: float


def shape(cost: float, exponent: float) -> float:
    """Sign-preserving power: rewards (negative costs) never flip into penalties."""
    if exponent == 1.0 or cost == 0.0:
        return cost
    return -((-cost) ** exponent) if cost < 0.0 else cost**exponent


def _round_half_away(value: float) -> int:
    """llround: half away from zero (Python's round() is banker's rounding).

    Exact for every finite double: ``value - trunc(value)`` is computed
    without rounding, where ``value + 0.5`` is not (0.49999999999999994 and
    odd integers in [2**52, 2**53) would round the wrong way).
    """
    whole = math.trunc(value)
    fraction = value - whole
    return whole + (1 if fraction >= 0.5 else -1 if fraction <= -0.5 else 0)


# The range of a score (the C++ "The range of a score", level.hpp): one
# contribution must be finite, and a hard one (or a below_hard medium one)
# below 2**62 in magnitude, or scoring raises OverflowError; the hard total
# saturates at +-(2**63 - 1) instead of leaving int64. The two messages are
# word for word the ones a solve raises (ContributionOverflowError, as the
# bindings translate it), so both paths refuse a pick the same way.
_MAX_HARD_CONTRIBUTION = 2.0**62
_HARD_SCORE_LIMIT = 2**63 - 1


def _contribution(name: str, weight: float, cost: float, exponent: float) -> float:
    if weight == 0:
        # Exactly 0, without shaping the cost: an opt-in constraint left at
        # weight 0 contributes nothing at any exponent (0 * inf would be nan).
        # The engine's weightedCost() follows the same rule.
        return 0.0
    try:
        value = weight * shape(cost, exponent)
    except OverflowError:
        # cost**exponent beyond a double, where the engine's pow() gives +-inf.
        value = weight * math.copysign(math.inf, cost)
    if not math.isfinite(value):
        raise OverflowError(
            f"constraint {name!r}: its contribution weight * cost^exponent is not finite ({value!r}); "
            "lower its weight or exponent"
        )
    return value


def _round_hard(name: str, value: float) -> int:
    if not abs(value) < _MAX_HARD_CONTRIBUTION:
        raise OverflowError(
            f"constraint {name!r}: a hard contribution of {value!r} exceeds the engine's limit of "
            "2**62 (about 4.6e18) per unit; lower its weight or exponent"
        )
    return _round_half_away(value)


def _saturate_hard(total: int) -> int:
    return max(-_HARD_SCORE_LIMIT, min(_HARD_SCORE_LIMIT, total))


def _default_level(entry: Mapping[str, Any]) -> str:
    """The level ``level=None`` means: the component's default level.

    ``constraint_costs()`` reports it as ``default_level``. An entry built
    without it (by hand, or by an older package) falls back on ``level``,
    which is the default only when the costs were computed for the default
    selection or for picks that kept this constraint's level.
    """
    return entry.get("default_level") or entry.get("level") or ("hard" if entry["hard"] else "soft")


def _default_weight(entry: Mapping[str, Any]) -> float:
    """The weight ``weight=None`` means: ``default_weight``, else ``weight`` (as for the level)."""
    weight = entry.get("default_weight")
    return entry["weight"] if weight is None else weight


def _level(spec: Any, entry: Mapping[str, Any]) -> str:
    default = _default_level(entry)
    if spec.level is None:
        return default
    wanted = Level(spec.level).value
    fold = entry.get("medium_fold")
    if wanted == "medium" and fold is None:
        raise ValueError(f"constraint {spec.name!r}: this use case has no medium level")
    if entry.get("reward", False):
        if wanted == "hard":
            raise ValueError(f"constraint {spec.name!r} is a reward (raw cost <= 0) and cannot be hard")
        if wanted == "medium" and fold["kind"] == "below_hard":
            raise ValueError(
                f"constraint {spec.name!r} is a reward (raw cost <= 0) and cannot be medium here, "
                "where medium counts toward the hard score"
            )
    if wanted != default and not entry.get("level_flippable", True):
        raise ValueError(f"constraint {spec.name!r} is always {default}; level={wanted!r} is not allowed")
    return wanted


def _domain_fold(costs: Mapping[str, Mapping[str, Any]]) -> tuple[str, float] | None:
    """The domain's medium fold, which every entry of a medium domain carries."""
    fold: tuple[str, float] | None = None
    for name, entry in costs.items():
        reported = entry.get("medium_fold")
        if reported is None:
            continue
        entry_fold = (str(reported["kind"]), float(reported["unit"]))
        if entry_fold[0] not in ("into_soft", "below_hard"):
            raise ValueError(f"constraint {name!r} reports an unknown medium fold {entry_fold[0]!r}")
        if fold is not None and fold != entry_fold:
            raise ValueError(f"constraint {name!r} reports a different medium fold")
        fold = entry_fold
    return fold


def score_costs(costs: Mapping[str, Mapping[str, Any]], constraints: Iterable[Any]) -> Score:
    """Apply specs to raw constraint costs and produce the engine score — pure Python.

    ``costs`` is what ``<package>.constraint_costs(solution)`` returns: per
    constraint the **raw** cost of each unit (before weight, exponent and
    level), its default weight and level (``default_weight`` /
    ``default_level``; costs computed under a selection carry that
    selection's ``weight`` / ``level`` next to them, and an unset knob here
    still means the default), and on a domain with a medium level the
    domain's medium fold (on every entry). No solver and no search are
    involved, so one scoring pass answers every "what would this cost at
    weight 10 / as a soft term" question. A weight of 0 contributes exactly
    0 whatever the exponent.

    The arithmetic is the engine's: each unit contributes
    ``weight * shape(cost, exponent)``; hard contributions are rounded per
    unit (half away from zero) before summing; then the domain's medium fold
    applies to the whole score, whatever the selection holds: ``into_soft``
    adds ``medium * unit`` to soft, ``below_hard`` reports
    ``hard * unit + medium`` (medium rounded per unit too) — so a below_hard
    domain scores ``hard * unit`` even with no medium constraint selected.
    The engine's range rules apply too: a contribution that is not finite, or
    a hard one of 2**62 or more, raises OverflowError; the hard total
    saturates at ``-(2**63 - 1)`` (never wraps to a feasible-looking score).
    Every listed spec is scored (pass ``select(...)``'s result), each looked up
    under its ``instance_keys`` key; an unknown one raises ValueError.
    """
    hard = 0
    soft = 0.0
    medium = 0.0
    fold = _domain_fold(costs)
    picked = list(constraints)
    for spec, key in zip(picked, instance_keys(picked)):
        entry = costs.get(key)
        if entry is None:
            if key != spec.name and spec.name in costs:
                raise ValueError(
                    f"constraint {spec.name!r} is picked more often than the costs hold it ({key!r} is "
                    "missing); compute constraint_costs() with the same constraint list"
                )
            available = sorted({str(value.get("name", name)) for name, value in costs.items()})
            raise ValueError(f"unknown constraint {spec.name!r}; available: " + ", ".join(map(repr, available)))
        exponent = 1.0 if spec.exponent is None else spec.exponent
        _check_knobs(spec.name, spec.weight, spec.level, exponent)
        level = _level(spec, entry)
        weight = _default_weight(entry) if spec.weight is None else spec.weight
        values = [_contribution(spec.name, weight, cost, exponent) for cost in entry["costs"]]
        if level == "hard":
            hard += sum(_round_hard(spec.name, value) for value in values)
        elif level == "medium":
            if fold is None:
                raise ValueError(f"constraint {spec.name!r} is medium-level, but the costs carry no medium_fold")
            if fold[0] == "below_hard":
                medium += sum(_round_hard(spec.name, value) for value in values)
            else:
                medium += sum(values)
        else:
            soft += sum(values)
    if fold is not None:
        kind, unit = fold
        if kind == "below_hard":
            hard = hard * int(unit) + _round_half_away(medium)
        else:
            soft += medium * unit
    if not math.isfinite(soft):
        raise OverflowError(f"the soft score is not finite ({soft!r}); lower the soft weights or exponents")
    hard = _saturate_hard(hard)
    return Score(-hard, -soft if soft != 0.0 else 0.0)
