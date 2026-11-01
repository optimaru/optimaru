"""Class-based CP-SAT constraint plugin contract.

Domain packages already expose one ``Constraint`` class hierarchy for the
optimaru engine. A CP-SAT plugin subclasses the matching domain
constraint and this mixin, and owns the code that adds its expression to the
model. The solver therefore only creates planning variables and asks the
registry to build the selected plugins.

Default level and weight come from the engine. Every domain package exposes
``constraint_info()``, and the plugin's ``spec`` reads its row:
``default_level``, ``default_weight``, ``level_flippable`` and ``reward`` are
the C++ component's constants, so the two solvers cannot disagree on a
default. A plugin must NOT restate ``default_level`` / ``default_weight``;
doing so raises TypeError, and so does a plugin whose domain package has no
``constraint_info()``.

Repeated picks: a selection may name one constraint several times
(``resolve`` keys them ``name``, ``"<name>#<k>"``). The registry builds the
plugin once per instance, each time through a view of the objective whose
``add`` / ``add_reward`` / ``selected[self.name]`` address that instance, so a
plugin written for one instance needs no change.

Offsets: ``ObjectiveBuilder.add`` shapes each call's term on its own, so an
``offset`` is honoured only by a plugin that adds one term per engine unit
(and one for a global's total), every evaluated unit included. Such a plugin
sets ``terms_per_unit = True``; for any other the registry refuses a non-zero
offset (``constraint 'x' offset is not supported by this CP-SAT model``).
``clip=False`` with a non-zero offset is refused on a level that counts toward
the hard score (hard, or medium with ``medium_fold="below_hard"``), with the
engine's message, whatever the plugin.
"""

from __future__ import annotations

import sys
from abc import ABC, abstractmethod
from collections.abc import Iterable, Sequence
from typing import Any, ClassVar, Generic, TypeVar

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
    exposes ``constraint_info``.
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

    Its defaults are the domain's ``constraint_info()`` row for its name.
    """

    label: str = ""
    # build() adds one term per engine unit (every evaluated unit, a constant
    # 0 included) or, for a global constraint, one term for its total: the
    # granularity the engine shapes at, so offset / clip are honoured.
    terms_per_unit: ClassVar[bool] = False

    @property
    def spec(self) -> ConstraintSpec:
        cls = type(self)
        name = getattr(self, "name", "")
        if not name:
            raise TypeError(f"{cls.__name__} must inherit a named Constraint")
        level, weight, flippable, reward, enabled = self._defaults(name)
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
            default_enabled=enabled,
        )

    def _defaults(self, name: str) -> tuple[str, int | float, bool, bool, bool | None]:
        cls = type(self)
        catalog = engine_constraint_info(cls)
        if catalog is None:
            raise TypeError(
                f"{cls.__name__} must derive from a domain constraint whose package exposes constraint_info()"
            )
        for attr in ("default_level", "default_weight"):
            if getattr(self, attr, None) is not None:
                raise TypeError(
                    f"{cls.__name__}.{attr} restates an engine default; "
                    f"{name!r} takes it from the domain's constraint_info()"
                )
        # A plugin per instance of a repeated rule is named "<name>#<k>"
        # (the constraint_costs() key rule); its row is the plain name's.
        row = catalog.get(name.partition("#")[0])
        if row is None:
            raise TypeError(f"{cls.__name__}: {name!r} is not in the domain's constraint_info()")
        weight = row.default_weight
        if float(weight).is_integer():
            weight = int(weight)  # CP-SAT objectives are integral
        enabled = getattr(row, "default_enabled", None)
        return row.default_level, weight, row.level_flippable, row.reward, enabled

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
        medium_fold: str | None = None,
    ) -> ObjectiveBuilder:
        """The objective over this registry's specs; defaults come from constraint_info().

        A name picked several times gives one instance per pick. A non-zero
        offset is refused for a plugin that does not add per-unit terms, and
        ``clip=False`` with an offset on a level that counts toward the hard
        score is refused for every plugin (``ObjectiveBuilder.level_of``;
        ``medium_fold`` as there: ``"below_hard"`` where medium counts toward
        the hard score).
        """
        objective = ObjectiveBuilder(model, levels, self.specs, constraints, medium_fold=medium_fold)
        for key, (spec, config) in objective.selected.items():
            if config.offset != 0 and not self.plugin_for(key, spec.name).terms_per_unit:
                raise ValueError(
                    f"constraint {spec.name!r} offset is not supported by this CP-SAT model"
                )
        return objective

    def plugin_for(self, key: str, name: str) -> ConstraintPlugin[ContextT]:
        """The plugin of an instance key: its own (a registry with a plugin per
        instance) or the one of its name."""
        plugin = self._by_name.get(key)
        return plugin if plugin is not None else self._by_name[name]

    def build(self, context: ContextT, objective: ObjectiveBuilder) -> None:
        for key, (spec, _config) in objective.selected.items():
            plugin = self.plugin_for(key, spec.name)
            view = objective if key == plugin.name else InstanceObjective(objective, key, plugin.name)
            plugin.build(context, view)

    def catalog(self) -> list[dict[str, Any]]:
        return [plugin.spec.as_dict() for plugin in self.plugins]


class InstanceObjective:
    """The objective as the plugin of one repeated instance sees it.

    ``add`` / ``add_reward`` under the plugin's own name land on the
    instance's key, and ``selected[name]`` returns the instance's
    ``(spec, config)``; everything else is the objective's.
    """

    def __init__(self, objective: ObjectiveBuilder, key: str, name: str) -> None:
        self._objective = objective
        self._key = key
        self._name = name

    def _target(self, name: str) -> str:
        return self._key if name == self._name else name

    def add(self, name: str, magnitude, *, bound: int) -> None:
        self._objective.add(self._target(name), magnitude, bound=bound)

    def add_reward(self, name: str, magnitude, *, bound: int) -> None:
        self._objective.add_reward(self._target(name), magnitude, bound=bound)

    @property
    def selected(self) -> dict:
        view = dict(self._objective.selected)
        view[self._name] = self._objective.selected[self._key]
        return view

    def __getattr__(self, attribute: str):
        return getattr(self._objective, attribute)
