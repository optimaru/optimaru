# bed_allocation — bed allocation (Timefold quickstart port)

Timefold quickstart [bed-allocation](https://github.com/TimefoldAI/timefold-quickstarts)
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
that stay is penalized per night at the medium level. So hard 0 is always reachable
(every hard violation can be resolved by giving up the assignment).

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

## Constraints (same semantics as Java `BedAllocationConstraintProvider`, 12 of them)

| Level | Constraint | Penalty |
|---|---|---|
| hard | sameBedInSameNight | overlapping nights x 1000 |
| hard | femaleInMaleRoom / maleInFemaleRoom | nights x 50 |
| hard | differentGenderInSameGenderRoomInSameNight | overlapping nights x 1000 |
| hard | departmentMinimumAge / departmentMaximumAge | nights x 100 |
| hard | requiredPatientEquipment | nights x (required equipment the room **has**) x 50 |
| medium | assignEveryPatientToABed | nights of "no bed" stays x 1 |
| soft | preferredMaximumRoomCapacity | nights x 8 |
| soft | departmentSpecialty | nights x 10 |
| soft | departmentSpecialtyNotFirstPriority | nights x (priority-1) x 10 |
| soft | preferredPatientEquipment | nights x (missing preferred equipment) x 50 |

Caveats (kept exactly as in the original):

- **requiredPatientEquipment weighs by the required equipment the room HAS.**
  The filter fires when any required equipment is missing, and the match weight is the
  count present, not the count missing — a room with none costs 0. Counterintuitive, but
  the Java original's unit test (`penalizesBy(6)`) pins these semantics, so they were
  ported as-is.
- departmentSpecialtyNotFirstPriority applies only when the department offers that
  specialty. (In Java, `getSpecialtyPriority()` throws an NPE when it is not offered, so
  that path has no defined semantics — here it is skipped, and a missing specialty only
  gets the departmentSpecialty penalty.)
- Dates are integer day indices (`models` uses `datetime.date.toordinal()`).
  `night_count = departure - arrival + 1` (the original's `+ 1` is kept).

## Demo

`bed_allocation.demo.generate_demo_data()` reproduces the Java `DemoDataGenerator`:
1 department (3 specialties), 10 rooms, all SAME_GENDER (13 beds), 28 nights, ~75
sequential stays per room timeline, with the original's gender/age/equipment
distributions. (The java.util.Random stream itself is not identical — distributions and
scale are the reference.) The C++ demo (`cpp/build/ba_demo`) reaches hard 0 / medium 0
(everyone assigned) within 10 s.
