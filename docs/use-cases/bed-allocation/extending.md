# Bed allocation: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its twelve constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | one C++ test case, four counts in the tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the monolithic calculator |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `unusedRoomEquipmentCost`, the nights a stay spends in a room times the
equipment of that room the patient neither requires nor prefers. It keeps equipped rooms free for
the patients who need them. Every snippet below was compiled and run, and every figure measured, on
optimaru 0.4.3; a later change to the solver moves the figures, not the steps.

## How a plan is scored

A stay is assigned a bed. The score is kept per **unit**: one bed, or the "no bed"
value, with the stays assigned to it as its **members**. A move changes one or two
stays, so only the units they leave and join are scored again.

There are two scoring paths, and they give the same score for the same plan
(`ba_plugin_test` checks it on 500 random plans):

| | `ba.solve(plan)` | `ba.solve(plan, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/ba/constraints/` |
| Constraints | the twelve, at their defaults | the ones you list, with your knobs |
| Steps per second | 12.3 million | 1.6 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the demo instance (80 stays, 13 beds), 2 million steps, one thread
of an Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work on the
assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.bed_allocation as ba
from optimaru.bed_allocation.constraints import DepartmentSpecialtyCost, UnassignedStaysCost, get_constraints
from optimaru.bed_allocation.demo import generate_demo_data

plan = generate_demo_data()
changed = {"department_specialty_cost", "unassigned_stays_cost", "department_max_age_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    DepartmentSpecialtyCost(weight=40),                                # heavier (default 10)
    UnassignedStaysCost(level="hard"),                                 # medium by default
]                                                                      # department_max_age_cost: dropped
result = ba.solve(plan, time_limit_seconds=10, constraints=picks)
```

To try weights without solving again, score a finished plan with other picks:

```python
from optimaru.bed_allocation.constraints import score

raw = ba.constraint_costs(result.to_core(), picks)  # raw cost of every constraint, per bed
print(score(raw, picks))                            # the score those picks give this plan
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/bed_allocation/constraints.py).
Stop here if the constraint you need is one of the twelve.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

Ten of the twelve constraints are a sum of one term per stay, and
[`constraints/common.hpp`](../../../optimaru/bed_allocation/cpp/include/ba/constraints/common.hpp)
has a base for that. Pick the base by what the term reads:

| The term reads | Base | You write |
| --- | --- | --- |
| the stay and its room | `PerStayRoomConstraint` | `roomCost(plan, roomIndex, stayIndex)` |
| the stay and its bed, or the "no bed" value | `PerStayConstraint` | `stayCost(plan, unit, stayIndex)` |

The example reads the room, so it is one function. Four edits:

**1. The component**, `optimaru/bed_allocation/cpp/include/ba/constraints/unused_room_equipment_cost.hpp`:

```cpp
#ifndef BA_CONSTRAINTS_UNUSED_ROOM_EQUIPMENT_COST_HPP
#define BA_CONSTRAINTS_UNUSED_ROOM_EQUIPMENT_COST_HPP

#include <algorithm>

#include "ba/constraints/common.hpp"

namespace ba {

// Soft, off by default (weight 0): unusedRoomEquipmentCost — every night a
// stay spends in a room, weighed by the equipment of that room the patient
// neither requires nor prefers. Raw cost per bed = sum over its stays of
// nights * (unused items), so 0 = no equipped room is taken by a patient with
// no use for its equipment.
struct UnusedRoomEquipmentCost final : PerStayRoomConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UnusedRoomEquipmentCost(double weight = kDefaultWeight)
        : PerStayRoomConstraint("unusedRoomEquipmentCost", weight, kDefaultLevel) {}
    double roomCost(const BedPlan& plan, int roomIndex, int stayIndex) const override {
        const Stay& stay = plan.stays[stayIndex];
        const auto listed = [](const std::vector<int>& ids, int id) {
            return std::find(ids.begin(), ids.end(), id) != ids.end();
        };
        std::int64_t unused = 0;
        for (int id = 0; id < plan.equipmentTypeCount; ++id) {
            if (!plan.roomHasEquipment(roomIndex, id)) continue;
            if (!listed(stay.requiredEquipmentIds, id) && !listed(stay.preferredEquipmentIds, id))
                ++unused;
        }
        return static_cast<double>(stay.nightCount() * unused);
    }
};

}  // namespace ba

#endif  // BA_CONSTRAINTS_UNUSED_ROOM_EQUIPMENT_COST_HPP
```

The name passed to the base, `"unusedRoomEquipmentCost"`, is the struct name with a
lower-case first letter. Python derives `unused_room_equipment_cost` from it.

The defaults live in these two constants and nowhere else; Python reads them through
`ba.constraint_info()`. `kDefaultLevel` is `Hard`, `Medium` or `Soft`, and the weight is
in units of that level. A medium constraint states its weight in medium units
(`unassignedStaysCost`: 1): the domain folds the medium level into the engine's score
itself (`kMediumFold` in `domain.hpp`), so no weight carries the 10^9. A hard constraint
returns whole numbers, because a hard contribution is rounded per bed.

**2. The assembly**, `cpp/include/ba/constraints.hpp`: include the header, and add
one line at the end of `defaultConstraints()`:

```cpp
    assembly.emplace<UnusedRoomEquipmentCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/bed_allocation/constraints.py`:

```python
class UnusedRoomEquipmentCost(Constraint):
    """Nights in a room, valued by the room equipment the patient has no use for. Opt-in: soft, weight 0."""

    name = "unused_room_equipment_cost"
    label_en = "Unused room equipment"
    label_ko = "쓰지 않는 병실 장비"
    description_en = (
        "For each stay: its nights times the number of equipment items of its room that the patient "
        "neither requires nor prefers. 0 means no equipped room is taken by a patient with no use for it."
    )
    description_ko = (
        "입원마다 재원 밤 수에, 병실 장비 중 환자가 필요로 하지도 선호하지도 않는 장비 수를 곱해 더합니다. "
        "0이면 장비를 쓰지 않는 환자가 장비를 갖춘 병실을 차지한 경우가 없습니다."
    )
```

**4. Its default instance**, in the same file: `unused_room_equipment_cost =
UnusedRoomEquipmentCost()` with the other instances, that instance in `_DEFAULTS`
before `mixed_gender_room_cost` (the per-bed constraints come first, the one global
last), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=bed_allocation
uv sync                                    # rebuilds optimaru.bed_allocation._core
```

```python
from optimaru.bed_allocation.constraints import UnusedRoomEquipmentCost

others = [spec for spec in get_constraints() if spec.name != "unused_room_equipment_cost"]
picks = [*others, UnusedRoomEquipmentCost(weight=5)]
config = ba.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
result = ba.solve(plan, config=config, constraints=picks)
print(sum(ba.constraint_costs(result.to_core(), picks)["unused_room_equipment_cost"]["costs"]))
```

On the demo instance that sum is 135. A default solve of the same length
(`constraints=None`) leaves it at 148.

What the base gave you: `cost()` of a bed is the sum of `roomCost` over its stays,
and when a stay moves the engine calls `roomCost` once for the bed it leaves and
once for the bed it joins. The constraint is already incremental.

The engine asks every constraint in the picks, whatever its weight. `get_constraints()` now
returns thirteen constraints, the new one at weight 0, and solving with that list is slower
than with the twelve (1.42 against 1.60 million steps per second) for the same score.
Leave a constraint out of the picks, as `others` does above, instead of passing it at weight 0.

## 3. Check it

**A case worked out by hand**, in `tests/bed_allocation/cpp/ba_plugin_test.cpp`,
next to the other single-component cases in `verifyPartialAssemblies()`:

```cpp
    {  // unusedRoomEquipmentCost alone: six nights in a room with two items,
        // one of them preferred, so 6 nights * 1 unused item.
        BedPlan plan = makeBase();
        const int room = addRoom(plan, GenderLimitation::ANY_GENDER, 1, {"oxygen", "monitor"});
        const int stay = addStay(plan, 0, 5, Gender::MALE, addBed(plan, room));
        plan.stays[stay].patientPreferredEquipment = {"monitor"};
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<UnusedRoomEquipmentCost>(1.0);
        checkScore("unusedRoomEquipmentCost alone", scoreWith(calculator, plan), 0, 0, -6);
    }
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=bed_allocation && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The counts.** A thirteenth constraint fails four checks that count or list the catalog.
Update them:

- `ba_plugin_test.cpp`: `constraint_totals().size() == 12`, and `rows.size() == 12` in
  `verifyCatalog()`. Add the new row there, before `mixedGenderRoomCost`. `rowIs()`
  expects a constraint that is on by default, so check the new row's fields directly
  (`!rows[11].default_enabled`);
- `tests/bed_allocation/test_constraint_specs.py`:
  `test_list_constraints_matches_the_spec_names` (the list), and
  `test_constraint_info_reports_the_cpp_defaults` (`EXPECTED_INFO`, and its assertion
  that every row is on by default).

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) if a move leaves the
score stale:

```python
ba.solve(plan, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

`ba_plugin_test` does the same for the default assembly, which now holds the constraint:
3,000 random moves, incremental against full recompute.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `ba.constraint_info()`; add the row. The notebook
`examples/bed-allocation.ipynb` prints the constraint list.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint, one call for the bed a stay leaves
and one for the bed it joins. What that call is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a sum of per-stay terms | `PerStayConstraint` / `PerStayRoomConstraint` | one `stayCost` call | step 2 |
| a sum over pairs of stays in one bed | `BaUnitConstraint` with `cost()`, `supports_delta()` and `delta_cost()` | one pass over the bed's other stays | [`bed_conflict_cost.hpp`](../../../optimaru/bed_allocation/cpp/include/ba/constraints/bed_conflict_cost.hpp) |
| anything that spans beds (a room, a department, the whole plan) | `MemberChangeGlobalConstraint`: keep your own totals in `on_member_changed()` | what your update costs | [`mixed_gender_room_cost.hpp`](../../../optimaru/bed_allocation/cpp/include/ba/constraints/mixed_gender_room_cost.hpp) |

A unit constraint with `cost()` alone is correct: the engine then recomputes the
whole bed on every change, which for a constraint over pairs is quadratic in the stays of
the bed. `delta_cost()` returns what one stay adds given the others, and must equal
`cost(with the stay) - cost(without it)` exactly. `full_assert` (step 3) checks
that it does.

Three more things cut the cost of a constraint that is already incremental.

**Compute once per solve.** The facts never change during a solve, so a term that
reads only facts can be a table. `on_reset()` runs when a solve starts and when it
ends, not per move:

```cpp
    // One table per solve: roomCost() below is then an array read.
    void on_reset(const BedPlan& plan) override {
        PerStayRoomConstraint::on_reset(plan);
        stayCount_ = plan.stays.size();
        table_.assign(plan.rooms.size() * stayCount_, 0.0);
        for (std::size_t room = 0; room < plan.rooms.size(); ++room) {
            for (std::size_t stay = 0; stay < stayCount_; ++stay) {
                table_[room * stayCount_ + stay] =
                        unusedNights(plan, static_cast<int>(room), static_cast<int>(stay));
            }
        }
    }
    double roomCost(const BedPlan& /*plan*/, int roomIndex, int stayIndex) const override {
        return table_[static_cast<std::size_t>(roomIndex) * stayCount_ + stayIndex];
    }

private:
    static double unusedNights(const BedPlan& plan, int roomIndex, int stayIndex) {
        const Stay& stay = plan.stays[stayIndex];
        const auto listed = [](const std::vector<int>& ids, int id) {
            return std::find(ids.begin(), ids.end(), id) != ids.end();
        };
        std::int64_t unused = 0;
        for (int id = 0; id < plan.equipmentTypeCount; ++id) {
            if (!plan.roomHasEquipment(roomIndex, id)) continue;
            if (!listed(stay.requiredEquipmentIds, id) && !listed(stay.preferredEquipmentIds, id))
                ++unused;
        }
        return static_cast<double>(stay.nightCount() * unused);
    }

    std::size_t stayCount_ = 0;
    std::vector<double> table_;  // rooms x stays
```

With the constraint at weight 5 the assembly went from 1.41 to 1.53 million steps per
second, and the solve ended on the same score, as it must. That takes back most of
what the constraint cost: the twelve alone run at 1.60. A term that walks a list or a map
gains more. Under
`full_assert` the engine rebuilds on every step, so `on_reset()` runs every step
there; 20,000 checked steps still take 0.4 seconds on the demo instance.

**Say which units the constraint can charge.** `scoped_units()` returns the units the constraint
is ever non-zero for, and the engine skips it on the others.
`PerStayRoomConstraint` already returns the real beds, so the "no bed" value is
never asked.

**Keep raw costs whole numbers.** Every constraint here counts nights, so sums stay exact
and a hard total is never rounded. Return `static_cast<double>` of an integer count.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`ba.solve(plan)` must score it, and `ba.solve(plan)` scores with the monolithic
calculators: nearly 8 times the steps per second of the assembly, and the only path that
runs parallel starts.

`ba_plugin_test` compares the default assembly with the monolithic calculators on
random plans, so it fails from the moment the weight is not 0 until both sides
agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 2.0;`.
It must be a whole number, because the monolithic calculators add in `int64`.

**2. The term**, in `cpp/src/score_calculator.cpp`. Include the header, read the
weight off the component, add the level to the `static_assert`, and add the term
to `ScoreCalculator::singleStayImpact()`:

```cpp
constexpr std::int64_t kUnusedEquipmentWeight = integralWeight<UnusedRoomEquipmentCost>();
```

```cpp
    // soft 13: unusedRoomEquipmentCost — weigh by the room equipment the
    // patient neither requires nor prefers.
    const auto listed = [](const std::vector<int>& ids, int id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    std::int64_t unused = 0;
    for (int id = 0; id < plan.equipmentTypeCount; ++id) {
        if (!plan.roomHasEquipment(roomIndex, id)) continue;
        if (!listed(stay.requiredEquipmentIds, id) && !listed(stay.preferredEquipmentIds, id))
            ++unused;
    }
    impact.soft += kUnusedEquipmentWeight * nights * unused;
```

That is all for a per-stay constraint. `incremental_calculator.cpp` builds a table of
`singleStayImpact()` per stay and room when a solve starts and reads it on every
move, so the new term costs nothing per step: the default path ran at 12.2 million
steps per second with the thirteenth constraint in it, against 12.3 with twelve.

A constraint over pairs is more work. `bedConflictCost` and `mixedGenderRoomCost` are kept
from per-night occupancy counters in `incremental_calculator.cpp` (`insert()` and
`retract()`), and a new pair constraint needs its own counters there, and its sum in
`ScoreCalculator::score()`.

**3. What else moves.** A constraint that is on by default changes every default score:

- the catalog checks of step 3 again: the row's weight, and that it is on by default;
- the hand-computed scores in `tests/bed_allocation/cpp/ba_test.cpp` (with the
  example at weight 2, one case that expected a soft score of 0 gets -12) and any
  Python test that expects a score at the defaults;
- the tables and the "twelve" in [README.md](README.md), [python.md](python.md),
  `constraints.py`, `__init__.py`, `_core.pyi` and `common.hpp`;
- the CP-SAT model the benchmark compares against, `external_solver.bed_allocation`
  in [external-solver](https://github.com/optimaru/external-solver). Without the
  same constraint there, [`benchmarks/bed-allocation`](../../../benchmarks/bed-allocation)
  no longer compares one objective;
- the published results in `benchmarks/bed-allocation/README.md`, and `CHANGELOG.md`.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

import optimaru.bed_allocation as ba
from optimaru.bed_allocation.constraints import get_constraints
from optimaru.bed_allocation.demo import generate_demo_data

plan = generate_demo_data()
steps = 2_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    config = ba.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    result = ba.solve(plan, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  soft {result.score_soft}")
```

Both lines end on the same soft score: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

| Measured on the demo instance | Steps per second |
| --- | --- |
| default path, twelve constraints | 12.3 million |
| assembly, twelve constraints | 1.60 million |
| assembly, twelve + the constraint of step 2 at weight 5 | 1.41 million |
| assembly, twelve + the constraint of step 2 at weight 0 (`get_constraints()`) | 1.42 million |
| assembly, twelve + the constraint with the table of step 4 at weight 5 | 1.53 million |
| assembly, twelve + the constraint with the table of step 4 at weight 0 | 1.55 million |
| default path, thirteen constraints (step 5) | 12.2 million |
| assembly, thirteen constraints (step 5) | 1.52 million |

Each figure is the median of five runs, all taken in one session; a run moves by a few
percent, and other work on the machine lowers every row together.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after:

```sh
cmake --build build --target ba_quality_bench
uv run --project benchmarks/bed-allocation/cpsat \
    python benchmarks/bed-allocation/dump_instances.py /tmp/ba        # the comparison instances
./build/bed_allocation/ba_quality_bench /tmp/ba/<name>.batxt 10 1    # seconds, seed
```

`ba_quality_bench` runs the default path only, so it measures a constraint after step 5.
Build the tree before and after the change into two build directories and run the
two binaries in turn on the same instances and seeds. The comments on `Solver::Config` in
[`solver.hpp`](../../../optimaru/bed_allocation/cpp/include/ba/solver.hpp) show what
such a comparison reports. Against CP-SAT: `uv run bed-allocation-compare --seconds 10
--workers 8` in `benchmarks/bed-allocation/cpsat`
([README](../../../benchmarks/bed-allocation/README.md)).

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/ba/constraints/<subject>_cost.hpp` | new | | table, delta | weight |
| `cpp/include/ba/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/bed_allocation/cpp/ba_plugin_test.cpp` | | case, counts | | |
| `tests/bed_allocation/test_constraint_specs.py` | | lists | | |
| `docs/use-cases/bed-allocation/README.md`, `python.md` | | table row | | defaults |
| `cpp/src/score_calculator.cpp` | | | | the term |
| `tests/bed_allocation/cpp/ba_test.cpp` | | | | expected scores |
| external-solver, `benchmarks/bed-allocation` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.
