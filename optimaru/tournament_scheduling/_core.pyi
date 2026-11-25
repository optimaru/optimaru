"""Type stubs for `optimaru.tournament_scheduling._core` — the tournament-scheduling C++ solver.

The compiled module wraps the C++ domain in `optimaru/tournament_scheduling/cpp`,
which imports the generic engine in `core/optimaru`.

Constraints (hard / medium / soft levels) and the score mapping
(registry names; default weights per raw unit, all 1):
  hard 1. team_conflict_cost                  -1 per pair of same-team assignments on one day
  hard 2. team_unavailability_cost            -1 when a (team, day) unavailability fact has an
                                              assignment
  med  3. assignment_count_imbalance_cost     LoadBalance unfairness of assignment counts per team
                                              (-1 medium per raw unit)
  soft 4. confrontation_count_imbalance_cost  LoadBalance unfairness of head-to-head counts per
                                              team pair

The engine score is Score(hard int, soft float), so medium is folded into
soft: ``soft = -(medium unfairness * MEDIUM_SCALE + soft unfairness)``
(MEDIUM_SCALE == 1e6; medium always dominates soft). "Unfairness" is
load-balance unfairness — sqrt(sum(load^2) - sum(load)^2 / n) rounded
to 6 significant digits (HALF_EVEN).
"""

from collections.abc import Callable, Iterable, Sequence
from typing import Any, Literal, NamedTuple

MEDIUM_SCALE: float

class Score:
    """Two-level score (hard, soft). `hard` is the integer violation sum (0 == feasible); `soft`
    is the folded medium/soft objective. Lexicographic comparison (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True when hard >= 0: no hard constraint is violated."""

class Team:
    """Problem fact: a participating team. Its list index is the value assigned to `TeamAssignment.team`."""
    def __init__(self, name: str) -> None: ...
    name: str

class Day:
    """Problem fact: a match day. Assignments/unavailability reference it by days index."""
    def __init__(self, date_index: int) -> None: ...
    date_index: int

class UnavailabilityPenalty:
    """Problem fact: (team, day) unavailable — one hard point of team_unavailability_cost
    when the cell has at least one assignment (one point per fact, however many assignments the cell holds)."""
    def __init__(self, team: int, day: int) -> None: ...
    team: int
    day: int

class TeamAssignment:
    """One match slot of a day. `team` is the decision variable
    (-1 == unassigned), written by the solver. ids must be unique."""
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
    """The problem and its solution: teams (the values) + days + unavailability + assignments.
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
        """Bulk-write the team of every assignment (length must equal the number of assignments)."""

def calculate_score(schedule: TournamentSchedule) -> Score:
    """Full recalculation (also stored in schedule.score)."""

def unfairness(loads: Sequence[int]) -> float:
    """Load-balance unfairness (6 significant digits, HALF_EVEN)."""

class SolverConfig:
    """The solver settings, by stage of the solve: ``stop``, ``alns_trigger``, ``strategies``, ``custom_moves``, ``moves``, ``selection``, ``acceptance``, ``alns``, ``run``.

    Read and write a setting as ``config.<group>.<name>``, or give whole groups when making it:
    ``SolverConfig(stop={"time_limit_seconds": 10})``. See `optimaru._config`."""

    def __init__(
        self,
        *,
        stop: dict[str, object] | None = None,
        alns_trigger: dict[str, object] | None = None,
        strategies: dict[str, object] | None = None,
        custom_moves: dict[str, object] | None = None,
        moves: dict[str, object] | None = None,
        selection: dict[str, object] | None = None,
        acceptance: dict[str, object] | None = None,
        alns: dict[str, object] | None = None,
        run: dict[str, object] | None = None,
    ) -> None: ...
    GROUPS: tuple[str, ...]
    def to_dict(self) -> dict[str, dict[str, object]]:
        """Every setting by group and name: the shape of a request's ``solver_parameters``."""
        ...

    class Stop:
        """``config.stop``: When the search stops: the first limit reached."""

        time_limit_seconds: float
        unimproved_time_limit_seconds: float
        step_limit: int
        unimproved_step_limit: int
        target_score: tuple[int, float]
        """Stop early at or above this score. E.g. (0, -inf) == stop at the first feasible."""

    stop: Stop
    """When the search stops: the first limit reached."""

    class AlnsTrigger:
        """``config.alns_trigger``: When a stalled search is kicked by ruin & recreate."""

        enabled: bool
        unimproved_steps: int

    alns_trigger: AlnsTrigger
    """When a stalled search is kicked by ruin & recreate."""

    class Strategies:
        """``config.strategies``: A step's move, first: built-in strategies, each drawn by its own weight in a fixed order."""

        descent_weight: float
        """Probability that a step runs a best-improvement descent over a nearby entity pair
        instead of the move mix (one sweep ≈ 2 * teams evaluations). Default 0 = off."""
        ruin_recreate_weight: float
        """Weight of destroy-repair as an ordinary move judged by the acceptor
        (separate from the stagnation kick). Default 0 = off."""
        ruin_recreate_max_ruin: int

    strategies: Strategies
    """A step's move, first: built-in strategies, each drawn by its own weight in a fixed order."""

    class CustomMoves:
        """``config.custom_moves``: A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

        share: float
        """Upper bound on the probability of drawing the custom operator (TeamCountMove) in a step. 1 = unlimited."""
        team_count: bool
        """Macro moves on the number of loaded teams (empty / revive a team), with or without
        `constraints=`. The only path across the medium-level team-count barrier, so default True."""

    custom_moves: CustomMoves
    """A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

    class Moves:
        """``config.moves``: A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

        change_weight: float
        swap_weight: float

    moves: Moves
    """A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

    class Selection:
        """``config.selection``: What the move mix moves: nearby candidates and this use case's hints."""

        value_recommender: bool
        """Steers the value choice of change moves to hard-feasible teams (no match that day +
        not unavailable), with or without `constraints=`. Default False."""

    selection: Selection
    """What the move mix moves: nearby candidates and this use case's hints."""

    class Acceptance:
        """``config.acceptance``: Whether the candidate becomes the current solution."""

        late_acceptance_size: int
        """0 = automatic: clamp(8 * assignments, 400, 3200)."""

    acceptance: Acceptance
    """Whether the candidate becomes the current solution."""

    class Alns:
        """``config.alns``: The ruin & recreate kick itself."""

        min_ruin_fraction: float
        max_ruin_fraction: float
        max_ruin_count: int
        operator_decay: float
        rewards: list[float]
        polish_steps: int

    alns: Alns
    """The ruin & recreate kick itself."""

    class Run:
        """``config.run``: Seeds, parallel starts and debugging aids."""

        random_seed: int
        parallel_solver_count: int
        full_assert: bool
        on_new_best: Callable[[int, Score], None] | None

    run: Run
    """Seeds, parallel starts and debugging aids."""

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: TournamentSchedule) -> None:
        """Solves in place: writes team_assignments[*].team and schedule.score.
        Pinned assignments are kept."""

    def solve_with_constraints(self, schedule: TournamentSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.tournament_scheduling.constraints). The given list replaces the default set; None
        keeps the default set. A name may be picked several times: each pick is its own
        instance with its own knobs, its level included, and the instances add up. A pick
        on medium is folded into schedule.score.soft at MEDIUM_SCALE."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case:
    the two per-team constraints, then the two imbalance constraints).
    The same names optimaru.tournament_scheduling.constraints uses."""

def constraint_costs(
    schedule: TournamentSchedule, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/offset/level rounding.
    A unit is a team; the two imbalance (global) constraints report one cost.
    `optimaru.tournament_scheduling.constraints.score()` takes these and produces a score:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str,
    "medium_fold": {"kind": "into_soft", "unit": 1e6}}}. The scoped
    team_unavailability_cost also carries "units": the teams with an unavailability
    fact, the only ones it evaluates (a literal offset, clip=False, charges those and
    no other team). None = the default constraints; a pick list (as for solve) scores
    exactly those picks, each entry carrying its pick's knobs, the k-th pick of a name
    keyed "<name>#<k>" from the second on. The medium fold belongs to the domain, so
    EVERY entry carries it."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: the default level and
    weight of each constraint (team_conflict_cost and team_unavailability_cost hard,
    assignment_count_imbalance_cost medium in medium units,
    confrontation_count_imbalance_cost soft; every default weight 1). All flippable,
    none a reward, all enabled by default."""
