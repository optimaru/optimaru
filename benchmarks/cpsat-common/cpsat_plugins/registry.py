from __future__ import annotations

from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
import math
import numbers
import time
from typing import Any, Protocol, TypeAlias

from ortools.sat.python import cp_model

# Folded objective coefficients must stay inside int64 with headroom.
_FOLD_LIMIT = 2**62


def _round_half_away(value: float) -> int:
    """llround, exactly (the optimaru ``_constraint`` helper's formula).

    ``value - trunc(value)`` is exact for a double, where ``value + 0.5`` is
    not (0.49999999999999994 would round up).
    """
    whole = math.trunc(value)
    fraction = value - whole
    return whole + (1 if fraction >= 0.5 else -1 if fraction <= -0.5 else 0)


def _is_bool(value: Any) -> bool:
    """bool, or numpy's bool scalar, without importing numpy."""
    if isinstance(value, bool):
        return True
    kind = type(value)
    return kind.__module__ == "numpy" and kind.__name__ in ("bool_", "bool")


def _check_knobs(
    name: str, weight: Any, level: Any, exponent: Any, offset: Any = 0.0, clip: Any = True
) -> tuple[float | None, float, float]:
    """The engine's knob rules, with the optimaru spec base's wording.

    A Mapping selection gets the checks a spec gets at construction, so the
    two solvers refuse the same values: weight None or a real number (not
    bool), finite and >= 0; level None, or "hard", "medium" or "soft";
    exponent a real number, finite and > 0 (and 1.0 on CP-SAT, see
    ``_check_exponent``); offset a real number (not bool), finite (and an
    integer on CP-SAT, see ``ObjectiveBuilder.add``); clip True or False.

    Like the spec base (``_check_knobs``) and the engine (``readKnobs``), a
    number is checked as the double ``float()`` makes of it and returned as
    that double: ``(weight, exponent, offset)``. So an offset of
    ``Fraction(1, 10**400)`` is 0.0 here as it is for the engine.
    """
    checked_weight = None
    if weight is not None:
        if isinstance(weight, bool) or not isinstance(weight, numbers.Real):
            raise TypeError(f"constraint {name!r}: weight must be a number or None, not {weight!r}")
        checked_weight = float(weight)
        if not math.isfinite(checked_weight) or checked_weight < 0:
            raise ValueError(f"constraint {name!r}: weight must be finite and >= 0, not {weight!r}")
    if level is not None:
        if not isinstance(level, str):
            raise TypeError(f"constraint {name!r}: level must be a Level, its name or None, not {level!r}")
        if level not in ("hard", "medium", "soft"):
            raise ValueError(f"constraint {name!r}: level must be 'hard', 'medium' or 'soft', not {level!r}")
    if isinstance(exponent, bool) or not isinstance(exponent, numbers.Real):
        raise TypeError(f"constraint {name!r}: exponent must be a number, not {exponent!r}")
    checked_exponent = float(exponent)
    if not math.isfinite(checked_exponent) or checked_exponent <= 0:
        raise ValueError(f"constraint {name!r}: exponent must be finite and > 0, not {exponent!r}")
    if isinstance(offset, bool) or not isinstance(offset, numbers.Real):
        raise TypeError(f"constraint {name!r}: offset must be a number, not {offset!r}")
    checked_offset = float(offset)
    if not math.isfinite(checked_offset):
        raise ValueError(f"constraint {name!r}: offset must be finite, not {offset!r}")
    if not _is_bool(clip):
        raise TypeError(f"constraint {name!r}: clip must be True or False, not {clip!r}")
    return checked_weight, checked_exponent, checked_offset


def _shift(value: int | float, offset: int | float, clip: bool) -> int | float:
    """The engine's shapeCost at exponent 1 (the only exponent CP-SAT takes).

    ``clip``: sign(x) * max(0, |x| - offset); otherwise x - offset. offset 0
    returns x unchanged.
    """
    if offset == 0:
        return value
    if not clip:
        return value - offset
    if value == 0:
        return 0
    excess = abs(value) - offset
    if not excess > 0:
        return 0
    return -excess if value < 0 else excess


def _evaluated(entry: Mapping[str, Any]) -> list:
    """The raw costs of the units a constraint evaluates (``units`` when scoped)."""
    costs = entry["costs"]
    units = entry.get("units")
    return list(costs) if units is None else [costs[unit] for unit in units]


def _is_constant(term: Any) -> bool:
    return isinstance(term, numbers.Integral)


def _check_exponent(name: str, exponent: float) -> None:
    """CP-SAT's own limit, checked after the engine's rules (``ObjectiveBuilder``)."""
    if exponent != 1.0:
        raise ValueError(f"constraint {name!r} exponent is not supported by CP-SAT")


def _constraint_name(name: str) -> str:
    """The constraint's name, which the engine's messages use.

    A registry with one plugin per pick (employee scheduling) names the k-th
    one ``"<name>#<k>"``, its ``constraint_costs()`` key; a constraint name
    never contains ``#``.
    """
    return name.partition("#")[0]


@dataclass(frozen=True)
class ConstraintSpec:
    """One catalog entry and its default score semantics.

    ``level`` and ``weight`` are the defaults (for a migrated domain, the
    engine's constraint_info() row). ``level_flippable`` False means a selection's level
    may not move the constraint off its level; ``reward`` means the raw cost
    is <= 0 and level="hard" is refused. Both paths reject the same
    selections. ``default_enabled`` is the row's ``default_enabled`` (None:
    ``weight > 0``, the rule for a domain whose catalog is its default set).
    """

    name: str
    label: str
    level: str
    weight: int | float = 1
    description: str = ""
    label_en: str = ""
    description_en: str = ""
    level_flippable: bool = True
    reward: bool = False
    default_enabled: bool | None = None

    def as_dict(self) -> dict[str, Any]:
        """The catalog row, in the server's ConstraintInfo vocabulary (knob_plan §5).

        ``kind`` is ``"either"`` when a selection may move the constraint,
        else its fixed level (a non-flippable constraint is hard or soft).
        ``params`` are the five selection knobs at "keep the default" — fed
        back as a selection they change nothing (so ``level`` is ``None``).
        """
        if self.level_flippable:
            kind = "either"
        else:
            kind = "hard" if self.level == "hard" else "soft"
        return {
            "name": self.name,
            "label": self.label,
            "description": self.description,
            "label_en": self.label_en,
            "description_en": self.description_en,
            "kind": kind,
            "default_enabled": self.weight > 0 if self.default_enabled is None else self.default_enabled,
            "default_level": self.level,
            "default_weight": self.weight,
            "level_flippable": self.level_flippable,
            "reward": self.reward,
            "params": {"weight": None, "level": None, "exponent": 1.0, "offset": 0.0, "clip": True},
        }


@dataclass(frozen=True)
class ConstraintConfig:
    """Runtime selection. A supplied list has replace semantics.

    A name may appear several times: each entry is its own instance with its
    own knobs (see ``resolve``). ``offset`` / ``clip`` shape every term the
    plugin adds (``ObjectiveBuilder.add``). ``parse`` stores the numbers as
    the doubles the engine reads. ``exponent`` is kept so that
    ``ObjectiveBuilder`` can refuse one other than 1.0 after the engine's own
    rules, as the engine would refuse the pick first.
    """

    name: str
    enabled: bool = True
    weight: int | float | None = None
    level: str | None = None
    offset: int | float = 0
    clip: bool = True
    exponent: float = 1.0

    @classmethod
    def parse(
        cls, value: ConstraintConfig | DomainConstraint | str | Mapping[str, Any]
    ) -> ConstraintConfig:
        if isinstance(value, cls):
            return value
        if isinstance(value, str):
            return cls(name=value)
        if not isinstance(value, Mapping) and hasattr(value, "name"):
            name = str(value.name)
            exponent = getattr(value, "exponent", 1.0)
            exponent = 1.0 if exponent is None else exponent
            weight = getattr(value, "weight", None)
            level = value.level  # no default: a pick without it must not fall back in silence
            offset = getattr(value, "offset", None)
            offset = 0 if offset is None else offset
            clip = getattr(value, "clip", None)
            clip = True if clip is None else clip
            weight, exponent, offset = _check_knobs(name, weight, level, exponent, offset, clip)
            return cls(
                name=name,
                weight=weight,
                level=None if level is None else str(level),
                offset=offset,
                clip=bool(clip),
                exponent=exponent,
            )
        payload = dict(value)
        name = payload.pop("name", payload.pop("type", None))
        if name is None:
            raise ValueError("constraint config needs 'name' or 'type'")
        # The catalog row's params ({weight, level, exponent, offset, clip})
        # and the server's ConstraintSelection {name, enabled, weight, level,
        # exponent[, offset, clip]} are both valid selections; only an
        # exponent other than 1.0 is CP-SAT-only.
        unknown = set(payload) - {"enabled", "weight", "level", "exponent", "offset", "clip"}
        if unknown:
            raise ValueError(
                f"unknown constraint parameters for {name!r}: {sorted(unknown)}"
            )
        name = str(name)
        exponent = payload.pop("exponent", 1.0)
        exponent = 1.0 if exponent is None else exponent
        weight = payload.get("weight")
        level = payload.get("level")
        offset = payload.get("offset")
        offset = 0 if offset is None else offset
        clip = payload.get("clip")
        clip = True if clip is None else clip
        weight, exponent, offset = _check_knobs(name, weight, level, exponent, offset, clip)
        enabled = payload.get("enabled", True)
        if not _is_bool(enabled):
            raise TypeError(f"constraint {name!r}: enabled must be True or False, not {enabled!r}")
        return cls(
            name=name,
            enabled=bool(enabled),
            weight=weight,
            level=None if level is None else str(level),
            offset=offset,
            clip=bool(clip),
            exponent=exponent,
        )


class DomainConstraint(Protocol):
    """Shared engine-side constraint selection accepted by CP-SAT too.

    ``offset`` / ``clip`` (optional on a duck-typed pick: 0 / True).
    """

    name: str
    weight: float | None
    level: str | None
    exponent: float


ConstraintSelection: TypeAlias = (
    ConstraintConfig | DomainConstraint | str | Mapping[str, Any]
)


def resolve(
    specs: Sequence[ConstraintSpec],
    constraints: Iterable[ConstraintSelection] | None,
) -> dict[str, tuple[ConstraintSpec, ConstraintConfig]]:
    """Resolve defaults or an explicit replacement list into an instance map.

    Keyed like the engine's ``constraint_costs()``: the first enabled pick of
    a name is ``name``, the k-th ``"<name>#<k>"`` (python::instanceKeys), so
    a name picked several times gives one entry per pick, each with its own
    knobs. Disabled picks are dropped before counting.
    """
    by_name = {spec.name: spec for spec in specs}
    selected = (
        [ConstraintConfig(name=spec.name) for spec in specs]
        if constraints is None
        else [ConstraintConfig.parse(value) for value in constraints]
    )
    result: dict[str, tuple[ConstraintSpec, ConstraintConfig]] = {}
    seen: dict[str, int] = {}
    for config in selected:
        if config.name not in by_name:
            raise ValueError(
                f"unknown constraint {config.name!r}; available: {sorted(by_name)}"
            )
        if not config.enabled:
            continue
        count = seen[config.name] = seen.get(config.name, 0) + 1
        key = config.name if count == 1 else f"{config.name}#{count}"
        if key in result:
            raise ValueError(f"duplicate constraint {key!r}")
        result[key] = (by_name[config.name], config)
    return result


_MEDIUM_FOLDS = (None, "into_soft", "below_hard")


def _check_literal_offset(name: str, level: str, config: ConstraintConfig, medium_fold: str | None) -> None:
    """``clip=False`` with an offset != 0 stays off the levels that count toward the hard score.

    The engine's rule, word for word as the optimaru spec base
    (``_check_literal_offset``) and the bindings (python::checkLiteralOffset)
    raise it: hard, and medium where the domain folds it below hard
    (``medium_fold="below_hard"``, task assigning), refuse the literal
    formula, because it charges every evaluated unit ``x - offset`` and hard 0
    must mean no violation. ``level`` is the effective one, and the offset
    is compared as the double the engine reads.
    """
    if config.clip or float(config.offset) == 0.0:
        return
    name = _constraint_name(name)
    if level == "hard":
        raise ValueError(
            f"constraint {name!r}: clip=False with a non-zero offset cannot be hard, because hard 0 "
            "must mean no violation; use clip=True (a threshold) or another level"
        )
    if level == "medium" and medium_fold == "below_hard":
        raise ValueError(
            f"constraint {name!r}: clip=False with a non-zero offset cannot be medium here, where "
            "medium counts toward the hard score; use clip=True (a threshold) or level='soft'"
        )


class ObjectiveBuilder:
    """Collect named penalty terms and build a safe lexicographic objective.

    ``medium_fold`` is the engine domain's MediumFold kind (what
    ``constraint_costs()`` reports as ``medium_fold["kind"]``): ``None`` for a
    domain without a medium level, ``"into_soft"``, or ``"below_hard"`` where
    medium counts toward the hard score (task assigning). The knob rules of
    every selected instance (``level_of``) are checked here, before any term
    is added.
    """

    def __init__(
        self,
        model: cp_model.CpModel,
        levels: Sequence[str],
        specs: Sequence[ConstraintSpec],
        constraints: Iterable[ConstraintSelection] | None = None,
        *,
        medium_fold: str | None = None,
    ) -> None:
        if not levels or levels[0] != "hard":
            raise ValueError("levels must start with 'hard'")
        if medium_fold not in _MEDIUM_FOLDS:
            raise ValueError(f"medium_fold must be None, 'into_soft' or 'below_hard', not {medium_fold!r}")
        self.model = model
        self.levels = tuple(levels)
        self.medium_fold = medium_fold
        self.selected = resolve(specs, constraints)
        self.terms: dict[str, list] = {level: [] for level in self.levels}
        self.bounds: dict[str, int] = {level: 0 for level in self.levels}
        # Refuse a selection before the model is built: first the engine's
        # rules for every instance (the engine and score() refuse such a pick
        # in these words), then what only CP-SAT cannot model.
        for spec, config in self.selected.values():
            self.level_of(spec, config)
        for spec, config in self.selected.values():
            _check_exponent(_constraint_name(spec.name), config.exponent)

    def level_of(self, spec: ConstraintSpec, config: ConstraintConfig) -> str:
        """The level a selection lands on, with the engine's knob rules.

        ``level=None`` keeps the default level. ``"medium"`` needs a domain
        with a medium level, a reward refuses ``"hard"`` (and ``"medium"``
        with ``medium_fold="below_hard"``), and a non-flippable constraint
        refuses any change of level. On the level it lands on,
        ``clip=False`` with an offset != 0 is refused where that level counts
        toward the hard score (hard; medium with ``medium_fold="below_hard"``).
        """
        level = self._requested_level(spec, config)
        _check_literal_offset(spec.name, level, config, self.medium_fold)
        return level

    def refuse_signed(self, name: str) -> None:
        """The engine's sign rule, for a plugin whose data can make its raw cost negative.

        A plugin over data that is not validated >= 0 (quadratic assignment's
        cost matrices; the engine's ``rawCostCanBeNegative``) calls this when
        its data holds a negative entry. Like a reward, such an instance
        cannot sit on a level that counts toward the hard score (hard; medium
        with ``medium_fold="below_hard"``), because a negative hard
        contribution would cancel another violation. Refused in the engine's
        words (SignedCostLevelError, as the bindings raise it). ``name`` is
        the name the plugin adds its terms under.
        """
        selected = self.selected.get(name)
        if selected is None:
            return
        spec, config = selected
        if spec.reward:
            return
        level = self.level_of(spec, config)
        head = f"constraint {_constraint_name(spec.name)!r} can have a negative raw cost on this problem (a bonus) "
        if level == "hard":
            raise ValueError(head + "and cannot be hard")
        if level == "medium" and self.medium_fold == "below_hard":
            raise ValueError(head + "and cannot be medium here, where medium counts toward the hard score")

    def _requested_level(self, spec: ConstraintSpec, config: ConstraintConfig) -> str:
        """The engine's level rules (level_guard.hpp checkedLevel), in its words."""
        wanted = config.level
        if wanted is None:
            return spec.level
        name = _constraint_name(spec.name)
        if wanted == "medium" and wanted not in self.levels:
            raise ValueError(f"constraint {name!r}: this use case has no medium level")
        if spec.reward and wanted == "hard":
            raise ValueError(f"constraint {name!r} is a reward (raw cost <= 0) and cannot be hard")
        if spec.reward and wanted == "medium" and self.medium_fold == "below_hard":
            raise ValueError(
                f"constraint {name!r} is a reward (raw cost <= 0) and cannot be medium here, "
                "where medium counts toward the hard score"
            )
        if wanted != spec.level and not spec.level_flippable:
            raise ValueError(f"constraint {name!r} is always {spec.level}; level={wanted!r} is not allowed")
        return wanted

    def add(self, name: str, magnitude, *, bound: int) -> None:
        """Add a non-negative raw penalty and a valid magnitude upper bound.

        One call is one engine unit (or a global constraint's single total):
        the selection's ``offset`` / ``clip`` shape each call's term on its
        own, as the engine shapes each unit. With an offset, call it for EVERY
        unit the engine evaluates, a constant 0 included (``bound=0``): the
        literal formula charges ``-weight * offset`` there.
        """
        self._add(name, magnitude, low=0, high=max(0, int(bound)))

    def add_reward(self, name: str, magnitude, *, bound: int) -> None:
        """Add a non-negative reward (represented as a negative penalty)."""
        self._add(name, -magnitude, low=-max(0, int(bound)), high=0)

    def _add(self, name: str, value, *, low: int, high: int) -> None:
        """Add ``weight * shift(value)`` for a raw ``value`` in ``[low, high]``."""
        selected = self.selected.get(name)
        if selected is None:
            return
        spec, config = selected
        level = self.level_of(spec, config)
        if level not in self.terms:
            raise ValueError(
                f"constraint {name!r} uses unsupported score level {level!r}"
            )
        weight = spec.weight if config.weight is None else config.weight
        if weight < 0:
            raise ValueError(f"constraint {name!r} weight must be >= 0")
        # The API's shared exact/metaheuristic contract represents weights as
        # JSON numbers, so Pydantic sends even catalog integers as ``50.0``.
        # OR-Tools accepts that when building an objective but produces a
        # FloatAffine that CpSolver.value() cannot evaluate for score output.
        if isinstance(weight, float):
            if not weight.is_integer():
                raise ValueError(
                    f"constraint {name!r} weight must be an integer for CP-SAT"
                )
            weight = int(weight)
        offset = config.offset
        if isinstance(offset, float):
            if not offset.is_integer():
                raise ValueError(
                    f"constraint {name!r} offset must be an integer for CP-SAT"
                )
            offset = int(offset)
        if not weight:
            return
        if offset == 0:
            # A zero range means this penalty is identically zero. Keeping
            # ``weight * 0`` creates OR-Tools' FloatAffine(IntConstant(0), ...),
            # which CpSolver.value() cannot evaluate when scores are reported.
            if high > low:
                self.terms[level].append(weight * value)
                self.bounds[level] += weight * (high - low)
            return
        term, term_low, term_high = self._shifted(value, low, high, offset, config.clip)
        if _is_constant(term):
            if term:
                self.terms[level].append(weight * int(term))
            return
        self.terms[level].append(weight * term)
        self.bounds[level] += weight * (term_high - term_low)

    def _shifted(self, value, low: int, high: int, offset: int, clip: bool):
        """``(term, low, high)``: the engine's offset shaping of an integral raw value.

        Literal: ``value - offset``. Threshold, offset > 0:
        ``max(0, x - offset) + min(0, x + offset)`` (each half only where the
        range reaches it). Threshold, offset < 0: ``x + |offset| * [x >= 1] -
        |offset| * [x <= -1]``. Exact for integral expressions and offsets.
        """
        if _is_constant(value) or high == low:
            constant = int(value) if _is_constant(value) else low
            shifted = _shift(constant, offset, clip)
            return int(shifted), int(shifted), int(shifted)
        if not clip:
            return value - offset, low - offset, high - offset
        model = self.model
        if offset > 0:
            parts = []
            term_low = term_high = 0
            if high > offset:
                above = model.new_int_var(0, high - offset, "")
                model.add_max_equality(above, [0, value - offset])
                parts.append(above)
                term_high = high - offset
            if low < -offset:
                below = model.new_int_var(low + offset, 0, "")
                model.add_min_equality(below, [0, value + offset])
                parts.append(below)
                term_low = low + offset
            if not parts:
                return 0, 0, 0
            return sum(parts), term_low, term_high
        gap = -offset
        term = value
        term_low, term_high = low, high
        if high >= 1:
            positive = model.new_bool_var("")
            model.add(value >= 1).only_enforce_if(positive)
            model.add(value <= 0).only_enforce_if(~positive)
            term = term + gap * positive
            term_high = high + gap
        if low <= -1:
            negative = model.new_bool_var("")
            model.add(value <= -1).only_enforce_if(negative)
            model.add(value >= 0).only_enforce_if(~negative)
            term = term - gap * negative
            term_low = low - gap
        return term, term_low, term_high

    def folds(self) -> bool:
        """Whether all levels fit one int64 objective (see `minimize`)."""
        span = 1
        for level in self.levels:
            span *= self.bounds[level] + 1
        return span < _FOLD_LIMIT

    def minimize(self) -> None:
        """Install one integer objective preserving the declared level order."""
        multiplier = 1
        expressions = []
        for level in reversed(self.levels):
            expressions.append(multiplier * sum(self.terms[level]))
            multiplier *= self.bounds[level] + 1
        self.model.minimize(sum(expressions))

    def solve(self, solver: cp_model.CpSolver) -> int:
        """Minimize lexicographically within the solver's time limit.

        When the folded objective fits int64 this is one `minimize` + solve.
        Otherwise CP-SAT would reject the model (MODEL_INVALID), so the levels
        are optimized one after another: each stage minimizes one level, then
        that level is capped at the value reached and the incumbent is hinted
        to the next stage. Every stage gets an equal share of the time left.
        """
        if self.folds():
            self.minimize()
            return solver.solve(self.model)
        budget = solver.parameters.max_time_in_seconds
        deadline = time.monotonic() + budget
        stages = [
            level
            for level in self.levels
            if any(not _is_constant(term) for term in self.terms[level])
        ]
        solved = optimal = False
        for index, level in enumerate(stages):
            left = max(deadline - time.monotonic(), 0.01)
            solver.parameters.max_time_in_seconds = left / (len(stages) - index)
            total = sum(self.terms[level])
            self.model.minimize(total)
            status = solver.solve(self.model)
            if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
                if not solved:
                    solver.parameters.max_time_in_seconds = budget
                    return status
                # Out of time on a later level: restore the last incumbent so
                # the caller can still read it from the solver.
                solver.parameters.fix_variables_to_their_hinted_value = True
                solver.solve(self.model)
                solver.parameters.fix_variables_to_their_hinted_value = False
                optimal = False
                break
            optimal = (optimal or not solved) and status == cp_model.OPTIMAL
            solved = True
            self.model.add(total <= int(solver.objective_value))
            self.model.clear_hints()
            for var_index in range(len(self.model.proto.variables)):
                var = self.model.get_int_var_from_proto_index(var_index)
                self.model.add_hint(var, solver.value(var))
        solver.parameters.max_time_in_seconds = budget
        return cp_model.OPTIMAL if optimal else cp_model.FEASIBLE

    def score(self, solver: cp_model.CpSolver) -> dict[str, int]:
        """Return negative penalty totals for the solver's current solution."""
        return {
            level: -sum(
                int(term) if _is_constant(term) else int(solver.value(term))
                for term in self.terms[level]
            )
            for level in self.levels
        }

    def score_costs(
        self,
        costs: Mapping[str, Mapping[str, Any]],
        aliases: Mapping[str, str | Sequence[str]] | None = None,
    ) -> dict[str, int | float]:
        """Score canonical raw constraint costs with this exact selection.

        Some CP-SAT formulations use a monotone integer surrogate (for
        example squared loads instead of square-root unfairness). Search may
        use that surrogate, but API scores must still use the shared engine
        meaning. ``costs`` is the result of a domain's ``constraint_costs``.

        Each selected instance reads its ``constraint_costs()`` key (``name``,
        ``"<name>#<k>"``), or the plain name's entry when the costs hold the
        name once (a repeat of a constraint has the same raw costs), and
        shapes every evaluated unit with its own offset / clip, as the engine
        does. ``aliases`` maps a name (or an instance key) to the engine
        entries it sums.
        """
        aliases = aliases or {}
        totals: dict[str, int | float] = {level: 0 for level in self.levels}
        for key, (spec, config) in self.selected.items():
            level = self.level_of(spec, config)
            weight = spec.weight if config.weight is None else config.weight
            source_names = aliases.get(key, aliases.get(spec.name, key))
            if isinstance(source_names, str):
                source_names = (source_names,)
            raw_values = []
            for source_name in source_names:
                entry = costs.get(source_name)
                if entry is None and source_name == key and key != spec.name:
                    entry = costs[spec.name]
                if entry is None:
                    entry = costs[source_name]  # KeyError names the missing entry
                raw_values.extend(_evaluated(entry))
            if config.offset != 0:
                raw_values = [_shift(value, config.offset, config.clip) for value in raw_values]
            if level == "hard":
                # Match DecomposedScoreCalculator: each unit contribution is
                # rounded independently, half away from zero.
                totals[level] += sum(
                    _round_half_away(weight * value) for value in raw_values
                )
            elif config.offset != 0:
                totals[level] += sum(weight * value for value in raw_values)
            else:
                totals[level] += weight * sum(raw_values)
        result: dict[str, int | float] = {}
        for level, value in totals.items():
            scored = -value if value != 0 else 0
            result[level] = (
                int(scored)
                if isinstance(scored, float) and scored.is_integer()
                else scored
            )
        return result
