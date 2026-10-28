"""Small, dependency-light constraint registry shared by CP-SAT backends.

Each backend owns its domain formulation. This package only standardizes how
callers select a subset, change a weight, move a term between hard/soft
levels, and expose the same catalog to an API or UI.
"""

from .plugins import ConstraintPlugin, ConstraintRegistry
from .registry import (
    ConstraintConfig,
    ConstraintSelection,
    ConstraintSpec,
    DomainConstraint,
    ObjectiveBuilder,
    resolve,
)

__all__ = [
    "ConstraintConfig",
    "ConstraintSelection",
    "ConstraintPlugin",
    "ConstraintRegistry",
    "ConstraintSpec",
    "DomainConstraint",
    "ObjectiveBuilder",
    "resolve",
]
