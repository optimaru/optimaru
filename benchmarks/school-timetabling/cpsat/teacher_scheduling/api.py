"""제약 DTO — API 로 주고받는 제약 표현.

`registry.CONSTRAINT_TYPES` 의 각 제약 클래스에서 pydantic 모델을 **런타임에**
만든다. 제약의 데이터클래스 필드가 그대로 DTO 필드가 되고, `type` 이 어느
제약인지 가리키는 판별자다. 코드 생성 파일이 따로 없으니 제약에 파라미터를
하나 추가하면 API 스키마가 그 자리에서 따라온다 — 두 곳이 어긋날 일이 없다.

    {"type": "weekly_period_counts", "hard": false, "weight": 10000}
    {"type": "teacher_gap", "weight": 20}

서버는 받은 DTO 목록을 `to_constraint()` 로 풀어 `solve(constraints=...)` 에
그대로 넘긴다. 기본 제약 위에 얹으려면 `registry.merge` 를 쓴다.
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
    """제약 DTO 공통 몸통. `type` 으로 제약 클래스를 찾아 그대로 되살린다."""

    model_config = ConfigDict(extra="forbid")

    type: str

    def to_constraint(self) -> Constraint:
        return CONSTRAINT_TYPE_BY_NAME[self.type](**self.model_dump(exclude={"type"}))


def _dto_for(constraint_type: type[Constraint]) -> type[ConstraintDtoBase]:
    """제약 클래스 하나 -> pydantic DTO 하나. 데이터클래스 필드가 곧 DTO 필드다."""
    hints = get_type_hints(constraint_type)
    fields: dict[str, Any] = {"type": (Literal[constraint_type.name], ...)}
    for field in constraint_type.__dataclass_fields__.values():
        fields[field.name] = (hints[field.name], field.default)
    return create_model(f"{constraint_type.__name__}Dto", __base__=ConstraintDtoBase, **fields)


#: 제약 클래스별 DTO. 순서는 `CONSTRAINT_TYPES` 와 같다.
CONSTRAINT_DTOS: tuple[type[ConstraintDtoBase], ...] = tuple(_dto_for(c) for c in CONSTRAINT_TYPES)

#: 판별자 union. pydantic 이 `type` 을 보고 알맞은 DTO 로 검증한다.
ConstraintDto = Annotated[reduce(or_, CONSTRAINT_DTOS), Field(discriminator="type")]


def to_constraints(dtos: list[ConstraintDtoBase]) -> tuple[Constraint, ...]:
    """DTO 목록 -> 제약 목록."""
    return tuple(dto.to_constraint() for dto in dtos)


def catalog() -> tuple[ConstraintSpec, ...]:
    """등록된 제약의 카탈로그. 서버가 목록을 손으로 베끼지 않게 하는 원본."""
    return constraint_specs()


__all__ = [
    "CONSTRAINT_DTOS",
    "ConstraintDto",
    "ConstraintDtoBase",
    "catalog",
    "to_constraints",
]
