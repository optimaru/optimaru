"""Class-based CP-SAT constraint plugin contract.

Domain packages already expose one ``Constraint`` class hierarchy for the
optimaru engine. A CP-SAT plugin subclasses the matching domain
constraint and this mixin, and owns the code that adds its expression to the
model. The solver therefore only creates planning variables and asks the
registry to build the selected plugins.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import replace
from typing import Any, Generic, TypeVar

from ortools.sat.python import cp_model

from .registry import ConstraintSelection, ConstraintSpec, ObjectiveBuilder

ContextT = TypeVar("ContextT")


class ConstraintPlugin(ABC, Generic[ContextT]):
    """Engine-specific builder mixed into a domain ``Constraint`` subclass."""

    default_level: str
    default_weight: int = 1
    label: str = ""

    @property
    def spec(self) -> ConstraintSpec:
        name = getattr(self, "name", "")
        if not name:
            raise TypeError(f"{type(self).__name__} must inherit a named Constraint")
        return ConstraintSpec(
            name=name,
            label=self.label
            or getattr(type(self), "label_en", "")
            or type(self).__name__,
            level=self.default_level,
            weight=self.default_weight,
            description=(
                getattr(type(self), "description_en", "")
                or (type(self).__doc__ or "").strip()
            ),
            label_ko=getattr(type(self), "label_ko", ""),
            label_en=getattr(type(self), "label_en", ""),
            description_ko=getattr(type(self), "description_ko", ""),
            description_en=getattr(type(self), "description_en", ""),
        )

    @abstractmethod
    def build(self, context: ContextT, objective: ObjectiveBuilder) -> None:
        """Add this constraint's complete CP-SAT expression to ``objective``."""


class ConstraintRegistry(Generic[ContextT]):
    """Ordered, validated assembly of class-based constraint plugins."""

    def __init__(self, plugins: Sequence[ConstraintPlugin[ContextT]]) -> None:
        self.plugins = tuple(plugins)
        names = [plugin.spec.name for plugin in self.plugins]
        if len(names) != len(set(names)):
            duplicates = sorted({name for name in names if names.count(name) > 1})
            raise ValueError(f"duplicate constraint plugins: {duplicates}")
        self._by_name = dict(zip(names, self.plugins, strict=True))

    @property
    def specs(self) -> tuple[ConstraintSpec, ...]:
        return tuple(plugin.spec for plugin in self.plugins)

    def objective(
        self,
        model: cp_model.CpModel,
        levels: Sequence[str],
        constraints: Iterable[ConstraintSelection] | None,
        *,
        default_weights: Mapping[str, int] | None = None,
    ) -> ObjectiveBuilder:
        default_weights = default_weights or {}
        specs = tuple(
            replace(spec, weight=default_weights.get(spec.name, spec.weight))
            for spec in self.specs
        )
        return ObjectiveBuilder(model, levels, specs, constraints)

    def build(self, context: ContextT, objective: ObjectiveBuilder) -> None:
        for name in objective.selected:
            self._by_name[name].build(context, objective)

    def catalog(self) -> list[dict[str, Any]]:
        return [plugin.spec.as_dict() for plugin in self.plugins]
