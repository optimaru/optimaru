"""Type stubs for `sports_league._core` — the sports-league-scheduling C++ solver.

The compiled module wraps the C++ domain in `use-cases/sports-league-scheduling/cpp`,
which imports the generic engine in `include/optimaru`.

Domain: assign a round (Round, planning value) to every match of the league (Match,
planning entity). `Match.round` is the planning variable and holds a position in the
`LeagueSchedule.rounds` list (-1 == unassigned). Round adjacency (same day / next day /
first / last) is all based on `Round.index`.

Score convention: `hard` is the number of constraint violations (0 == feasible), `soft`
is minus the sum of travel distance (km) and classic penalties. Both are maximized.

Constraints (HardSoftScore, same semantics as the Java quickstart), at the default
weights (the C++ components' defaults, reported by constraint_info()):
  hard 1. team_conflict_cost            -1 per pair of matches in the same round index
                                        sharing a team (once per pair even if both teams match)
  hard 2. consecutive_home_matches_cost when a team's home matches cover 4+ distinct consecutive
                                        round indices, -n for the run length
                                        (duplicate matches on one index count as 1)
  hard 3. consecutive_away_matches_cost the same for away matches
  hard 4. repeat_match_on_next_day_cost -1 per match whose return fixture is in the next round
                                        index (ifExists: at most once per match)
  soft 5. start_to_away_hop_cost        per match with no previous round index,
                                        -dist(away, home)
  soft 6. home_to_away_hop_cost         for consecutive-index pairs (m1, m2) with m2.away ==
                                        m1.home, -dist(m1.home, m2.home)
  soft 7. away_to_away_hop_cost         if m2.away == m1.away,
                                        -dist(m1.home, m2.home)
  soft 8. away_to_home_hop_cost         if m2.home == m1.away,
                                        -dist(m1.home, m1.away)
  soft 9. away_to_end_hop_cost          per match with no next round index,
                                        -dist(home, away)
  soft 10. workday_classic_matches_cost per classic match in a non-weekend/holiday round
                                        -1000 (weight 1000)
"""

from collections.abc import Callable, Iterable
from typing import Any, Literal, NamedTuple

class Score:
    """HardSoft score. `hard` is the integer violation sum (0 == feasible); `soft`
    is the objective. Lexicographic comparison (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True if hard == 0."""

class Team:
    """Problem fact: a team. Matches reference teams by position in the team list."""
    def __init__(self, id: str, name: str = "") -> None: ...
    id: str
    name: str

class Round:
    """Problem fact and planning value. `index` is the round number the constraints use
    (>= 0; usually consecutive 0..N-1, but sparse is fine)."""
    def __init__(self, index: int, weekend_or_holiday: bool = False) -> None: ...
    index: int
    weekend_or_holiday: bool

class Match:
    """Planning entity: one match to get a round. `round` is the planning
    variable (-1 == unassigned; the value is a position in the rounds list), written by the solver."""
    def __init__(
        self,
        id: str,
        home_team: int,
        away_team: int,
        classic_match: bool = False,
        round: int = -1,
    ) -> None: ...
    id: str
    home_team: int
    away_team: int
    classic_match: bool
    round: int

class LeagueSchedule:
    """Planning solution: teams/rounds (value range) + matches (entities) + the distance
    matrix. solve changes the matches' `round` and `score` in place.

    Note: list attributes such as `matches` return copies, so to change assignments use
    `set_assignment` or write them on the Matches before building the
    LeagueSchedule."""
    def __init__(
        self,
        teams: list[Team],
        rounds: list[Round],
        matches: list[Match],
        distance_to_team: list[list[int]] = ...,
    ) -> None: ...
    teams: list[Team]
    rounds: list[Round]
    matches: list[Match]
    distance_to_team: list[list[int]]
    """[from][to] km distance matrix (direction preserved). Empty list = all 0."""
    @property
    def score(self) -> Score: ...
    def team_count(self) -> int: ...
    def round_count(self) -> int: ...
    def match_count(self) -> int: ...
    def assignment(self) -> list[int]:
        """Current round value per match (the solution vector)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk-write the round per match. ValueError if the length differs from the match count."""

def calculate_score(schedule: LeagueSchedule) -> Score:
    """Full recompute of all 10 constraints (also stored in `schedule.score`)."""

class SolverConfig:
    """Termination conditions / seed / search options of `Solver`."""
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """Wall-clock budget of solve() (default 30 s)."""
    unimproved_spent_limit_seconds: float
    """Time limit since the last new best; 0 disables."""
    step_count_limit: int
    """Local-search step budget; 0 disables."""
    unimproved_step_count_limit: int
    """Step limit since the last new best; 0 disables."""
    random_seed: int
    parallel_solver_count: int
    """Number of independent multistarts (seed+i); the best wins."""
    late_acceptance_size: int
    """LA history length. 0 = automatic (clamp(32 * matches, 400, 3200))."""
    full_assert: bool
    """Re-checks incremental == full recompute on every move (for debugging, slow)."""
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
    nearby_size: int
    """Number of nearby neighbors — matches sharing a team, ordered by home ground distance.
    0 = automatic (4 * teams), negative disables nearby selection."""
    nearby_probability: float
    """Probability of drawing the swap partner from the nearby neighbors."""
    costliest_match_sample: int
    """Random sample size when picking a match to move — the one with the largest current
    travel penalty among them is chosen (tournament selection). 1 means uniform random.
    While infeasible it automatically falls back to uniform random."""
    best_score_limit: tuple[int, float]
    """Stop early once bestScore >= (hard, soft). (0, -inf) stops at the first
    feasible."""
    on_new_best: Callable[[int, Score], None] | None
    """Called on every new best: on_new_best(step, score)."""

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: LeagueSchedule) -> None:
        """Solves in place: writes matches[*].round and schedule.score."""

    def solve_with_constraints(self, schedule: LeagueSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see sports_league.constraints). The given list replaces the default set; None
        keeps the default set. A name may be picked several times: each pick is its own
        instance with its own knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case: the
    three unit constraints first, then the globals): start_to_away_hop_cost,
    away_to_end_hop_cost, workday_classic_matches_cost, team_conflict_cost,
    consecutive_home_matches_cost, consecutive_away_matches_cost,
    repeat_match_on_next_day_cost, home_to_away_hop_cost, away_to_away_hop_cost,
    away_to_home_hop_cost. The same names sports_league.constraints uses."""

def constraint_costs(
    schedule: LeagueSchedule, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Per-constraint **raw** costs — before weight/exponent/offset/level rounding:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "hard": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str}}. The three unit constraints
    report one cost per round position, an empty round included, the seven globals
    a single total. None = the default constraints; a pick list (as for solve)
    scores exactly those picks, each entry carrying its pick's knobs, the k-th pick
    of a name keyed "<name>#<k>" from the second on.
    `sports_league.constraints.score()` takes these and produces a score. This is
    the only per-constraint surface (the 0.1.x Breakdown / compute_breakdown are
    gone)."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: start_to_away_hop_cost,
    away_to_end_hop_cost (soft, 1), workday_classic_matches_cost (soft, 1000),
    team_conflict_cost, consecutive_home_matches_cost, consecutive_away_matches_cost,
    repeat_match_on_next_day_cost (hard, 1), home_to_away_hop_cost,
    away_to_away_hop_cost, away_to_home_hop_cost (soft, 1); all flippable, none a
    reward, all scored by default (default_enabled)."""
