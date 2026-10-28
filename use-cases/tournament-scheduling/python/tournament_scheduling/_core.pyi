"""Type stubs for `tournament_scheduling._core` — the tournament-scheduling C++ solver.

The compiled module wraps the C++ domain in `use-cases/tournament-scheduling/cpp`,
which imports the generic engine in `include/optimaru`.

Constraints of the Java quickstart (HardMediumSoftBigDecimalScore) and the score mapping:
  hard 1. oneAssignmentPerDatePerTeam  -1 per pair of same-team assignments on one day
  hard 2. unavailabilityPenalty        -1 when a (team, day) unavailability fact has an assignment
  med  3. fairAssignmentCountPerTeam   LoadBalance unfairness of assignment counts per team
  soft 4. evenlyConfrontationCount     LoadBalance unfairness of head-to-head counts per team pair

The engine score is HardSoftScore(hard int, soft float), so medium is folded into
soft: ``soft = -(medium unfairness * MEDIUM_SCALE + soft unfairness)``
(MEDIUM_SCALE == 1e6; medium always dominates soft). "Unfairness" is
Timefold LoadBalance::unfairness — sqrt(sum(load^2) - sum(load)^2 / n) rounded
to 6 significant digits (HALF_EVEN).
"""

from collections.abc import Callable, Iterable, Sequence
from typing import Any

MEDIUM_SCALE: float

class Score:
    """HardSoft score. `hard` is the integer violation sum (0 == feasible); `soft`
    is the folded medium/soft objective. Lexicographic comparison (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True if hard == 0."""

class Team:
    """Problem fact: a participating team. The list index is the planning variable's value."""
    def __init__(self, name: str) -> None: ...
    name: str

class Day:
    """Problem fact: a match day. Assignments/unavailability reference it by days index."""
    def __init__(self, date_index: int) -> None: ...
    date_index: int

class UnavailabilityPenalty:
    """Problem fact: (team, day) unavailable — one hard point when the cell has at
    least one assignment (one point per fact; Java's forEach + ifExists)."""
    def __init__(self, team: int, day: int) -> None: ...
    team: int
    day: int

class TeamAssignment:
    """Planning entity: one match slot of a day. `team` is the planning variable
    (-1 == unassigned), written by the solver. ids must be unique (Java @PlanningId)."""
    def __init__(
        self,
        id: int,
        day: int,
        index_in_day: int = 0,
        team: int = -1,
        pinned: bool = False,
    ) -> None: ...
    id: int
    day: int
    index_in_day: int
    team: int
    """Index into the teams list; -1 == unassigned."""
    pinned: bool
    """If True, team is fixed (it counts in the score but never moves)."""

class TournamentSchedule:
    """Planning solution: teams (value range) + days + unavailability + assignments (entities).
    solve changes the team_assignments' `team` and `score` in place.

    Note: list attributes such as `team_assignments` return copies, so use
    `set_assignment` to change assignments."""
    def __init__(
        self,
        teams: Sequence[Team],
        days: Sequence[Day],
        unavailability_penalties: Sequence[UnavailabilityPenalty] = (),
        team_assignments: Sequence[TeamAssignment] = (),
    ) -> None: ...
    teams: list[Team]
    days: list[Day]
    unavailability_penalties: list[UnavailabilityPenalty]
    team_assignments: list[TeamAssignment]
    @property
    def score(self) -> Score: ...
    def team_count(self) -> int: ...
    def day_count(self) -> int: ...
    def assignment(self) -> list[int]:
        """Current team index per assignment (-1 == unassigned)."""

    def set_assignment(self, assignment: Sequence[int]) -> None:
        """Bulk-write planning variables (length must equal the number of assignments)."""

def calculate_score(schedule: TournamentSchedule) -> Score:
    """Full recalculation (also stored in schedule.score)."""

def unfairness(loads: Sequence[int]) -> float:
    """Timefold LoadBalance::unfairness (6 significant digits, HALF_EVEN)."""

class SolverConfig:
    """Engine settings. Default: 30 s budget, LA + ALNS, single start."""
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = automatic: clamp(8 * assignments, 400, 3200)."""
    full_assert: bool
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    change_move_weight: float
    swap_move_weight: float
    descent_weight: float
    """Probability that a step runs a best-improvement descent over a nearby entity pair
    instead of the move mix (one sweep ≈ 2 * teams evaluations). Default 0 = off."""
    ruin_recreate_as_move_weight: float
    """Weight of destroy-repair as an ordinary move judged by the acceptor
    (separate from the stagnation kick). Default 0 = off."""
    ruin_recreate_as_move_max_ruin: int
    custom_move_probability: float
    """Upper bound on the probability of drawing the custom operator (TeamCountMove) in a step. 1 = unlimited."""
    value_recommender: bool
    """Steers the value choice of change moves to hard-feasible teams (no match that day +
    not unavailable) (monolithic path only). Default False."""
    team_count_moves: bool
    """Macro moves on the number of loaded teams (empty / revive a team). The only path
    across the medium-level team-count barrier, so default True."""
    best_score_limit: tuple[int, float]
    """Stop early at or above this score. E.g. (0, -1e18) == stop at the first feasible."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: TournamentSchedule) -> None:
        """Solves in place: writes team_assignments[*].team and schedule.score.
        Pinned assignments are kept."""

    def solve_with_constraints(self, schedule: TournamentSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see tournament_scheduling.constraints). The given list replaces the default set; None
        keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case).
    The same names tournament_scheduling.constraints uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/level rounding.
    `tournament_scheduling.constraints.score()` takes these and produces a score.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
