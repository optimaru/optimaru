from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import replace

from ortools.sat.python import cp_model


def catalog(defaults) -> list[dict]:
    return [
        {
            "name": item.name,
            "label": item.label,
            "description": type(item).__doc__ or "",
            "kind": "hard" if item.default_hard else "soft",
            "default_enabled": True,
            "params": {"weight": item.default_weight, "hard": item.default_hard},
        }
        for item in defaults
    ]


def selections(defaults, constraints: Iterable | None):
    by_name = {item.name: item for item in defaults}
    if constraints is None:
        return list(defaults)
    result = []
    for value in constraints:
        if isinstance(value, str):
            row = {"name": value}
        elif isinstance(value, Mapping):
            row = dict(value)
        else:
            row = {"name": value.name, "weight": value.weight, "hard": value.hard, "exponent": value.exponent}
        if not row.pop("enabled", True):
            continue
        if row.pop("exponent", 1.0) != 1.0:
            raise ValueError("constraint exponent is not supported by CP-SAT")
        name = row.pop("name")
        if name not in by_name:
            raise ValueError(f"unknown constraint: {name}")
        result.append(replace(by_name[name], **{key: val for key, val in row.items() if val is not None}))
    return result


class Objective:
    def __init__(self, model: cp_model.CpModel, selected) -> None:
        self.model = model
        self.selected = {item.name: item for item in selected}
        self.hard = []
        self.soft = []
        self.soft_bound = 0

    def add(self, name: str, expression, bound: int, *, reward: bool = False) -> None:
        item = self.selected.get(name)
        if item is None:
            return
        weight = item.default_weight if item.weight is None else item.weight
        if isinstance(weight, float):
            if not weight.is_integer():
                raise ValueError("CP-SAT weights must be integers")
            weight = int(weight)
        term = weight * (-expression if reward else expression)
        is_hard = item.default_hard if item.hard is None else item.hard
        (self.hard if is_hard else self.soft).append(term)
        if not is_hard:
            self.soft_bound += abs(weight) * bound

    def minimize(self) -> None:
        self.model.minimize((self.soft_bound + 1) * sum(self.hard) + sum(self.soft))


def solve_model(model, *, time_limit_seconds: float, random_seed: int, num_workers: int):
    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = solver.solve(model)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(f"CP-SAT did not find a solution (status={solver.status_name(status)})")
    return solver
