from __future__ import annotations

from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
import math
import time
from typing import Any, Protocol, TypeAlias

from ortools.sat.python import cp_model

# Folded objective coefficients must stay inside int64 with headroom.
_FOLD_LIMIT = 2**62


@dataclass(frozen=True)
class ConstraintSpec:
    """One catalog entry and its default score semantics."""

    name: str
    label: str
    level: str
    weight: int = 1
    description: str = ""
    label_en: str = ""
    description_en: str = ""

    def as_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "label": self.label,
            "description": self.description,
            "label_en": self.label_en,
            "description_en": self.description_en,
            "kind": "hard" if self.level == "hard" else "soft",
            "default_enabled": True,
            "params": {"weight": self.weight, "hard": self.level == "hard"},
        }


@dataclass(frozen=True)
class ConstraintConfig:
    """Runtime selection. A supplied list has replace semantics."""

    name: str
    enabled: bool = True
    weight: int | None = None
    hard: bool | None = None

    @classmethod
    def parse(
        cls, value: ConstraintConfig | DomainConstraint | str | Mapping[str, Any]
    ) -> ConstraintConfig:
        if isinstance(value, cls):
            return value
        if isinstance(value, str):
            return cls(name=value)
        if not isinstance(value, Mapping) and hasattr(value, "name"):
            exponent = getattr(value, "exponent", 1.0)
            if exponent != 1.0:
                raise ValueError(
                    f"constraint {value.name!r} exponent is not supported by CP-SAT"
                )
            return cls(
                name=str(value.name),
                weight=getattr(value, "weight", None),
                hard=getattr(value, "hard", None),
            )
        payload = dict(value)
        name = payload.pop("name", payload.pop("type", None))
        if name is None:
            raise ValueError("constraint config needs 'name' or 'type'")
        unknown = set(payload) - {"enabled", "weight", "hard"}
        if unknown:
            raise ValueError(
                f"unknown constraint parameters for {name!r}: {sorted(unknown)}"
            )
        return cls(name=str(name), **payload)


class DomainConstraint(Protocol):
    """Shared engine-side constraint selection accepted by CP-SAT too."""

    name: str
    weight: float | None
    hard: bool | None
    exponent: float


ConstraintSelection: TypeAlias = (
    ConstraintConfig | DomainConstraint | str | Mapping[str, Any]
)


def resolve(
    specs: Sequence[ConstraintSpec],
    constraints: Iterable[ConstraintSelection] | None,
) -> dict[str, tuple[ConstraintSpec, ConstraintConfig]]:
    """Resolve defaults or an explicit replacement list into a name map."""
    by_name = {spec.name: spec for spec in specs}
    selected = (
        [ConstraintConfig(name=spec.name) for spec in specs]
        if constraints is None
        else [ConstraintConfig.parse(value) for value in constraints]
    )
    result: dict[str, tuple[ConstraintSpec, ConstraintConfig]] = {}
    for config in selected:
        if config.name not in by_name:
            raise ValueError(
                f"unknown constraint {config.name!r}; available: {sorted(by_name)}"
            )
        if config.name in result:
            raise ValueError(f"duplicate constraint {config.name!r}")
        if config.enabled:
            result[config.name] = (by_name[config.name], config)
    return result


class ObjectiveBuilder:
    """Collect named penalty terms and build a safe lexicographic objective."""

    def __init__(
        self,
        model: cp_model.CpModel,
        levels: Sequence[str],
        specs: Sequence[ConstraintSpec],
        constraints: Iterable[ConstraintSelection] | None = None,
    ) -> None:
        if not levels or levels[0] != "hard":
            raise ValueError("levels must start with 'hard'")
        self.model = model
        self.levels = tuple(levels)
        self.selected = resolve(specs, constraints)
        self.terms: dict[str, list] = {level: [] for level in self.levels}
        self.bounds: dict[str, int] = {level: 0 for level in self.levels}

    def enabled(self, name: str) -> bool:
        return name in self.selected

    def add(self, name: str, magnitude, *, bound: int) -> None:
        """Add a non-negative raw penalty and a valid magnitude upper bound."""
        selected = self.selected.get(name)
        if selected is None:
            return
        spec, config = selected
        level = spec.level
        if config.hard is True:
            level = "hard"
        elif config.hard is False and level == "hard":
            level = self.levels[1] if len(self.levels) > 1 else "hard"
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
        # A zero upper bound means this penalty is identically zero. Keeping
        # ``weight * 0`` creates OR-Tools' FloatAffine(IntConstant(0), ...),
        # which CpSolver.value() cannot evaluate when scores are reported.
        if weight and bound > 0:
            self.terms[level].append(weight * magnitude)
            self.bounds[level] += weight * max(0, int(bound))

    def add_reward(self, name: str, magnitude, *, bound: int) -> None:
        """Add a non-negative reward (represented as a negative penalty)."""
        self.add(name, -magnitude, bound=bound)

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
        stages = [level for level in self.levels if self.terms[level]]
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
            level: -sum(int(solver.value(term)) for term in self.terms[level])
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
        """
        aliases = aliases or {}
        totals: dict[str, int | float] = {level: 0 for level in self.levels}
        for name, (spec, config) in self.selected.items():
            level = spec.level
            if config.hard is True:
                level = "hard"
            elif config.hard is False and level == "hard":
                level = self.levels[1] if len(self.levels) > 1 else "hard"
            weight = spec.weight if config.weight is None else config.weight
            source_names = aliases.get(name, name)
            if isinstance(source_names, str):
                source_names = (source_names,)
            raw_values = [
                value
                for source_name in source_names
                for value in costs[source_name]["costs"]
            ]
            if level == "hard":
                # Match DecomposedScoreCalculator: each unit contribution is
                # rounded independently, half away from zero.
                totals[level] += sum(
                    math.floor(weight * value + 0.5)
                    if weight * value >= 0
                    else math.ceil(weight * value - 0.5)
                    for value in raw_values
                )
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

    def catalog(self) -> list[dict[str, Any]]:
        return [spec.as_dict() for spec, _config in self.selected.values()]
