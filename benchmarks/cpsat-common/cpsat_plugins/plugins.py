"""Class-based CP-SAT constraint plugin contract.

Domain packages already expose one ``Constraint`` class hierarchy for the
optimaru engine. A CP-SAT plugin subclasses the matching domain
constraint and this mixin, and owns the code that adds its expression to the
model. The solver therefore only creates planning variables and asks the
registry to build the selected plugins.

Default level and weight come from the engine. When the domain package exposes
``constraint_info()`` (every package migrated to the 0.2.0 contract does), the
plugin's ``spec`` reads its row: ``default_level``, ``default_weight``,
``level_flippable`` and ``reward`` are the C++ component's constants, so the
two solvers cannot disagree on a default. A migrated plugin must NOT restate
``default_level`` / ``default_weight``; doing so raises TypeError.
"""

from __future__ import annotations

import sys
from abc import ABC, abstractmethod
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import replace
from typing import Any, Generic, TypeVar

from ortools.sat.python import cp_model

from .registry import ConstraintSelection, ConstraintSpec, ObjectiveBuilder

ContextT = TypeVar("ContextT")

# Per domain package: {constraint name: its constraint_info() row}.
_ENGINE_CATALOGS: dict[str, dict[str, Any]] = {}


def engine_constraint_info(cls: type) -> dict[str, Any] | None:
    """The ``constraint_info()`` rows of the domain package ``cls`` derives from.

    Walks the MRO and, for each class, its module's parent packages
    (``facility_location.constraints`` -> ``facility_location``), returning the
    first package's catalog keyed by name. None when no package in the chain
    exposes ``constraint_info`` (a domain not yet on the 0.2.0 contract).
    """
    for base in cls.__mro__:
        package_name = getattr(base, "__module__", "").rpartition(".")[0]
        while package_name:
            cached = _ENGINE_CATALOGS.get(package_name)
            if cached is not None:
                return cached
            provider = getattr(sys.modules.get(package_name), "constraint_info", None)
            if callable(provider):
                catalog = {row.name: row for row in provider()}
                _ENGINE_CATALOGS[package_name] = catalog
                return catalog
            package_name = package_name.rpartition(".")[0]
    return None


class ConstraintPlugin(ABC, Generic[ContextT]):
    """Engine-specific builder mixed into a domain ``Constraint`` subclass.

    0.2.0 transition shim: removed once every use case is migrated. A plugin of
    a domain without ``constraint_info()`` still declares its defaults by hand
    as ClassVars (``default_level`` required, ``default_weight`` defaulting to
    1), exactly as before 0.2.0.
    """

    label: str = ""

    @property
    def spec(self) -> ConstraintSpec:
        cls = type(self)
        name = getattr(self, "name", "")
        if not name:
            raise TypeError(f"{cls.__name__} must inherit a named Constraint")
        level, weight, flippable, reward = self._defaults(name)
        return ConstraintSpec(
            name=name,
            label=self.label or getattr(cls, "label_en", "") or cls.__name__,
            level=level,
            weight=weight,
            description=(getattr(cls, "description_en", "") or (cls.__doc__ or "").strip()),
            label_en=getattr(cls, "label_en", ""),
            description_en=getattr(cls, "description_en", ""),
            level_flippable=flippable,
            reward=reward,
        )

    def _defaults(self, name: str) -> tuple[str, int | float, bool, bool]:
        cls = type(self)
        catalog = engine_constraint_info(cls)
        if catalog is not None:
            for attr in ("default_level", "default_weight"):
                if getattr(self, attr, None) is not None:
                    raise TypeError(
                        f"{cls.__name__}.{attr} restates an engine default; "
                        f"{name!r} takes it from the domain's constraint_info()"
                    )
            row = catalog.get(name)
            if row is None:
                raise TypeError(f"{cls.__name__}: {name!r} is not in the domain's constraint_info()")
            weight = row.default_weight
            if float(weight).is_integer():
                weight = int(weight)  # CP-SAT objectives are integral
            return row.default_level, weight, row.level_flippable, row.reward
        # 0.2.0 transition shim: removed once every use case is migrated. The
        # defaults may be ClassVars or, as employee-scheduling's RulePlugin
        # sets them, instance attributes.
        level = getattr(self, "default_level", None)
        if level is None:
            raise TypeError(f"{cls.__name__} must declare default_level (its domain has no constraint_info())")
        weight = getattr(self, "default_weight", None)
        return level, 1 if weight is None else weight, True, False

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
        """``default_weights`` is a 0.2.0 transition shim: removed once every
        use case is migrated (a migrated domain's defaults all come from
        constraint_info(), and its canonical model carries no weights)."""
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
