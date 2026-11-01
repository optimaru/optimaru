# bed_allocation — bed allocation (C++ port)

The `bed-allocation` example model (Apache-2.0; see the repository NOTICE)
(hospital bed allocation, PAS): the pybind11 package of the `../cpp` implementation ported
onto the generic C++ engine in `include/optimaru/`. A BASIC-variable domain that picks one bed (Bed) per stay (Stay).

## Install / test

```bash
cd use-cases/bed-allocation/python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

## Usage

```python
import bed_allocation as ba
from bed_allocation.demo import generate_demo_data

plan = generate_demo_data()          # pydantic BedPlanModel (1 department, 10 rooms, 13 beds, ~75 stays)
result = ba.solve(plan, time_limit_seconds=10.0, random_seed=42)
print(result.score_hard, result.score_medium, result.score_soft)
print([s.bed_id for s in result.stays][:5])
```

Passing a hand-built `_core.BedPlan` solves it in place. For JSON serialization use
`bed_allocation.models.BedPlanModel` (pydantic v2).

## Domain and planning variables

`Stay.bed` is the only planning variable (engine BASIC encoding):

| Value | Meaning |
|---|---|
| `-1` | uninitialized — no score contribution (input state) |
| `0..bed_count()-1` | real bed index |
| `bed_count()` (= `no_bed_value()`) | **"no bed" sentinel** — the quickstart's null bed |

The Java original's `@PlanningVariable(allowsUnassigned = true)` is reproduced by
adding one sentinel to the value range: the search can move a stay to "no bed", and
that stay is penalized per night at the medium level (`unassigned_stays_cost`). So hard 0
is always reachable (every hard violation can be resolved by giving up the assignment).

## Score mapping (HardMediumSoftScore -> engine HardSoftScore)

The original score type is `HardMediumSoftScore`; the engine only has `HardSoftScore(int64 hard,
double soft)`. The mapping rule (`cpp/include/ba/domain.hpp`):

```
engine.hard = hard
engine.soft = medium * 10^9 + soft      # every constraint is a penalty (<= 0) and an integer
```

While `|soft| < 10^9`, medium dominates soft lexicographically within a double (exact up
to 2^53) — the soft sum is bounded by about `228 x total stay-nights`, so it stays exact up
to millions of stay-nights. The Python `Score` exposes all three levels as-is.

The 10^9 is the domain's **medium fold** (`kMediumFold = MediumFold::intoSoft(10^9)`,
declared once in `domain.hpp`); it is not part of any weight. Every
`constraint_costs()` entry carries it as `"medium_fold": {"kind": "into_soft", "unit": 1e9}`,
and `bed_allocation.constraints.score()` applies it, so `score(raw)` equals
`(score.hard, score.medium * 10**9 + score.soft)`.

## Constraints (same semantics as Java `BedAllocationConstraintProvider`, 12 of them)

In `list_constraints()` order; `constraint_info()` reports the default level and weight of
each from the C++ components (`kDefaultLevel` / `kDefaultWeight`):

| Level | Constraint (Python name) | C++ component | Default weight | Penalty |
|---|---|---|---|---|
| hard | `bed_conflict_cost` | `BedConflictCost` | 1000 | overlapping nights in one bed x 1000 |
| hard | `female_in_male_room_cost` / `male_in_female_room_cost` | `FemaleInMaleRoomCost` / `MaleInFemaleRoomCost` | 50 | nights x 50 |
| hard | `department_min_age_cost` / `department_max_age_cost` | `DepartmentMinAgeCost` / `DepartmentMaxAgeCost` | 100 | nights x 100 |
| hard | `required_patient_equipment_cost` | `RequiredPatientEquipmentCost` | 50 | nights x (required equipment the room **has**) x 50 |
| medium | `unassigned_stays_cost` | `UnassignedStaysCost` | 1 (medium units) | nights of "no bed" stays x 1 |
| soft | `preferred_max_room_capacity_cost` | `PreferredMaxRoomCapacityCost` | 8 | nights x 8 |
| soft | `department_specialty_cost` | `DepartmentSpecialtyCost` | 10 | nights x 10 |
| soft | `department_specialty_priority_cost` | `DepartmentSpecialtyPriorityCost` | 10 | nights x (priority-1) x 10 |
| soft | `preferred_patient_equipment_cost` | `PreferredPatientEquipmentCost` | 50 | nights x (missing preferred equipment) x 50 |
| hard | `mixed_gender_room_cost` (global, last) | `MixedGenderRoomCost` | 1000 | overlapping mixed-gender nights in a SAME_GENDER room x 1000 |

Pick, reweight or flip them through `bed_allocation.constraints`
(`ba.solve(plan, constraints=[BedConflictCost(), UnassignedStaysCost(weight=3), ...])`).
Every spec takes `weight=None` (the C++ default, in units of its level), `level=None` (the
default level, otherwise a `Level` or its name), `exponent=1.0` and the keyword-only
`offset=0.0` / `clip=True` (`clip=True` counts only what lies beyond the offset,
`clip=False` shapes `x - offset` as it is). `unassigned_stays_cost` is the only medium
constraint: `level="hard"` charges one hard unit per night, `level="soft"` one soft unit per
night (outside the 10^9 fold), and an explicit weight stays in medium units. A constraint
may be picked several times (since 0.3.0); each pick is its own instance with its own
knobs, the instances add up, and `constraint_costs(plan, picks)` keys the k-th pick
`<name>#<k>`.

**0.2.0:** every constraint was renamed to the unified `*_cost` names (old -> new:
`same_bed_in_same_night` -> `bed_conflict_cost`, `female_in_male_room` ->
`female_in_male_room_cost`, `male_in_female_room` -> `male_in_female_room_cost`,
`different_gender_in_same_gender_room_in_same_night` -> `mixed_gender_room_cost`,
`department_minimum_age` -> `department_min_age_cost`, `department_maximum_age` ->
`department_max_age_cost`, `required_patient_equipment` -> `required_patient_equipment_cost`,
`assign_every_patient_to_a_bed` -> `unassigned_stays_cost`,
`preferred_maximum_room_capacity` -> `preferred_max_room_capacity_cost`,
`department_specialty` -> `department_specialty_cost`,
`department_specialty_not_first_priority` -> `department_specialty_priority_cost`,
`preferred_patient_equipment` -> `preferred_patient_equipment_cost`; spec classes and
module-level instances likewise). `unassigned_stays_cost` defaults to weight 1 in medium
units (before: a soft weight of 10^9) and `level="soft"` now demotes it to soft in the
engine as well. The package re-exports `ConstraintInfo` / `constraint_info()`. Raw costs
and the default score are unchanged.

Caveats (kept exactly as in the original):

- **`required_patient_equipment_cost` weighs by the required equipment the room HAS.**
  The filter fires when any required equipment is missing, and the match weight is the
  count present, not the count missing — a room with none costs 0. Counterintuitive, but
  the Java original's unit test (`penalizesBy(6)`) pins these semantics, so they were
  ported as-is.
- `department_specialty_priority_cost` applies only when the department offers that
  specialty. (In Java, `getSpecialtyPriority()` throws an NPE when it is not offered, so
  that path has no defined semantics — here it is skipped, and a missing specialty only
  gets the `department_specialty_cost` penalty.)
- Dates are integer day indices (`models` uses `datetime.date.toordinal()`).
  `night_count = departure - arrival + 1` (the original's `+ 1` is kept).

## Demo

`bed_allocation.demo.generate_demo_data()` reproduces the Java `DemoDataGenerator`:
1 department (3 specialties), 10 rooms, all SAME_GENDER (13 beds), 28 nights, ~75
sequential stays per room timeline, with the original's gender/age/equipment
distributions. (The java.util.Random stream itself is not identical — distributions and
scale are the reference.) The C++ demo (`build/bed-allocation/ba_demo`, built as shown in
../README.md) reaches hard 0 / medium 0 (everyone assigned) within 10 s.
