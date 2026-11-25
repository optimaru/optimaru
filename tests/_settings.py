"""A ``SolverConfig`` setting by its ``group.name`` (optimaru._config), for tests that loop over settings."""

from typing import Any


def get_setting(config: Any, path: str) -> Any:
    group, name = path.split(".")
    return getattr(getattr(config, group), name)


def set_setting(config: Any, path: str, value: Any) -> None:
    group, name = path.split(".")
    setattr(getattr(config, group), name, value)
