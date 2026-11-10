"""Type stubs for `optimaru.bed_allocation._core` — the bed-allocation C++ solver.

The compiled module wraps the C++ domain in `optimaru/bed_allocation/cpp`,
which imports the generic engine in `optimaru/cpp/optimaru`.

The only decision variable is `Stay.bed` (assignment-shape int)::

    -1                            uninitialized (no score contribution)
    0..bed_count()-1              real bed index
    bed_count() == no_bed_value() "no bed" sentinel — the stay is left
                                  unassigned; penalized per night at the
                                  medium level

Score convention (hard / medium / soft levels; all penalties),
at the default weights (the C++ components' kDefaultWeight, see constraint_info()):
  hard 1.  bed_conflict_cost                  overlapping nights in one bed x -1000
  hard 2.  female_in_male_room_cost           female in a MALE_ONLY room, nights x -50
  hard 3.  male_in_female_room_cost           male in a FEMALE_ONLY room, nights x -50
  hard 4.  mixed_gender_room_cost             mixed overlapping nights in a SAME_GENDER
                                              room x -1000 (global)
  hard 5.  department_min_age_cost            below department minimum age, nights x -100
  hard 6.  department_max_age_cost            above department maximum age, nights x -100
  hard 7.  required_patient_equipment_cost    room short of required equipment: nights x
                                              (required equipment the room HAS) x -50
                                              (so a room with none costs 0)
  medium 8. unassigned_stays_cost             nights of "no bed" stays x -1 (medium units)
  soft 9.  preferred_max_room_capacity_cost   room above preferred capacity, nights x -8
  soft 10. department_specialty_cost          specialty not offered by the department,
                                              nights x -10
  soft 11. department_specialty_priority_cost priority p > 1: nights x (p-1) x -10
  soft 12. preferred_patient_equipment_cost   nights x (missing preferred equipment) x -50

Internally the engine uses Score(int64 hard, double soft), mapped as
`engine.soft = medium * 10**9 + soft` (the domain's medium fold, "into_soft" with
unit 10**9; level priority holds while |soft| < 10**9; see
cpp/include/ba/domain.hpp for the exact rules). Medium weights are in medium units:
unassigned_stays_cost's default weight is 1, not 10**9.
"""

from collections.abc import Callable, Iterable
from enum import Enum
from typing import Any, Literal, NamedTuple

class Score:
    """Hard / medium / soft score. hard 0 == feasible. Lexicographic comparison (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def medium(self) -> int: ...
    @property
    def soft(self) -> int: ...
    def is_feasible(self) -> bool:
        """True if hard >= 0."""

class Gender(Enum):
    MALE = ...
    FEMALE = ...

class GenderLimitation(Enum):
    ANY_GENDER = ...
    MALE_ONLY = ...
    FEMALE_ONLY = ...
    SAME_GENDER = ...

class Department:
    """Problem fact: a department. minimum_age/maximum_age None == no limit."""
    def __init__(
        self,
        id: str,
        name: str,
        specialty_to_priority: dict[str, int] = ...,
        minimum_age: int | None = None,
        maximum_age: int | None = None,
    ) -> None: ...
    id: str
    name: str
    specialty_to_priority: dict[str, int]
    minimum_age: int | None
    maximum_age: int | None

class Room:
    """Problem fact: a room. `department` is an index into BedPlan.departments."""
    def __init__(
        self,
        id: str,
        name: str,
        department: int = 0,
        capacity: int = 1,
        gender_limitation: GenderLimitation = ...,
        equipment: list[str] = ...,
    ) -> None: ...
    id: str
    name: str
    department: int
    capacity: int
    gender_limitation: GenderLimitation
    equipment: list[str]

class Bed:
    """Problem fact (the value a stay is assigned): a bed. `room` is an index into BedPlan.rooms."""
    def __init__(self, id: str, room: int, index_in_room: int = 0) -> None: ...
    id: str
    room: int
    index_in_room: int

class Stay:
    """One patient stay. `bed` is the decision variable
    (see the encoding in the module docstring), written by the solver. Dates are integer
    day indices (only differences matter; night_count = departure - arrival + 1)."""
    def __init__(
        self,
        id: str,
        patient_name: str,
        patient_gender: Gender,
        patient_age: int,
        arrival_day: int,
        departure_day: int,
        specialty: str = "",
        patient_preferred_maximum_room_capacity: int | None = None,
        patient_required_equipment: list[str] = ...,
        patient_preferred_equipment: list[str] = ...,
        bed: int = -1,
    ) -> None: ...
    id: str
    patient_name: str
    patient_gender: Gender
    patient_age: int
    patient_preferred_maximum_room_capacity: int | None
    patient_required_equipment: list[str]
    patient_preferred_equipment: list[str]
    arrival_day: int
    departure_day: int
    specialty: str
    bed: int
    def night_count(self) -> int: ...
    def same_night_count(self, other: Stay) -> int: ...

class BedPlan:
    """The whole problem and its solution. solve changes the stays' `bed` and `score` in
    place.

    Note: list attributes such as `stays` return copies, so use `set_assignment`
    to change assignments."""
    def __init__(
        self,
        departments: list[Department],
        rooms: list[Room],
        beds: list[Bed],
        stays: list[Stay],
    ) -> None: ...
    @property
    def departments(self) -> list[Department]: ...
    @property
    def rooms(self) -> list[Room]: ...
    @property
    def beds(self) -> list[Bed]: ...
    @property
    def stays(self) -> list[Stay]: ...
    @property
    def score(self) -> Score: ...
    def bed_count(self) -> int: ...
    def no_bed_value(self) -> int:
        """The "no bed" sentinel value (== bed_count())."""
    def assignment(self) -> list[int]:
        """Current bed value per stay (the solution)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk-write the decision variable (`stays` returns copies, so writing
        individual elements has no effect)."""

def calculate_score(plan: BedPlan) -> Score:
    """Full recompute of the hard / medium / soft score (also stored in plan.score)."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = automatic: clamp(8 * stays, 8000, 12000)."""
    full_assert: bool
    """Full score checks: compares the incremental score to a full recompute after every move
    (for debugging; very slow)."""
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
    nearby_probability: float
    """Biases swap partners / related ruin toward stays with nearby date ranges.
    0 disables nearby selection."""
    nearby_size: int
    """Size of the nearby neighbor list."""
    best_score_limit: tuple[int, int, int]
    """Stop early once (hard, medium, soft) is reached. -(2**63) on medium or
    soft means no floor on the soft side (only hard is compared), so
    (0, -(2**63), -(2**63)) stops at the first feasible solution."""
    on_new_best: Callable[[int, Score], None] | None
    """Called with (step, score) on every new best solution."""

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, plan: BedPlan) -> None:
        """Solves in place: fills stays[*].bed and plan.score. Every stay ends
        with a real bed or the "no bed" sentinel value (never -1)."""

    def solve_with_constraints(self, plan: BedPlan, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.bed_allocation.constraints). The given list replaces the default set; None
        keeps the default set. A name may be picked several times: each pick is its own
        instance with its own knobs, its level included, and the instances add up.
        plan.score reports the three levels the assembly put the terms on
        (unassigned_stays_cost with level="soft" lands on soft, a soft constraint with
        level="medium" on medium), and so does every score passed to on_new_best."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case:
    the unit constraints, then the global mixed_gender_room_cost).
    The same names optimaru.bed_allocation.constraints uses."""

def constraint_costs(plan: BedPlan, constraints: Iterable[Any] | None = ...) -> dict[str, dict[str, Any]]:
    """Raw per-unit cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str,
    "medium_fold": {"kind": "into_soft", "unit": 1e9}}}. Units are the assignable values
    (beds in order, then the "no bed" sentinel); a global constraint has one entry. A unit
    constraint's "units" lists the units it evaluates, where a literal offset (clip=False)
    charges a raw 0: the real beds for the per-bed constraints (never the sentinel),
    [no_bed_value()] for unassigned_stays_cost. None = the default
    constraints; a pick list (as for solve) scores exactly those picks, each entry
    carrying its pick's knobs, the k-th pick of a name keyed "<name>#<k>" from the second
    on. `medium_fold` is on EVERY entry (the fold belongs to the domain), and
    `optimaru.bed_allocation.constraints.score()` applies it."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: each component's default
    level and weight (in units of that level; unassigned_stays_cost is medium, weight 1),
    whether its level may be flipped, whether it is a reward, and whether it is scored
    when no constraints are given."""
