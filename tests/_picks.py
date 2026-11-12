"""Pick lists built from the defaults by name, for the tests.

The docs write the plain comprehension (``[X() if spec.name == "x_cost" else spec for spec in
get_constraints()]``); here a misspelt name fails instead of leaving the list unchanged.
"""

from __future__ import annotations


def _matches(specs, name: str) -> int:
    found = sum(spec.name == name for spec in specs)
    assert found == 1, f"{name!r} names {found} of the picks {[spec.name for spec in specs]}, not exactly one"
    return found


def replace_default(specs, name: str, new):
    """``specs`` in their order, the one pick named ``name`` replaced by ``new``."""
    _matches(specs, name)
    return [new if spec.name == name else spec for spec in specs]


def without(specs, name: str):
    """``specs`` in their order, without the one pick named ``name``."""
    _matches(specs, name)
    return [spec for spec in specs if spec.name != name]


def only(specs, *names: str):
    """The picks with these names, one each, in the order of ``specs``."""
    for name in names:
        _matches(specs, name)
    return [spec for spec in specs if spec.name in names]
