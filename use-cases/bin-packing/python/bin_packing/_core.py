"""Dependency-light domain and metaheuristic solver for bin packing."""

from __future__ import annotations

import math
import random
import time
from collections.abc import Callable
from dataclasses import dataclass, field


@dataclass
class Item:
    id: str
    weight: int
    bin: int = -1
    color: str | None = None


@dataclass(order=True, frozen=True)
class Score:
    hard: int = 0
    soft: float = 0.0


@dataclass
class BinPackingProblem:
    bin_capacity: int
    items: list[Item]
    capacity_weight: int = 1
    used_bin_weight: int = 1
    color_weight: int = 1
    conflict_weight: int = 1
    conflicts: list[tuple[str, str]] = field(default_factory=list)
    score: Score = field(default_factory=Score)
    warm_start: bool = False

    def bins_used(self) -> int:
        return len({item.bin for item in self.items if item.bin >= 0})

    def bin_loads(self) -> list[int]:
        if not self.items or all(item.bin < 0 for item in self.items):
            return []
        loads = [0] * (max(item.bin for item in self.items) + 1)
        for item in self.items:
            if item.bin >= 0:
                loads[item.bin] += item.weight
        return loads


@dataclass
class SolverConfig:
    spent_limit_seconds: float = 30.0
    random_seed: int = 0
    parallel_solver_count: int = 1
    max_iterations: int = 50_000
    stagnation_limit: int = 5_000
    temperature: float = 2.0
    cooling_rate: float = 0.9995
    best_score_limit: tuple[int, float] | None = None
    on_new_best: Callable | None = None


def list_constraints() -> list[str]:
    return ["bin_capacity", "color_compatibility", "item_conflicts", "minimize_bins"]


def constraint_costs(problem: BinPackingProblem) -> dict:
    loads = problem.bin_loads()
    colors_by_bin: dict[int, dict[str, int]] = {}
    bins_by_id = {item.id: item.bin for item in problem.items}
    for item in problem.items:
        if item.bin >= 0 and item.color is not None:
            counts = colors_by_bin.setdefault(item.bin, {})
            counts[item.color] = counts.get(item.color, 0) + 1
    return {
        "bin_capacity": {
            "hard": True,
            "weight": problem.capacity_weight,
            "costs": [max(0, load - problem.bin_capacity) for load in loads],
        },
        "minimize_bins": {
            "hard": False,
            "weight": problem.used_bin_weight,
            "costs": [sum(load > 0 for load in loads)],
        },
        "color_compatibility": {
            "hard": True,
            "weight": problem.color_weight,
            "costs": [max(0, count - 1) for counts in colors_by_bin.values() for count in counts.values()],
        },
        "item_conflicts": {
            "hard": True,
            "weight": problem.conflict_weight,
            "costs": [
                int(bins_by_id.get(left, -1) >= 0 and bins_by_id.get(left) == bins_by_id.get(right))
                for left, right in problem.conflicts
            ],
        },
    }


def calculate_score(problem: BinPackingProblem, constraints=None) -> Score:
    from .constraints import score

    value = score(constraint_costs(problem), constraints)
    problem.score = Score(value.hard, value.soft)
    return problem.score


def _compact(assignments: list[int]) -> list[int]:
    remap: dict[int, int] = {}
    return [remap.setdefault(bin_index, len(remap)) for bin_index in assignments]


def _first_fit(weights: list[int], capacity: int, order: list[int], seed: list[int] | None = None) -> list[int]:
    """Place every item of `order` that `seed` leaves at -1, around the loads of the items it already holds."""
    assignments = [-1] * len(weights) if seed is None else seed.copy()
    loads = [0] * (max(assignments, default=-1) + 1)
    for item_index, bin_index in enumerate(assignments):
        if bin_index >= 0:
            loads[bin_index] += weights[item_index]
    for item_index in order:
        if assignments[item_index] >= 0:
            continue
        weight = weights[item_index]
        candidates = [
            (capacity - (load + weight), bin_index) for bin_index, load in enumerate(loads) if load + weight <= capacity
        ]
        if candidates:
            _, selected = min(candidates)
        else:
            selected = len(loads)
            loads.append(0)
        assignments[item_index] = selected
        loads[selected] += weight
    return assignments


def _score_assignments(problem: BinPackingProblem, assignments: list[int], constraints) -> Score:
    for item, bin_index in zip(problem.items, assignments, strict=True):
        item.bin = bin_index
    return calculate_score(problem, constraints)


def _scalar(score: Score) -> float:
    return score.hard * 1_000_000_000.0 + score.soft


def _try_empty_bin(assignments: list[int], weights: list[int], capacity: int, rng: random.Random) -> list[int]:
    loads = [0] * (max(assignments, default=-1) + 1)
    members: list[list[int]] = [[] for _ in loads]
    for item_index, bin_index in enumerate(assignments):
        loads[bin_index] += weights[item_index]
        members[bin_index].append(item_index)
    non_empty = [index for index, items in enumerate(members) if items]
    if len(non_empty) <= 1:
        return assignments
    source = min(non_empty, key=lambda index: (loads[index], rng.random()))
    candidate = assignments.copy()
    targets = [index for index in non_empty if index != source]
    items = members[source].copy()
    rng.shuffle(items)
    items.sort(key=lambda index: weights[index], reverse=True)
    for item_index in items:
        feasible = [
            (capacity - loads[target] - weights[item_index], target)
            for target in targets
            if loads[target] + weights[item_index] <= capacity
        ]
        if not feasible:
            return assignments
        _, target = min(feasible)
        candidate[item_index] = target
        loads[target] += weights[item_index]
    return _compact(candidate)


class Solver:
    def __init__(self, config: SolverConfig | None = None):
        self.config = config or SolverConfig()

    def solve(self, problem: BinPackingProblem) -> Score:
        return self.solve_with_constraints(problem, None)

    def solve_with_constraints(self, problem: BinPackingProblem, constraints) -> Score:
        if problem.bin_capacity <= 0:
            raise ValueError("bin_capacity must be positive")
        if len({item.id for item in problem.items}) != len(problem.items):
            raise ValueError("item ids must be unique")
        if any(item.weight <= 0 for item in problem.items):
            raise ValueError("item weights must be positive")
        if not problem.items:
            problem.score = Score()
            # A seed is used once, so a result edited and solved again starts fresh.
            problem.warm_start = False
            return problem.score

        cfg = self.config
        deadline = time.monotonic() + max(0.001, cfg.spent_limit_seconds)
        rng = random.Random(cfg.random_seed)
        weights = [item.weight for item in problem.items]
        descending = sorted(range(len(weights)), key=lambda index: (-weights[index], index))
        best: list[int] | None = None
        best_score: Score | None = None
        seed: list[int] | None = None
        if problem.warm_start:
            # Unassigned items cost nothing and -1 breaks the moves, so complete the seed before the search.
            seed = _first_fit(weights, problem.bin_capacity, descending, [item.bin for item in problem.items])
            best, best_score = seed.copy(), _score_assignments(problem, seed, constraints)
        else:
            for start in range(max(1, cfg.parallel_solver_count)):
                order = descending.copy()
                if start > 0:
                    rng.shuffle(order)
                candidate = _first_fit(weights, problem.bin_capacity, order)
                candidate_score = _score_assignments(problem, candidate, constraints)
                if best_score is None or candidate_score > best_score:
                    best, best_score = candidate.copy(), candidate_score
        assert best is not None and best_score is not None
        current = best.copy()
        current_score = _score_assignments(problem, current, constraints)
        temperature = max(1e-9, cfg.temperature)
        stagnant = 0

        for iteration in range(max(0, cfg.max_iterations)):
            if time.monotonic() >= deadline:
                break
            if stagnant >= max(1, cfg.stagnation_limit):
                if seed is not None:
                    current = seed.copy()
                else:
                    order = list(range(len(weights)))
                    rng.shuffle(order)
                    order.sort(key=lambda index: -weights[index])
                    current = _first_fit(weights, problem.bin_capacity, order)
                current_score = _score_assignments(problem, current, constraints)
                temperature = max(1e-9, cfg.temperature)
                stagnant = 0

            if rng.random() < 0.35:
                candidate = _try_empty_bin(current, weights, problem.bin_capacity, rng)
            else:
                candidate = current.copy()
                item_index = rng.randrange(len(candidate))
                max_bin = max(candidate, default=0)
                candidate[item_index] = rng.randrange(max_bin + 2)
                if len(candidate) > 1 and rng.random() < 0.25:
                    other = rng.randrange(len(candidate))
                    candidate[item_index], candidate[other] = candidate[other], candidate[item_index]
                candidate = _compact(candidate)

            candidate_score = _score_assignments(problem, candidate, constraints)
            delta = _scalar(candidate_score) - _scalar(current_score)
            if delta >= 0 or rng.random() < math.exp(max(-50.0, delta / temperature)):
                current, current_score = candidate, candidate_score
            if candidate_score > best_score:
                best, best_score = candidate.copy(), candidate_score
                stagnant = 0
                if cfg.on_new_best is not None:
                    _score_assignments(problem, best, constraints)
                    cfg.on_new_best(problem)
                if cfg.best_score_limit is not None and (best_score.hard, best_score.soft) >= tuple(
                    cfg.best_score_limit
                ):
                    break
            else:
                stagnant += 1
            temperature *= cfg.cooling_rate

        _score_assignments(problem, best, constraints)
        # A seed is used once, so a result edited and solved again starts fresh.
        problem.warm_start = False
        return problem.score
