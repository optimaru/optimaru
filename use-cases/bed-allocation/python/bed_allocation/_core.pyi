"""Type stubs for `bed_allocation._core` — the bed-allocation C++ solver.

The compiled module wraps the C++ domain in `use-cases/bed-allocation/cpp`,
which imports the generic engine in `include/optimaru`.

The only planning variable is `Stay.bed` (BASIC int)::

    -1                            uninitialized (no score contribution)
    0..bed_count()-1              real bed index
    bed_count() == no_bed_value() "no bed" sentinel — the quickstart's
                                  null bed (allowsUnassigned = true);
                                  penalized per night at the medium level

Score convention (HardMediumSoftScore, as in the Java quickstart; all penalties):
  hard 1.  sameBedInSameNight             overlapping nights in one bed x -1000
  hard 2.  femaleInMaleRoom               female in a MALE_ONLY room, nights x -50
  hard 3.  maleInFemaleRoom               male in a FEMALE_ONLY room, nights x -50
  hard 4.  differentGenderInSameGenderRoomInSameNight
                                          mixed overlapping nights in a SAME_GENDER
                                          room x -1000
  hard 5.  departmentMinimumAge           below department minimum age, nights x -100
  hard 6.  departmentMaximumAge           above department maximum age, nights x -100
  hard 7.  requiredPatientEquipment       room short of required equipment: nights x
                                          (required equipment the room HAS) x -50
                                          (the semantics pinned by the Java
                                          original's unit test)
  medium 8. assignEveryPatientToABed      nights of "no bed" stays x -1
  soft 9.  preferredMaximumRoomCapacity   room above preferred capacity, nights x -8
  soft 10. departmentSpecialty            specialty not offered by the department, nights x -10
  soft 11. departmentSpecialtyNotFirstPriority
                                          priority p > 1: nights x (p-1)
                                          x -10
  soft 12. preferredPatientEquipment      nights x (missing preferred equipment) x -50

Internally the engine uses HardSoftScore(int64 hard, double soft), mapped as
`engine.soft = medium * 10**9 + soft` (level priority holds while |soft| < 10**9;
see cpp/include/ba/domain.hpp for the exact rules).
"""

from collections.abc import Callable, Iterable
from enum import Enum
from typing import Any

class Score:
    """HardMediumSoft score. hard 0 == feasible. Lexicographic comparison (hard first)."""
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
    """Problem fact (planning value): a bed. `room` is an index into BedPlan.rooms."""
    def __init__(self, id: str, room: int, index_in_room: int = 0) -> None: ...
    id: str
    room: int
    index_in_room: int

class Stay:
    """Planning entity: one patient stay. `bed` is the planning variable
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
    """Planning solution. solve changes the stays' `bed` and `score` in
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
        """ The "no bed" sentinel planning value (== bed_count())."""
    def assignment(self) -> list[int]:
        """Current bed value per stay (the solution)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk-write the planning variable (`stays` returns copies, so writing
        individual elements has no effect)."""

def calculate_score(plan: BedPlan) -> Score:
    """Full recompute of the HardMediumSoft score (also stored in plan.score)."""

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
    """FULL_ASSERT: compares the incremental score to a full recompute after every move
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
    """Stop early once (hard, medium, soft) is reached. E.g. (0, -(2**62),
    -(2**62)) stops at the first feasible solution."""
    on_new_best: Callable[[int, Score], None] | None
    """Called with (step, score) on every new best solution."""

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, plan: BedPlan) -> None:
        """Solves in place: fills stays[*].bed and plan.score. Every stay ends
        with a real bed or the "no bed" sentinel value (never -1)."""

    def solve_with_constraints(self, plan: BedPlan, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see bed_allocation.constraints). The given list replaces the default set; None
        keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case).
    The same names bed_allocation.constraints uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/level rounding.
    `bed_allocation.constraints.score()` takes these and produces a score.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
