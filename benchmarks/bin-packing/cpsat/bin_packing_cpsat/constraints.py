"""Class-based CP-SAT plugins sharing the canonical domain classes."""

from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, replace
from typing import Any

from bin_packing import constraints as domain
from bin_packing.models import BinPackingProblemModel
from cpsat_plugins import ConstraintSpec, ObjectiveBuilder
from ortools.sat.python import cp_model


@dataclass(frozen=True)
class BinPackingContext:
    model: cp_model.CpModel
    problem: BinPackingProblemModel
    assignments: dict[tuple[int, int], Any]
    used: list[Any]
    overflow: list[Any]
    color_excess: list[Any]
    conflict_violations: list[Any]


class BinCapacity(domain.BinCapacity):
    """Sum of each bin's positive load above capacity."""

    default_level = "hard"
    default_weight = 1
    label = "Bin capacity"

    @property
    def spec(self) -> ConstraintSpec:
        return ConstraintSpec(
            self.name,
            self.label,
            self.default_level,
            self.default_weight,
            type(self).__doc__ or "",
        )

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(context.overflow),
            bound=sum(item.weight for item in context.problem.items),
        )


class MinimizeBins(domain.MinimizeBins):
    """Number of non-empty bins in the solution."""

    default_level = "soft"
    default_weight = 1
    label = "Minimize used bins"

    @property
    def spec(self) -> ConstraintSpec:
        return ConstraintSpec(
            self.name,
            self.label,
            self.default_level,
            self.default_weight,
            type(self).__doc__ or "",
        )

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(self.name, sum(context.used), bound=len(context.problem.items))


class ColorCompatibility(domain.ColorCompatibility):
    """Extra same-color items assigned to one bin."""

    default_level = "hard"
    default_weight = 1
    label = "Color compatibility"

    @property
    def spec(self) -> ConstraintSpec:
        return ConstraintSpec(
            self.name,
            self.label,
            self.default_level,
            self.default_weight,
            type(self).__doc__ or "",
        )

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name, sum(context.color_excess), bound=len(context.problem.items)
        )


class ItemConflicts(domain.ItemConflicts):
    """Forbidden item pairs assigned to one bin."""

    default_level = "hard"
    default_weight = 1
    label = "Item conflicts"

    @property
    def spec(self) -> ConstraintSpec:
        return ConstraintSpec(
            self.name,
            self.label,
            self.default_level,
            self.default_weight,
            type(self).__doc__ or "",
        )

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        objective.add(
            self.name,
            sum(context.conflict_violations),
            bound=len(context.problem.conflicts),
        )


class BinPackingRegistry:
    """Small registry compatible with both old and new cpsat-plugins releases."""

    def __init__(
        self,
        plugins: Sequence[
            BinCapacity | ColorCompatibility | ItemConflicts | MinimizeBins
        ],
    ) -> None:
        self.plugins = tuple(plugins)
        self._by_name = {plugin.name: plugin for plugin in plugins}

    @property
    def specs(self) -> tuple[ConstraintSpec, ...]:
        return tuple(plugin.spec for plugin in self.plugins)

    def catalog(self) -> list[dict[str, Any]]:
        return [spec.as_dict() for spec in self.specs]

    def objective(
        self,
        model: cp_model.CpModel,
        levels: Sequence[str],
        constraints: Iterable[Any | str | Mapping[str, Any]] | None,
        *,
        default_weights: Mapping[str, int] | None = None,
    ) -> ObjectiveBuilder:
        weights = default_weights or {}
        specs = tuple(
            replace(spec, weight=weights.get(spec.name, spec.weight))
            for spec in self.specs
        )
        return ObjectiveBuilder(model, levels, specs, constraints)

    def build(self, context: BinPackingContext, objective: ObjectiveBuilder) -> None:
        for name in objective.selected:
            self._by_name[name].build(context, objective)


REGISTRY = BinPackingRegistry(
    [BinCapacity(), ColorCompatibility(), ItemConflicts(), MinimizeBins()]
)
