# Bed allocation

Derived from the Timefold Quickstarts `bed-allocation` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../../NOTICE).

Domain model, constraints and API details: [`python.md`](python.md).

## Usage

```sh
OPTIMARU_USE_CASES=bed_allocation uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/bed_allocation
```

```python
from optimaru import bed_allocation as ba
from optimaru.bed_allocation.demo import generate_demo_data

plan = generate_demo_data()                       # models.BedPlanModel
result = ba.solve(plan, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_medium, result.score_soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`bed_allocation.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=bed_allocation && cmake --build build -j
ctest --test-dir build
```

## Constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost per bed (0 = no violation) |
|---|---|---|---|---|
| `bed_conflict_cost` | `BedConflictCost` | hard | 1000 | shared nights summed over every pair of stays in the bed |
| `female_in_male_room_cost` | `FemaleInMaleRoomCost` | hard | 50 | nights of its female stays when the room is `MALE_ONLY` |
| `male_in_female_room_cost` | `MaleInFemaleRoomCost` | hard | 50 | nights of its male stays when the room is `FEMALE_ONLY` |
| `department_min_age_cost` | `DepartmentMinAgeCost` | hard | 100 | nights of its stays younger than the department's `minimum_age` |
| `department_max_age_cost` | `DepartmentMaxAgeCost` | hard | 100 | nights of its stays older than the department's `maximum_age` |
| `required_patient_equipment_cost` | `RequiredPatientEquipmentCost` | hard | 50 | per stay whose room misses a required item: nights x required items the room **has** (quirk kept) |
| `unassigned_stays_cost` | `UnassignedStaysCost` | **medium** | 1 (medium units) | nights of the stays on the "no bed" sentinel (that unit only) |
| `preferred_max_room_capacity_cost` | `PreferredMaxRoomCapacityCost` | soft | 8 | nights of its stays whose preferred maximum is below the room capacity |
| `department_specialty_cost` | `DepartmentSpecialtyCost` | soft | 10 | nights of its stays whose specialty the department does not offer |
| `department_specialty_priority_cost` | `DepartmentSpecialtyPriorityCost` | soft | 10 | (priority - 1) x nights of its stays offered at priority > 1 |
| `preferred_patient_equipment_cost` | `PreferredPatientEquipmentCost` | soft | 50 | nights x preferred items the room lacks, over its stays |
| `mixed_gender_room_cost` | `MixedGenderRoomCost` (global) | hard | 1000 | one total: shared nights of every opposite-gender pair in one `SAME_GENDER` room |

This is the `list_constraints()` order (unit constraints first, the global last). Every
constraint is flippable onto any of the three levels (`level="hard"`, `"medium"` or
`"soft"`, or a `Level`), none is a reward and all twelve are scored by default;
`bed_allocation.constraint_info()` reports exactly this table from the C++ components
(`kDefaultLevel` / `kDefaultWeight`).

The medium level folds into the engine's soft level as `medium * 10^9 + soft`
(`kMediumFold` in `optimaru/bed_allocation/cpp/include/ba/domain.hpp`); weights are in units of their level, so
`unassigned_stays_cost` defaults to weight **1** medium unit per night.
`UnassignedStaysCost(level="hard")` makes it one hard unit per night,
`UnassignedStaysCost(level="soft")` one soft unit per night (outside the fold), and an
explicit weight is in units of the level in effect — in the engine, in
`constraints.score()` and in the CP-SAT reference alike. Any other constraint may move
onto medium the same way (`PreferredMaxRoomCapacityCost(level="medium")` charges one
medium unit per night above the preferred capacity, in both solvers too), and a repeated
pick carries its own level.

`unassigned_stays_cost` is a real medium constraint: `level="soft"` demotes it to soft and
`level="hard"` gives one hard unit per night; `solve(..., constraints=...)` reports the
levels the selection put the terms on.

**Offsets and repeated picks.** Every constraint takes the knobs `weight`, `level`,
`exponent` and, keyword-only, `offset` and `clip`, and may be picked several times. A raw cost `x` (per real bed for the per-bed constraints, on
the one total for the global `mixed_gender_room_cost`) is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which charges every unit the
constraint evaluates, a raw 0 included, and acts as a reward below the offset);
`offset=0` is the plain power shaping. The per-bed constraints evaluate the real beds, an empty
one included, and never the "no bed" sentinel (it is no bed); `unassigned_stays_cost`
evaluates the sentinel only. A `constraint_costs()` entry lists its evaluated units as
`"units"`, and the literal offset charges exactly those. Both apply per evaluated unit, not
to the total. `clip=False` with an offset is refused on hard (by default, every constraint
the table lists as hard), because hard 0 must mean no violation; medium folds into soft
here, so it takes the literal formula. Each pick is its own instance with its own knobs,
and the instances add up:

```python
ba.solve(plan, constraints=[
    BedConflictCost(),
    UnassignedStaysCost(),                         # 1 medium unit per unassigned night
    UnassignedStaysCost(exponent=2, offset=10),    # + (unassigned nights beyond 10)^2
    PreferredMaxRoomCapacityCost(),
])
```

`ba.constraint_costs(plan.to_core(), picks)` (it takes the engine `BedPlan`) keys the second
pick `unassigned_stays_cost#2`. The
CP-SAT reference takes integral offsets on every constraint (one term per evaluated unit,
one for the global total).

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is the model a solve returned or that model as a JSON dict:

```python
solved = ba.solve(plan, time_limit_seconds=5.0, random_seed=42)
seeded = ba.with_initial_solution(plan, solved.model_dump(mode="json"))
result = ba.solve(seeded, time_limit_seconds=5.0, random_seed=43)
print(result.score_hard, result.score_medium, result.score_soft)
```

Stays are matched by id. A stay the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. A stay with no bed has no bed id to carry, so it comes back unassigned and construction places it again. Unknown ids raise `ValueError` before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the
engine is `optimaru/bed_allocation/cpp/include/ba/solver.hpp` + `optimaru/bed_allocation/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/bed_allocation/cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`optimaru/bed_allocation/cpp/include/ba/constraints/` (currently `bed_conflict_cost`, `female_in_male_room_cost`,
`male_in_female_room_cost`, `mixed_gender_room_cost`, `department_min_age_cost`,
`department_max_age_cost`, `required_patient_equipment_cost`, `unassigned_stays_cost`,
`preferred_max_room_capacity_cost`, `department_specialty_cost`,
`department_specialty_priority_cost`, `preferred_patient_equipment_cost`, plus `common.hpp`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; the per-stay bases `PerStayConstraint` / `PerStayRoomConstraint` in
   `common.hpp` cover a cost that is a sum of per-(stay, bed) terms; positive raw cost =
   penalty, 0 = no violation) declaring `static constexpr double kDefaultWeight` and
   `static constexpr optimaru::Level kDefaultLevel`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter). A medium
   constraint declares `optimaru::Level::Medium` and a weight in medium units — the
   domain's `kMediumFold` (`domain.hpp`) does the 10^9 folding;
2. include it in `optimaru/bed_allocation/cpp/include/ba/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `ba_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `ba_test` / `ba_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `optimaru/bed_allocation/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, level="hard")])`;
5. mirror it in the CP-SAT model, `external_solver.bed_allocation` in
   [external-solver](https://github.com/optimaru/external-solver) (a plugin subclassing the
   spec; defaults come from `constraint_info()`), so the benchmark in
   [`benchmarks/bed-allocation/cpsat`](../../../benchmarks/bed-allocation/cpsat) stays like
   for like.

## Benchmark

[`benchmarks/bed-allocation`](../../../benchmarks/bed-allocation) compares this use case with OR-Tools CP-SAT on the same instances.
