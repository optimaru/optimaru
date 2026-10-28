"""Constraint DTOs — the constraint representation exchanged through an API.

Builds a pydantic model **at runtime** from each constraint class in
`registry.CONSTRAINT_TYPES`. A constraint's dataclass fields become the DTO
fields, and `type` is the discriminator naming the constraint. There is no
generated code file, so adding a parameter to a constraint updates the API
schema on the spot — the two cannot drift apart.

    {"type": "weekly_period_counts", "hard": false, "weight": 10000}
    {"type": "teacher_gap", "weight": 20}

A caller unpacks the received DTO list with `to_constraint()` and passes it to
`solve(constraints=...)` as is. To build on top of the defaults use `registry.merge`.
"""

from __future__ import annotations

from functools import reduce
from operator import or_
from typing import Annotated, Any, Literal, get_type_hints

from pydantic import BaseModel, ConfigDict, Field, create_model

from teacher_scheduling.solver.registry import (
    CONSTRAINT_TYPE_BY_NAME,
    CONSTRAINT_TYPES,
    Constraint,
    ConstraintSpec,
    constraint_specs,
)


class ConstraintDtoBase(BaseModel):
    """Common body of the constraint DTOs. Finds the constraint class by `type` and rebuilds it as is."""

    model_config = ConfigDict(extra="forbid")

    type: str

    def to_constraint(self) -> Constraint:
        return CONSTRAINT_TYPE_BY_NAME[self.type](**self.model_dump(exclude={"type"}))


def _dto_for(constraint_type: type[Constraint]) -> type[ConstraintDtoBase]:
    """One constraint class -> one pydantic DTO. The dataclass fields are the DTO fields."""
    hints = get_type_hints(constraint_type)
    fields: dict[str, Any] = {"type": (Literal[constraint_type.name], ...)}
    for field in constraint_type.__dataclass_fields__.values():
        fields[field.name] = (hints[field.name], field.default)
    return create_model(f"{constraint_type.__name__}Dto", __base__=ConstraintDtoBase, **fields)


#: DTO per constraint class. Same order as `CONSTRAINT_TYPES`.
CONSTRAINT_DTOS: tuple[type[ConstraintDtoBase], ...] = tuple(_dto_for(c) for c in CONSTRAINT_TYPES)

#: Discriminated union. pydantic validates against the right DTO by looking at `type`.
ConstraintDto = Annotated[reduce(or_, CONSTRAINT_DTOS), Field(discriminator="type")]


def to_constraints(dtos: list[ConstraintDtoBase]) -> tuple[Constraint, ...]:
    """DTO list -> constraint list."""
    return tuple(dto.to_constraint() for dto in dtos)


def catalog() -> tuple[ConstraintSpec, ...]:
    """The catalog of registered constraints. The source that keeps callers from copying the list by hand."""
    return constraint_specs()


__all__ = [
    "CONSTRAINT_DTOS",
    "ConstraintDto",
    "ConstraintDtoBase",
    "catalog",
    "to_constraints",
]
