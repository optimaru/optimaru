from __future__ import annotations

import math
import random
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from typing import ClassVar, NamedTuple, TypeVar


@dataclass(frozen=True)
class Constraint:
    weight: float | None = None
    hard: bool | None = None
    exponent: float = 1.0
    name: ClassVar[str] = ""
    default_hard: ClassVar[bool] = False
    default_weight: ClassVar[float] = 1.0
    label: ClassVar[str] = ""


class Score(NamedTuple):
    hard: int
    soft: float


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


def score_costs(costs: dict, constraints: Sequence[Constraint] | None, defaults: Sequence[Constraint]) -> Score:
    selected = defaults if constraints is None else constraints
    if not selected:
        raise ValueError("constraints must not be empty (pass None for defaults)")
    hard = 0.0
    soft = 0.0
    for constraint in selected:
        if constraint.name not in costs:
            raise ValueError(f"unknown constraint: {constraint.name}")
        raw = costs[constraint.name]
        weight = constraint.default_weight if constraint.weight is None else constraint.weight
        is_hard = constraint.default_hard if constraint.hard is None else constraint.hard
        if constraint.exponent <= 0:
            raise ValueError("constraint exponent must be positive")
        total = sum(math.copysign(abs(value) ** constraint.exponent, value) for value in raw)
        if is_hard:
            hard += weight * total
        else:
            soft += weight * total
    rounded_hard = int(math.floor(hard + 0.5) if hard >= 0 else math.ceil(hard - 0.5))
    return Score(-rounded_hard, -soft if soft else 0.0)


State = TypeVar("State")


def anneal(
    initial: State,
    neighbor: Callable[[State, random.Random], State],
    evaluate: Callable[[State], Score],
    config: SolverConfig,
) -> tuple[State, Score]:
    rng = random.Random(config.random_seed)
    deadline = time.monotonic() + max(0.001, config.spent_limit_seconds)
    starts = [initial]
    for start in range(1, max(1, config.parallel_solver_count)):
        candidate = initial
        for _ in range(start * 3):
            candidate = neighbor(candidate, rng)
        starts.append(candidate)
    scored_starts = [(candidate, evaluate(candidate)) for candidate in starts]
    current, current_score = max(scored_starts, key=lambda item: item[1])
    best, best_score = current, current_score
    temperature = max(1e-9, config.temperature)
    stagnant = 0
    for _ in range(max(0, config.max_iterations)):
        if time.monotonic() >= deadline:
            break
        candidate = neighbor(current, rng)
        candidate_score = evaluate(candidate)
        delta = (candidate_score.hard - current_score.hard) * 1_000_000_000 + (
            candidate_score.soft - current_score.soft
        )
        if delta >= 0 or rng.random() < math.exp(max(-50.0, delta / temperature)):
            current, current_score = candidate, candidate_score
        if candidate_score > best_score:
            best, best_score = candidate, candidate_score
            stagnant = 0
            if config.on_new_best is not None:
                config.on_new_best(candidate)
            if config.best_score_limit is not None and tuple(best_score) >= tuple(config.best_score_limit):
                break
        else:
            stagnant += 1
        if stagnant >= max(1, config.stagnation_limit):
            current, current_score = initial, evaluate(initial)
            stagnant = 0
            temperature = max(1e-9, config.temperature)
        temperature *= config.cooling_rate
    return best, best_score


def configure(
    config: SolverConfig | None,
    time_limit_seconds: float | None,
    random_seed: int | None,
    parallel_solver_count: int | None,
) -> SolverConfig:
    cfg = config or SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    return cfg
