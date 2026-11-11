# Quadratic assignment: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its five constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | one C++ test case, the catalog in the tests, the Python reference scorer |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header, two checks |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `unusedLocationCapacityCost`, the capacity a location has beyond the
demand of the facility standing there. It is the other side of `locationCapacityCost`, which charges
the demand over the capacity, and it keeps the large locations for the facilities that need them.
Every snippet below was compiled and run, and every figure measured, on optimaru 0.4.3; a later
change to the solver moves the figures, not the steps.

## How an assignment is scored

A facility stands at a location. The score is kept per **unit**: one location, with
the facilities standing there as its **members**. That is one facility once the
assignment is complete, and two for a moment in the middle of a swap. The search
only swaps two facilities, so a move touches two locations. Three of the five constraints
are a sum of one term per location. The two objectives, the direct cost and flow
times distance, are one number for the whole assignment: they are **global**
components that keep their own totals.

There are two scoring paths, and they give the same score for the same assignment
(`qap_plugin_test` checks it on 500 random assignments):

| | `qap.solve(problem)` | `qap.solve(problem, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/qap/constraints/` |
| Constraints | the five, at their defaults | the ones you list, with your knobs |
| Steps per second | 3.7 million | 2.9 million |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the instance of step 1 (40 facilities from the generator of the
benchmark, with demands and capacities added), 5 million steps, one thread of an Apple
M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work on the assembly. Step 5
moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults.
The generated instances of the benchmark have no demands and no capacities, so this one
adds them:

```python
import random

from optimaru import quadratic_assignment as qap
from optimaru.quadratic_assignment.constraints import FlowDistanceCost, ForbiddenAssignmentsCost, get_constraints
from optimaru.quadratic_assignment.models import QuadraticAssignmentProblemModel

size = 40
rng = random.Random(5)  # the generator of the benchmark, at 40 facilities
facilities = [f"F{i}" for i in range(size)]
locations = [f"L{i}" for i in range(size)]
flows = [[0] * size for _ in range(size)]
distances = [[0] * size for _ in range(size)]
for a in range(size):
    for b in range(a + 1, size):
        flows[a][b] = flows[b][a] = rng.randint(0, 9)
        distances[a][b] = distances[b][a] = rng.randint(1, 9)
problem = QuadraticAssignmentProblemModel(
    facilities=facilities,
    locations=locations,
    flows=flows,
    distances=distances,
    fixed_assignments={locations[0]: facilities[size - 1]},
    forbidden_assignments=[{"facility": facilities[0], "location": locations[1]}],
    facility_demands={name: rng.randint(1, 9) for name in facilities},  # not in the benchmark
    location_capacities={name: 9 for name in locations[::2]},  # not in the benchmark
)

changed = {"flow_distance_cost", "forbidden_assignments_cost", "assignment_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    FlowDistanceCost(weight=2),                                        # heavier (default 1)
    ForbiddenAssignmentsCost(level="soft", weight=500),                # hard by default
]                                                                      # assignment_cost: dropped
result = qap.solve(problem, time_limit_seconds=10, constraints=picks)
```

To try weights without solving again, score a finished assignment with other picks:

```python
from optimaru.quadratic_assignment.constraints import score

raw = qap.constraint_costs(result, picks)  # raw cost of every constraint, per location or as one total
print(score(raw, picks))                   # the score those picks give this assignment
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/quadratic_assignment/constraints.py).
Stop here if the constraint you need is one of the five.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/quadratic_assignment/cpp/include/qap/constraints/common.hpp)
names two bases. Pick the base by what the cost is:

| The cost is | Base | You write |
| --- | --- | --- |
| a sum of one term per location | `QapUnitConstraint` | `cost(problem, location, members)` |
| one number for the whole assignment | `QapMemberChangeGlobalConstraint` | `reset()`, `on_member_changed()` and `cost()` |

The example reads one location and the facility on it, so it is one function. Four
edits:

**1. The component**,
`optimaru/quadratic_assignment/cpp/include/qap/constraints/unused_location_capacity_cost.hpp`:

```cpp
#ifndef QAP_CONSTRAINTS_UNUSED_LOCATION_CAPACITY_COST_HPP
#define QAP_CONSTRAINTS_UNUSED_LOCATION_CAPACITY_COST_HPP

#include "qap/constraints/common.hpp"

namespace qap {

// Soft, off by default (weight 0): unusedLocationCapacityCost — capacity a
// location has beyond the demand standing there. Raw cost per location =
// max(0, capacity - summed demand of its facilities) for a location with a
// capacity, 0 for one without; a facility missing from facility_demands has
// demand 0, so it leaves the whole capacity unused. 0 = every capacitated
// location is filled (a demand above the capacity costs 0 here: that is
// locationCapacityCost's).
struct UnusedLocationCapacityCost final : QapUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit UnusedLocationCapacityCost(double weight = kDefaultWeight)
        : QapUnitConstraint("unusedLocationCapacityCost", weight, kDefaultLevel) {}

    void on_reset(const QuadraticAssignmentProblem& problem) override {
        detail::requirePrepared(problem);
    }

    double cost(const QuadraticAssignmentProblem& problem, int unit,
                const std::vector<int>& members) const override {
        if (!problem.hasCapacity[unit]) return 0.0;
        std::int64_t demand = 0;
        for (int facility : members) demand += problem.demandOf[facility];
        const std::int64_t unused = problem.capacityOf[unit] - demand;
        return unused > 0 ? static_cast<double>(unused) : 0.0;
    }
};

}  // namespace qap

#endif  // QAP_CONSTRAINTS_UNUSED_LOCATION_CAPACITY_COST_HPP
```

The name passed to the base, `"unusedLocationCapacityCost"`, is the struct name with
a lower-case first letter. Python derives `unused_location_capacity_cost` from it.
`on_reset()` checks that `ScoreCalculator::prepare()` ran: `hasCapacity`, `capacityOf`
and `demandOf` are the flat caches it builds, and every solve runs it first. `cost()`
loops over the members because it must be right for any state a caller writes: a
location left empty, or two facilities on one. The solver asks it when a swap is
complete, so there it sees one facility.

The defaults live in these two constants and nowhere else; Python reads them through
`qap.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this domain has no
medium level, and `level="medium"` raises `ValueError`. A hard constraint returns whole
numbers, because a hard contribution is rounded per location. The flows, distances
and direct costs are not checked to be 0 or more. A constraint that reads them and can
come out negative on such data overrides `raw_cost_can_be_negative()`, as
`AssignmentCost` does, and the engine then refuses it on the hard level.

**2. The assembly**, `cpp/include/qap/constraints.hpp`: include the header, and add
one line to `defaultConstraints()`, after `LocationCapacityCost`. The catalog lists
the unit constraints first and the globals after them, and the docs write
`get_constraints()[:-1]` for "all but the flow objective":

```cpp
    assembly.emplace<UnusedLocationCapacityCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/quadratic_assignment/constraints.py`:

```python
class UnusedLocationCapacityCost(Constraint):
    """Location capacity beyond the demand standing there. Opt-in: soft, weight 0."""

    name = "unused_location_capacity_cost"
    label_en = "Unused location capacity"
    label_ko = "남는 위치 용량"
    description_en = (
        "Capacity of each location beyond the demand of the facility standing there, summed over the locations "
        "with a capacity; a facility without a facility_demands entry has demand 0. One unit of cost is one "
        "unit of capacity left unused; 0 means every location with a capacity is filled. Its default weight is "
        "0, so it only counts once you give it a weight."
    )
    description_ko = (
        "용량이 정해진 위치마다 그 위치의 용량이 놓인 시설의 수요보다 큰 만큼을 모두 더한 값이며, "
        "facility_demands에 없는 시설의 수요는 0으로 봅니다. 남는 용량 1단위가 비용 1이며, 0이면 용량이 정해진 "
        "모든 위치가 가득 찬 것입니다. 기본 가중치가 0이므로 가중치를 주어야 점수에 반영됩니다."
    )
```

**4. Its default instance**, in the same file: `unused_location_capacity_cost =
UnusedLocationCapacityCost()` with the other instances, that instance in `_DEFAULTS`
after `location_capacity_cost`, and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=quadratic_assignment
uv sync                                    # rebuilds optimaru.quadratic_assignment._core
```

```python
from optimaru.quadratic_assignment.constraints import UnusedLocationCapacityCost

others = [spec for spec in get_constraints() if spec.name != "unused_location_capacity_cost"]
picks = [*others, UnusedLocationCapacityCost(weight=20)]
config = qap.SolverConfig()
config.step_count_limit = 2_000_000
config.random_seed = 1
result = qap.solve(problem, config=config, constraints=picks)
raw = qap.constraint_costs(result, picks)
print(sum(raw["unused_location_capacity_cost"]["costs"]), raw["flow_distance_cost"]["costs"])
```

On the instance above that prints `36.0 [29484.0]`. A default solve of the same
length (`constraints=None`) leaves 95 of capacity unused at a flow cost of 28,968.
The lowest the instance allows is 28: its pin keeps F39, a demand of 2, on L0.

What the base gave you: after a swap the engine calls `cost()` once for each of the
two locations it touched, and keeps the results. A refused swap puts the old results
back without another call. Each call is one facility and one comparison, so the
constraint is cheap from the start.

## 3. Check it

**A case worked out by hand**, in `tests/quadratic_assignment/cpp/qap_plugin_test.cpp`,
with the other components' cases in `verifyRawCostsByHand()`: a positive cost and
the score it gives at a weight, then a location over its capacity and an instance
without capacities.

```cpp
        // unusedLocationCapacityCost: what each capacity has beyond the demand
        // standing there. It is opt-in, so it scores at a weight.
        QuadraticAssignmentProblem p = sideInstance();
        place(p, {1, 0});  // L1 (capacity 3) holds F2 (2), L2 (10) holds F1 (5)
        check(rawCosts(p, "unusedLocationCapacityCost") == std::vector<double>{1.0, 5.0},
              "unused capacity: positive [1, 5]");
        check(sameScore(scoreWith(defaultConstraints().weight("unusedLocationCapacityCost", 2), p),
                        Score{0, -26.0 - 12.0}),
              "unused capacity at weight 2: 2 x 6 soft on top of 3 + 23");
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=quadratic_assignment && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The catalog.** A sixth constraint fails six checks of `qap_plugin_test` and thirteen
Python tests, and they list more than a count. Update them:

- `qap_plugin_test.cpp`, the `expected` totals of `verifyAssemblyParity()`. The test
  compares the total of every component of the default assembly with the monolith's
  `RawCosts` on 500 random assignments. The monolith has no total for an opt-in
  constraint, so compute one in the test (for every location with a capacity, the capacity
  minus the demand standing there, floored at 0) at the constraint's place. The walk of
  `verifyIncremental()` reads the names off `defaultConstraints()`, so it covers the
  new constraint as it is: after every move, its costs against a fresh recompute.
- `qap_plugin_test.cpp`, the layout in `verifyTotalsAndLayout()` (six entries, the
  globals from the fifth), the two name lists of `verifyNames()`, `rows.size() == 5`
  and the row in `verifyCatalog()`, and the scoped entries in
  `verifyRepeatsAndOffsets()`, which are read by index. `rowIs()` expects a constraint that
  is on by default; make it compare `row.default_enabled` with `weight > 0.0`.
- `tests/quadratic_assignment/test_constraint_specs.py`, seven tests: `NAMES`, the
  instances and the `constraint_info()` rows list the catalog,
  `test_constraint_costs_match_the_hand_calculation` and
  `test_the_scoped_side_constraints_report_their_units` list every entry of
  `constraint_costs()`, and `test_weight_none_means_the_component_default` gave every
  constraint the weight 1 (read the default weights from `constraint_info()` there).
- `tests/quadratic_assignment/reference_quadratic_assignment.py`, the independent
  scorer in plain Python that six tests of `test_reference_parity.py` compare the
  engine with, per constraint. Add the constraint's name to `REFERENCE_NAMES` and
  `ENGINE_NAME`, its default to `REFERENCE_DEFAULTS`, and its terms to
  `constraint_costs()`:

  ```python
          "unused_location_capacities": [
              max(0, capacity - facility_demands.get(assignment[location], 0))
              for location, capacity in location_capacities.items()
          ],
  ```

- `tests/quadratic_assignment/test_reference_parity.py`, its own list of the spec
  classes, `SPECS`. No test fails without the new class there, but
  `test_every_constraint_shapes_like_the_oracle` then leaves the constraint out.

Then add cases that pick the constraint, at a weight and on the hard level
(`test_levels_by_hand_on_both_paths` in `test_constraint_specs.py`).

**Incremental against full.** Solve with every score checked against a full
recompute. It raises `RuntimeError` ("Score corruption: incremental score ... != full
recompute ...") if a move leaves the score stale:

```python
qap.solve(problem, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

Give the constraint a weight for this: at weight 0 a stale cost scores the same.

**The tables and the CP-SAT model.** The constraint table in [README.md](README.md)
must equal `qap.constraint_info()`; add the row, and the header to the component files
its C++ section lists. [python.md](python.md),
`__init__.py` and `_core.pyi` describe the score in words, and the notebook
`examples/quadratic-assignment.ipynb` prints the constraint list. The benchmark
compares against `external_solver.quadratic_assignment` in
[external-solver](https://github.com/optimaru/external-solver): one plugin per
constraint, a subclass of the Python spec with its defaults from `constraint_info()`.
Its `tests/quadratic_assignment/test_constraint_parity.py` runs over
`list_constraints()` and pins permutations to compare each raw cost, so it fails for
a constraint without a plugin, opt-in or not. Mirror the constraint there with an exact
encoding.

## 4. Make it cheaper per move

On the assembly a swap costs, for every constraint in the picks, the two locations the
facilities leave and join. What that costs depends on the constraint:

| The constraint is | Use | Per swap | Example to copy |
| --- | --- | --- | --- |
| a sum of one term per facility at the location | `QapUnitConstraint` with `cost()`, `supports_delta()` and `delta_cost()` | one lookup per join or leave | [`forbidden_assignments_cost.hpp`](../../../optimaru/quadratic_assignment/cpp/include/qap/constraints/forbidden_assignments_cost.hpp) |
| a clamp, or another function of the location that does not add up per facility | `QapUnitConstraint` with `cost()` alone | the location's facilities | step 2 |
| a sum of one number per facility over the whole assignment | `QapMemberChangeGlobalConstraint` with one `int64` total | one addition per join or leave | [`assignment_cost.hpp`](../../../optimaru/quadratic_assignment/cpp/include/qap/constraints/assignment_cost.hpp) |
| a sum over pairs of facilities | the same, with its own copy of the locations | every facility, four times | [`flow_distance_cost.hpp`](../../../optimaru/quadratic_assignment/cpp/include/qap/constraints/flow_distance_cost.hpp) |

The example is the second row, and it stays without `delta_cost()`. A delta must equal
`cost(with the facility) - cost(without it)` exactly. For a clamp that depends on the
other facilities of the location, so the delta would add them up as `cost()` does and
save nothing. `cost()` is one facility and one comparison already. The time of a swap
goes to the last row: flow times distance walks all 40 facilities for each of the
four joins and leaves.

What is left is to say which locations the constraint can charge. `scoped_units()` returns
them, and the engine skips the constraint on the others. `prepare()` has the list:

```cpp
    void on_reset(const QuadraticAssignmentProblem& problem) override {
        detail::requirePrepared(problem);
        scope_ = &problem.capacitatedLocations;
    }
```

```cpp
    const std::vector<int>* scoped_units() const override { return scope_; }

private:
    const std::vector<int>* scope_ = nullptr;
```

Half of the locations of the instance have a capacity, so the scope halves the calls:
in 1 million steps of the solve of step 2 the engine called `cost()` 2.0 million times
without it and 1.0 million times with it. The solve printed the same line as before,
as it must. With the scope switched off and on inside one build (an environment
variable read in `on_reset()`, the median of five runs each), the assembly went from
2.71 to 2.86 million steps per second with the constraint at weight 0, and from 2.73
to 2.86 at weight 20. Measure a small change that way. The builds of step 2 and step 4
ran at 2.72 and 2.71 at weight 0, and at 2.71 and 2.72 at weight 20: a rebuild alone
moves a figure by as much as the scope gains ([step 6](#6-measure)).

The constraint costs the same at weight 0 as at weight 20. The engine keeps the cost of
every constraint in the picks, whatever its weight, and `get_constraints()` lists the
opt-in constraint. Leave it out of the picks when you do not use it, as `others` does
in step 2: the five alone ran at 2.94 in the build with the switch.

The scope is also part of what the constraint means, so settle it when you add the constraint:

- `constraint_costs()` reports the scoped locations as `units`. Two checks of step 3
  list them and change with the scope: the scoped entries in
  `verifyRepeatsAndOffsets()`, and
  `test_the_scoped_side_constraints_report_their_units`.
- A pick with an `offset` and `clip=False` shapes every location the constraint
  evaluates, a raw cost of 0 included. Without a scope that is every location; with
  it, the locations that have a capacity.

**Keep raw costs whole numbers.** Demands and capacities are `int64`, so the costs
stay exact. Return `static_cast<double>` of the integer result.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`qap.solve(problem)` must score it, and it scores with the monolithic calculators:
1.3 times the steps per second of the assembly, and the only path that runs
parallel starts. `qap_plugin_test` compares the default assembly with them on random
assignments, so it fails from the moment the weight is not 0 until both sides agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`. The
comment above the struct and the Python spec (its docstring and the last sentence of
both descriptions) say weight 0; change them with it.

**2. The term**, in four files. `RawCosts` in `cpp/include/qap/score_calculator.hpp`
holds the raw total of every constraint. Add the field, after `locationCapacityCost`:

```cpp
        std::int64_t unusedLocationCapacityCost = 0;
```

In `cpp/src/score_calculator.cpp`, the full recompute, include the header, read the
weight off the component (`kUnusedWeight`), and add it to the first `static_assert`
and its level to the second. Then the total in `rawCosts()`, from the loads it has
summed already, and the term in `score()`:

```cpp
    // soft: unusedLocationCapacityCost — the other side of the same loads.
    for (int l : problem.capacitatedLocations) {
        raw.unusedLocationCapacityCost +=
                std::max<std::int64_t>(0, problem.capacityOf[l] - load[l]);
    }
```

```cpp
    score.soft = static_cast<double>(-(kUnusedWeight * raw.unusedLocationCapacityCost +
                                       kAssignmentWeight * raw.assignmentCost +
                                       kFlowWeight * raw.flowDistanceCost));
```

The incremental calculator keeps one running total per constraint, and retracts and
inserts a facility's terms when it moves. Declare the member `unused_` and the helper
`capacitySlack()` in `cpp/include/qap/incremental_calculator.hpp`. In
`cpp/src/incremental_calculator.cpp`: the same include and weight, `capacitySlack()`
(`capacityPenalty()` with the subtraction turned around), the start value in
`rebuild()`, the total in `rawCosts()`, the same term in `score()`, and in both
`insert()` and `retract()` the two lines around the change of the load:

```cpp
    // Every capacity starts unused (nothing stands anywhere yet).
    unused_ = 0;
    for (int l : solution.capacitatedLocations) unused_ += solution.capacityOf[l];
```

```cpp
    overCapacity_ -= capacityPenalty(location);
    unused_ -= capacitySlack(location);
    load_[location] += p.demandOf[facility];
    overCapacity_ += capacityPenalty(location);
    unused_ += capacitySlack(location);
```

The default path ran at 3.55 million steps per second with the sixth constraint in
it. That is inside what it ran at with five in the builds before, 3.54 to 3.71
([step 6](#6-measure)): the term costs less than a rebuild moves the figure.

**3. The parity checks.** The `expected` totals in `qap_plugin_test.cpp` now read
`raw.unusedLocationCapacityCost`, and the row in `verifyCatalog()` has the weight 1.
`perConstraint()`, `kConstraintNames` and the count of the walk in
`tests/quadratic_assignment/cpp/qap_test.cpp` get the sixth entry: that walk compares
the incremental calculator with the full recompute, total by total. `qap_test.cpp`
also has a positive and a zero case by hand for each of the five totals, in
`verifyConstraintsByHand()`; add the pair for the sixth. The Python reference gets the
weight in `REFERENCE_DEFAULTS`, and `test_constraint_info_reports_the_cpp_defaults`
the row.

**4. What else moves.** With that done the parity checks pass on both sides, and
every check that states a score at the defaults fails:

- `qap_test`: 2 checks, the hand-computed scores of the side instance. Its identity
  assignment is now -45 soft, not -37, because L2 holds a demand of 2 on a capacity
  of 10;
- `qap_plugin_test`: 12 checks, the scores it works out by hand on the same instance
  (dropped components, overrides, exponents);
- Python: 7 cases, in `test_constraint_specs.py` (5) and `test_solve.py` (2). One of
  them is `score_soft == -total_cost`: the model's `total_cost` stays the flow cost
  plus the direct cost;
- the table and the counts in [README.md](README.md), the score in
  [python.md](python.md), and the constraint lists and counts in the comments of
  `common.hpp`, `constraints.hpp`, `domain.hpp`, `score_calculator.hpp`,
  `incremental_calculator.hpp` and `bindings.cpp` and in the docstrings of
  `constraints.py`, `__init__.py` and `_core.pyi`;
- the CP-SAT model: without the constraint at the same weight in
  `external_solver.quadratic_assignment`,
  [`benchmarks/quadratic-assignment`](../../../benchmarks/quadratic-assignment) no
  longer compares one objective. Its published results and `CHANGELOG.md` follow.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

steps = 5_000_000
for label, picked in (("default path", None), ("assembly", get_constraints())):
    core = problem.to_core()
    config = qap.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    qap.solve(core, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:14s} {steps / seconds / 1e6:5.2f} M steps/s  {core.score}")
```

Both lines end on the same score: same seed, same steps, same scoring. If they
differ after a change, the two paths no longer score alike.

| Measured on the instance of step 1, median of three runs | Steps per second |
| --- | --- |
| default path, five constraints | 3.71 million |
| assembly, five constraints | 2.86 million |
| assembly, five + the constraint of step 2 at weight 0 | 2.72 million |
| assembly, five + the constraint of step 2 at weight 20 | 2.71 million |
| assembly, five + the constraint with the scope (step 4) at weight 0 | 2.71 million |
| assembly, five + the constraint with the scope at weight 20 | 2.72 million |
| default path, six constraints (step 5) | 3.55 million |
| assembly, six constraints (step 5) | 2.72 million |

Each state of the tree is its own build of the extension, and a build moves the figures
of code it did not change: the default path, which steps 2 to 4 leave as it is, ran at
3.71, 3.58 and 3.54 in the builds of the unchanged tree, of step 2 and of step 4. That
is as much as the scope of step 4 gains, so the table does not show it. A figure also
moves by about 2 percent from one run to the next, and other work on the machine, which
there was when these were taken, lowers every row together. The two default-path rows
are not the same work either: with the constraint on, the solve makes other moves.

**What it buys.** A constraint changes what the search can reach, so compare the results
at the same budget over several seeds, with and without it:

```python
for seed in range(1, 6):
    config = qap.SolverConfig()
    config.step_count_limit = 2_000_000
    config.random_seed = seed
    for picked in (others, picks):
        raw = qap.constraint_costs(qap.solve(problem, config=config, constraints=picked), picks)
        print(seed, sum(raw["unused_location_capacity_cost"]["costs"]), raw["flow_distance_cost"]["costs"])
```

Each line is the unused capacity and the flow cost, without the constraint and then
with it at weight 20. On the instance the five solves without the constraint leave 95,
70, 71, 64 and 68 of capacity unused; with it they leave 36, 41, 40, 36 and 33. The
flow cost is 188 to 516 higher on four seeds and 72 lower on one. A step limit makes
the lines repeatable.

Against CP-SAT at a fixed time: `uv run quadratic-assignment-compare --seconds 10
--workers 8` in `benchmarks/quadratic-assignment/cpsat`
([README](../../../benchmarks/quadratic-assignment/README.md)). It solves on the
default path only, so it measures a constraint after step 5 and with the constraint in the
CP-SAT model. The QAPLIB runs of `benchmarks/quadratic-assignment/bench` have no
capacities, so this constraint is 0 on them.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/qap/constraints/<subject>_cost.hpp` | new | | scope | weight |
| `cpp/include/qap/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | the spec's text, counts |
| `tests/quadratic_assignment/cpp/qap_plugin_test.cpp` | | case, totals, counts | scoped entries | totals, row |
| `tests/quadratic_assignment/test_constraint_specs.py` | | lists, cases | `units` | row, expected scores |
| `tests/quadratic_assignment/reference_quadratic_assignment.py` | | the constraint | | weight |
| `tests/quadratic_assignment/test_reference_parity.py` | | `SPECS` | | |
| `docs/use-cases/quadratic-assignment/README.md`, `python.md` | | table row | | defaults |
| `cpp/include/qap/score_calculator.hpp`, `incremental_calculator.hpp` | | | | the fields |
| `cpp/src/score_calculator.cpp`, `incremental_calculator.cpp` | | | | the term |
| `tests/quadratic_assignment/cpp/qap_test.cpp` | | | | totals, expected scores |
| external-solver, `benchmarks/quadratic-assignment` | | the same constraint | | results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.
