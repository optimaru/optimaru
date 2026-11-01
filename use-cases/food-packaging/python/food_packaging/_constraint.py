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

    facility_capacity_cost = FacilityCapacityCost()  # the ready-made FacilityCapacityCost()
    _DEFAULTS = (facility_capacity_cost, ...)

    def get_constraints(constraints=None):
        return select(constraints, _DEFAULTS)

    def score(costs, constraints=None):
        return score_costs(costs, get_constraints(constraints))

Every spec takes the same five knobs, and only those:

- ``weight``   ``None`` = the component's default weight (C++ kDefaultWeight,
               in units of its level); otherwise a finite number >= 0.
- ``level``    ``None`` = the component's default level; otherwise a
               :class:`Level` or its name (``"hard"``, ``"medium"``, ``"soft"``).
               ``Level.MEDIUM`` needs a domain with a medium level. A reward
               refuses hard, and refuses medium where medium folds below hard;
               a constraint whose level is not flippable refuses any change.
- ``exponent`` sign-preserving power shaping, > 0 (1.0 = none): per unit for a
               unit constraint, on the single total for a global one.
- ``offset``   a shift of that shaping, any finite number (0.0 = none), at the
               same granularity as ``exponent``. Keyword-only.
- ``clip``     what happens below the offset (keyword-only). ``True`` (the
               default, the threshold): ``sign(x) * max(0, |x| - offset) **
               exponent``, so only the part beyond the offset counts.
               ``False`` (the literal formula): ``shape(x - offset,
               exponent)``, which is negative below the offset (a reward) and
               charges every unit the constraint evaluates, a raw 0 included.

With ``offset=0`` both modes are the plain ``shape(x, exponent)``, so a spec
that leaves the two new knobs alone scores exactly as before. Each unit (or a
global's total) then contributes ``weight * shape(x, exponent, offset,
clip)``.

Defaults (weight, level, flippability, reward) live in C++ only and are
reported by ``<package>.constraint_info()``; there are no default ClassVars
here. Domain parameters are extra dataclass fields on a subclass (decorate it
with ``@dataclass(frozen=True)`` and call ``super().__post_init__()`` from
its own ``__post_init__``, if it has one).

One constraint may be picked several times, in every package: each pick is
its own instance with its own knobs (and, where the constraint has domain
parameters, its own parameters), scored independently, and the instances add
up. ``x**2 + x**3`` is two picks, ``exponent=2`` and ``exponent=3``; two
identical picks simply count twice. ``constraint_costs()`` keys the k-th
instance of a name ``"<name>#<k>"`` (the first keeps the plain name), and
``instance_keys`` / ``score_costs`` follow the same rule.
"""

import math
import numbers
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field, fields
from enum import StrEnum
from typing import Any, ClassVar, NamedTuple

__all__ = ["Constraint", "Level", "Score", "instance_keys", "score_costs", "select", "shape"]

_KNOBS = ("weight", "level", "exponent", "offset", "clip")


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

    ``offset`` and ``clip`` are keyword-only, so the positional order stays
    ``weight, level, exponent`` followed by a subclass's domain parameters.
    """

    weight: float | None = None  # None = the component's default weight
    level: Level | str | None = None  # None = the component's default level
    exponent: float = 1.0
    offset: float = field(default=0.0, kw_only=True)  # 0.0 = no shift
    clip: bool = field(default=True, kw_only=True)  # True = threshold, False = literal

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
        _check_knobs(self.name, self.weight, self.level, self.exponent, self.offset, self.clip)

    def params(self) -> dict[str, Any]:
        """The domain parameters: every dataclass field but the five knobs."""
        return {field.name: getattr(self, field.name) for field in fields(self) if field.name not in _KNOBS}


def _check_knobs(name: str, weight: Any, level: Any, exponent: Any, offset: Any = 0.0, clip: Any = True) -> None:
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
    if isinstance(offset, bool) or not isinstance(offset, numbers.Real):
        raise TypeError(f"constraint {name!r}: offset must be a number, not {offset!r}")
    if not math.isfinite(offset):
        raise ValueError(f"constraint {name!r}: offset must be finite, not {offset!r}")
    if not _is_bool(clip):
        raise TypeError(f"constraint {name!r}: clip must be True or False, not {clip!r}")


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
    "the defaults" (a filter that came back empty is a mistake). A name may
    appear several times: every pick is its own instance with its own knobs
    (and parameters), scored on its own, and the instances add up; identical
    picks count twice. The same rules as the C++ ``applyConstraints``.

    ``repeatable`` is accepted for compatibility and changes nothing: since
    0.3.0 every package accepts repeated picks.
    """
    del repeatable
    if constraints is None:
        return list(defaults)
    picked = list(constraints)
    if not picked:
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return picked


def instance_keys(constraints: Iterable[Any]) -> list[str]:
    """The ``constraint_costs()`` key of each pick, in order.

    The name itself, and ``"<name>#<k>"`` for the k-th pick (k >= 2) of a name
    the selection states several times: the rule the C++
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


def _power(cost: float, exponent: float) -> float:
    if exponent == 1.0 or cost == 0.0:
        return cost
    return -((-cost) ** exponent) if cost < 0.0 else cost**exponent


def shape(cost: float, exponent: float, offset: float = 0.0, clip: bool = True) -> float:
    """Sign-preserving power, shifted by ``offset``: the engine's shapeCost.

    ``shape(x, e)`` raises ``|x|`` to ``e`` and keeps the sign (rewards, i.e.
    negative costs, never flip into penalties). With an offset:

    - ``clip=True`` (the threshold): ``sign(x) * max(0, |x| - offset) ** e``;
      ``x == 0`` stays 0 at any offset.
    - ``clip=False`` (the literal formula): ``shape(x - offset, e)``.

    ``offset=0`` is ``shape(x, e)`` in both modes. The steps are the C++ ones,
    so both give the same float.
    """
    if offset == 0.0:
        return _power(cost, exponent)
    if not clip:
        return _power(cost - offset, exponent)
    if cost == 0.0:
        return 0.0
    excess = abs(cost) - offset
    if not excess > 0.0:
        return 0.0
    shaped = _power(excess, exponent)
    return -shaped if cost < 0.0 else shaped


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


def _contribution(
    name: str, weight: float, cost: float, exponent: float, offset: float = 0.0, clip: bool = True
) -> float:
    if weight == 0:
        # Exactly 0, without shaping the cost: an opt-in constraint left at
        # weight 0 contributes nothing at any exponent or offset (0 * inf
        # would be nan). The engine's weightedCost() follows the same rule.
        return 0.0
    try:
        value = weight * shape(cost, exponent, offset, clip)
    except OverflowError:
        # A power beyond a double, where the engine's pow() gives +-inf with
        # the sign of what was raised (x - offset for the literal formula).
        base = cost - offset if offset != 0.0 and not clip else cost
        value = weight * math.copysign(math.inf, base)
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


def _evaluated(entry: Mapping[str, Any]) -> list[float]:
    """The raw costs of the units a constraint evaluates.

    Every unit's, unless the entry names its ``units`` (a scoped constraint:
    the other cells are 0 and never evaluated). The literal offset formula
    charges exactly these, a raw 0 included, as the engine does.
    """
    costs = entry["costs"]
    units = entry.get("units")
    return list(costs) if units is None else [costs[unit] for unit in units]


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
    still means the default), for a scoped constraint the ``units`` it
    evaluates, and on a domain with a medium level the domain's medium fold
    (on every entry). No solver and no search are involved, so one scoring
    pass answers every "what would this cost at weight 10 / as a soft term /
    squared beyond 10" question. A weight of 0 contributes exactly 0
    whatever the exponent or offset.

    The arithmetic is the engine's: each evaluated unit (every unit, or the
    entry's ``units``) contributes ``weight * shape(cost, exponent, offset,
    clip)``; hard contributions are rounded per
    unit (half away from zero) before summing; then the domain's medium fold
    applies to the whole score, whatever the selection holds: ``into_soft``
    adds ``medium * unit`` to soft, ``below_hard`` reports
    ``hard * unit + medium`` (medium rounded per unit too) — so a below_hard
    domain scores ``hard * unit`` even with no medium constraint selected.
    The engine's range rules apply too: a contribution that is not finite, or
    a hard one of 2**62 or more, raises OverflowError; the hard total
    saturates at ``-(2**63 - 1)`` (never wraps to a feasible-looking score).
    Every listed spec is scored (pass ``select(...)``'s result), each looked up
    under its ``instance_keys`` key; an unknown one raises ValueError. The
    k-th pick of a constraint without domain parameters may also be scored
    from costs that hold the name only once (the default ``constraint_costs()``):
    every instance of it has the same raw costs, so its entry is read with
    the pick's own knobs.
    """
    hard = 0
    soft = 0.0
    medium = 0.0
    fold = _domain_fold(costs)
    picked = list(constraints)
    for spec, key in zip(picked, instance_keys(picked)):
        entry = costs.get(key)
        if entry is None and key != spec.name and spec.name in costs and not _params_of(spec):
            # A repeat of a parameterless constraint: its raw costs are the
            # first instance's, whatever knobs either pick carries.
            entry = costs[spec.name]
        if entry is None:
            if key != spec.name and spec.name in costs:
                raise ValueError(
                    f"constraint {spec.name!r} is picked more often than the costs hold it ({key!r} is "
                    "missing); compute constraint_costs() with the same constraint list"
                )
            available = sorted({str(value.get("name", name)) for name, value in costs.items()})
            raise ValueError(f"unknown constraint {spec.name!r}; available: " + ", ".join(map(repr, available)))
        exponent = 1.0 if spec.exponent is None else spec.exponent
        offset = getattr(spec, "offset", None)
        offset = 0.0 if offset is None else offset
        clip = getattr(spec, "clip", None)
        clip = True if clip is None else clip
        _check_knobs(spec.name, spec.weight, spec.level, exponent, offset, clip)
        level = _level(spec, entry)
        weight = _default_weight(entry) if spec.weight is None else spec.weight
        values = [_contribution(spec.name, weight, cost, exponent, offset, bool(clip)) for cost in _evaluated(entry)]
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
