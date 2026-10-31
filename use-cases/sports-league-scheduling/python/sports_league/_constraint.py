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
- ``hard``     ``None`` = the component's default level (hard, medium or
               soft); ``True`` = hard; ``False`` = soft. Medium is a
               default-only level. A reward refuses ``True``; a constraint
               whose level is not flippable refuses any change.
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
follow the same rule.
"""

import math
import numbers
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, fields
from typing import Any, ClassVar, NamedTuple

__all__ = ["Constraint", "Score", "instance_keys", "score_costs", "select", "shape"]

_KNOBS = ("weight", "hard", "exponent")


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
    hard: bool | None = None  # None = the component's default level
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
        _check_knobs(self.name, self.weight, self.hard, self.exponent)

    def params(self) -> dict[str, Any]:
        """The domain parameters: every dataclass field but the three knobs."""
        return {field.name: getattr(self, field.name) for field in fields(self) if field.name not in _KNOBS}


def _check_knobs(name: str, weight: Any, hard: Any, exponent: Any) -> None:
    if weight is not None:
        if isinstance(weight, bool) or not isinstance(weight, numbers.Real):
            raise TypeError(f"constraint {name!r}: weight must be a number or None, not {weight!r}")
        if not math.isfinite(weight) or weight < 0:
            raise ValueError(f"constraint {name!r}: weight must be finite and >= 0, not {weight!r}")
    if hard is not None and not _is_bool(hard):
        raise TypeError(f"constraint {name!r}: hard must be True, False or None, not {hard!r}")
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
    """
    if constraints is None:
        return list(defaults)
    picked = list(constraints)
    if not picked:
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    seen: set[Any] = set()
    for spec in picked:
        key = (spec.name, repr(sorted(_params_of(spec).items()))) if repeatable else spec.name
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
    try:
        value = weight * shape(cost, exponent)
    except OverflowError:
        # cost**exponent beyond a double, where the engine's pow() gives +-inf
        # (and weight 0 then gives nan, as 0 * inf does in C++).
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


def _level(spec: Any, entry: Mapping[str, Any]) -> str:
    default = entry.get("level") or ("hard" if entry["hard"] else "soft")
    if spec.hard is None:
        return default
    wanted = "hard" if spec.hard else "soft"
    if wanted == "hard" and entry.get("reward", False):
        raise ValueError(f"constraint {spec.name!r} is a reward (raw cost <= 0) and cannot be hard")
    if wanted != default and not entry.get("level_flippable", True):
        raise ValueError(f"constraint {spec.name!r} is always {default}; hard={bool(spec.hard)!r} is not allowed")
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
    level), its default weight and level, and on a domain with a medium level
    the domain's medium fold (on every entry). No solver and no search are
    involved, so one scoring pass answers every "what would this cost at
    weight 10 / as a soft term" question.

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
        _check_knobs(spec.name, spec.weight, spec.hard, exponent)
        level = _level(spec, entry)
        weight = entry["weight"] if spec.weight is None else spec.weight
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
