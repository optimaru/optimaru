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
"""

import math
import numbers
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, fields
from typing import Any, ClassVar, NamedTuple

__all__ = ["Constraint", "Score", "score_costs", "select", "shape"]

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
    if hard is not None and not isinstance(hard, bool):
        raise TypeError(f"constraint {name!r}: hard must be True, False or None, not {hard!r}")
    if isinstance(exponent, bool) or not isinstance(exponent, numbers.Real):
        raise TypeError(f"constraint {name!r}: exponent must be a number, not {exponent!r}")
    if not math.isfinite(exponent) or exponent <= 0:
        raise ValueError(f"constraint {name!r}: exponent must be finite and > 0, not {exponent!r}")


def select(constraints: Iterable[Any] | None, defaults: Sequence[Any]) -> list[Any]:
    """The ``get_constraints`` contract: ``None`` = ``defaults``; a list replaces them.

    Only the listed constraints are scored. An empty list is an error, not
    "the defaults" (a filter that came back empty is a mistake), and so is
    naming the same constraint twice (the later pick would win in silence).
    The same rules as the C++ ``applyConstraints``.
    """
    if constraints is None:
        return list(defaults)
    picked = list(constraints)
    if not picked:
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    seen: set[str] = set()
    for spec in picked:
        if spec.name in seen:
            raise ValueError(
                f"constraint {spec.name!r} picked more than once; state it once with the metadata you want"
            )
        seen.add(spec.name)
    return picked


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
    """llround: half away from zero (Python's round() is banker's rounding)."""
    return int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))


def _level(spec: Any, entry: Mapping[str, Any]) -> str:
    default = entry.get("level") or ("hard" if entry["hard"] else "soft")
    if spec.hard is None:
        return default
    wanted = "hard" if spec.hard else "soft"
    if wanted == "hard" and entry.get("reward", False):
        raise ValueError(f"constraint {spec.name!r} is a reward (raw cost <= 0) and cannot be hard")
    if wanted != default and not entry.get("level_flippable", True):
        raise ValueError(f"constraint {spec.name!r} is always {default}; hard={spec.hard!r} is not allowed")
    return wanted


def score_costs(costs: Mapping[str, Mapping[str, Any]], constraints: Iterable[Any]) -> Score:
    """Apply specs to raw constraint costs and produce the engine score — pure Python.

    ``costs`` is what ``<package>.constraint_costs(solution)`` returns: per
    constraint the **raw** cost of each unit (before weight, exponent and
    level), its default weight and level, and for a medium constraint the
    domain's medium fold. No solver and no search are involved, so one
    scoring pass answers every "what would this cost at weight 10 / as a soft
    term" question.

    The arithmetic is the engine's: each unit contributes
    ``weight * shape(cost, exponent)``; hard contributions are rounded per
    unit (half away from zero) before summing; medium folds into soft
    (``into_soft``) or below hard (``below_hard``, rounded per unit too).
    Every listed spec is scored (pass ``select(...)``'s result); an unknown
    name raises ValueError.
    """
    hard = 0
    soft = 0.0
    medium = 0.0
    fold: tuple[str, float] | None = None
    for spec in constraints:
        entry = costs.get(spec.name)
        if entry is None:
            raise ValueError(
                f"unknown constraint {spec.name!r}; available: " + ", ".join(repr(name) for name in sorted(costs))
            )
        exponent = 1.0 if spec.exponent is None else spec.exponent
        _check_knobs(spec.name, spec.weight, spec.hard, exponent)
        level = _level(spec, entry)
        weight = entry["weight"] if spec.weight is None else spec.weight
        values = [weight * shape(cost, exponent) for cost in entry["costs"]]
        if level == "hard":
            hard += sum(_round_half_away(value) for value in values)
        elif level == "medium":
            entry_fold = (entry["medium_fold"]["kind"], float(entry["medium_fold"]["unit"]))
            if fold is not None and fold != entry_fold:
                raise ValueError(f"constraint {spec.name!r} reports a different medium fold")
            fold = entry_fold
            if fold[0] == "below_hard":
                medium += sum(_round_half_away(value) for value in values)
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
    return Score(-hard, -soft if soft != 0.0 else 0.0)
