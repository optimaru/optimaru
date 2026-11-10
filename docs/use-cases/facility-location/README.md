# Facility Location

The Timefold Quickstarts `facility-location` example model
(Apache-2.0; see the repository [NOTICE](../../../NOTICE))
(Java/Quarkus) ported, with modifications, onto this repository's C++ engine (`optimaru/cpp/optimaru/`).
The classic FLP: pick one facility for every consumer.

## Domain (assignment shape)

- `Consumer.facility` — the only decision variable. An index into `facilities`,
  `-1` = unassigned (Java's `@PlanningVariable Facility facility` null).
- `Facility` — a shadow entity with `@InverseRelationShadowVariable(consumers)` in Java,
  but here the score calculator maintains the aggregates (assigned demand, consumer
  count), so it is a plain problem fact.
- Distance = `ceil(Euclidean degree distance x 111 km)` (Java `Location.getDistanceTo`).

## Constraints (Score — the engine score type as-is, no mapping)

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost per facility (0 = no violation) |
|---|---|---|---|---|
| `facility_capacity_cost` | `FacilityCapacityCost` | hard | 1 | `max(0, assigned demand - capacity)` while used |
| `facility_setup_cost` | `FacilitySetupCost` | soft | 2 | its setup cost while it serves at least one consumer |
| `distance_from_facility_cost` | `DistanceFromFacilityCost` | soft | 6 | summed consumer-to-facility distance (m) |

Every constraint is flippable (`level="hard"` / `level="soft"`), none is a reward and all
three are scored by default (`default_enabled`);
`facility_location.constraint_info()` reports exactly this table from the C++
components (`kDefaultLevel` / `kDefaultWeight`). The distance weight **6** is the
`ConstraintWeightOverrides` demo value of the Java solution (the provider declares
`ofSoft(5)`; the solution overrides it with `ofSoft(6)`).

The problem carries no weights: they come only from constraint selections, e.g.
`constraints=[FacilityCapacityCost(), FacilitySetupCost(), DistanceFromFacilityCost(weight=5)]`
for the un-overridden Java provider.

Besides `weight`, `level` and `exponent`, every constraint takes `offset` and `clip`
(keyword-only), and may be picked several times. Each facility's raw cost `x` is shaped as
`sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which lowers the penalty of every
facility below the offset, an unused one included); `offset=0` is the plain shaping. Both
apply per facility, not to the total. `clip=False` with an offset is refused on hard (the
capacity constraint by default), because hard 0 must mean no violation. Each pick is its own
instance with its own knobs, and the instances add up:

```python
fl.solve(problem, constraints=[
    FacilityCapacityCost(),
    FacilitySetupCost(),
    DistanceFromFacilityCost(),                                    # 6 x distance
    DistanceFromFacilityCost(weight=0.01, exponent=2, offset=500),  # + 0.01 x (distance beyond 500 m)^2
])
```

`fl.constraint_costs(problem, picks)` keys the second pick `distance_from_facility_cost#2`.

## C++

- `optimaru/facility_location/cpp/include/fl/domain.hpp` — domain + `optimaru::SolutionTraits` specialization
- `optimaru/facility_location/cpp/include/fl/constraints/` — one component per constraint: `facility_capacity_cost.hpp`,
  `facility_setup_cost.hpp`, `distance_from_facility_cost.hpp` (+ `common.hpp`);
  `optimaru/facility_location/cpp/include/fl/constraints.hpp` assembles the default set (`defaultConstraints()`)
- `score_calculator` — full recompute (the reference)
- `incremental_calculator` — before/after hooks, O(1) per move
- `solver` — engine wrapper (CH + Late Acceptance + adaptive ruin & recreate)
- `optimaru/facility_location/cpp/src/main.cpp` — demo reproducing the Java `DemoDataGenerator` scale
  (30 facilities x capacity 150, 60 consumers x demand 15, London box, setup cost N(50000, 10000²))

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=facility_location
cmake --build build -j4
./build/facility_location/fl_test         # per-constraint hand checks + 3000 random moves incremental==full + ScoreChecks::Full solve
./build/facility_location/fl_plugin_test  # plugin assembly == monolith, raw costs by hand, catalog, overrides
./build/facility_location/fl_demo 10   # 10 s solve; checks hard 0 + every consumer assigned (exit 0)
```

## Python

pybind11 package `facility_location` (module `optimaru.facility_location._core`).

```bash
OPTIMARU_USE_CASES=facility_location uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/facility_location
```

```python
from optimaru import facility_location as fl

problem = fl.FacilityLocationProblem(facilities, consumers)
fl.solve(problem, time_limit_seconds=5.0, random_seed=42)
print(problem.score, problem.used_facility_count(), problem.total_cost())
```

The pydantic v2 serialization mirror is `optimaru.facility_location.models`
(`FacilityLocationProblemModel.from_core / to_core`).

## Usage

```sh
OPTIMARU_USE_CASES=facility_location uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/facility_location
```

```python
from optimaru import facility_location as fl

facilities = [
    fl.Facility("F1", fl.Location(51.50, -0.12), 50_000, 300),   # id, location, setup cost, capacity
    fl.Facility("F2", fl.Location(51.52, -0.08), 40_000, 300),
]
consumers = [fl.Consumer(f"C{i}", fl.Location(51.49 + i * 0.005, -0.10), 30) for i in range(10)]
problem = fl.FacilityLocationProblem(
    facilities, consumers, fl.Location(51.44, -0.16), fl.Location(51.56, -0.01)
)
fl.solve(problem, time_limit_seconds=5.0, random_seed=42)          # solves in place
print(problem.score, problem.total_cost())
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, a full `config` (`facility_location.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=facility_location && cmake --build build -j
ctest --test-dir build
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is `FacilityLocationProblemModel.from_core(problem)` or that model as a JSON dict. `solve` takes the engine object, so convert the result with `to_core()`:

```python
from optimaru.facility_location.models import FacilityLocationProblemModel

solved = FacilityLocationProblemModel.from_core(problem)        # `problem` solved above
restart = fl.with_initial_solution(solved, solved).to_core()    # same facts, so it can be both arguments
fl.solve(restart, time_limit_seconds=5.0, random_seed=43)
print(restart.score, restart.total_cost())
```

Consumers are matched by id. A consumer the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. `facility` is a position in `facilities`, so a solution that lists different facilities, or the same ones in another order, raises `ValueError`. So do unknown ids and out-of-range values, before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the
engine is `optimaru/facility_location/cpp/include/fl/solver.hpp` + `optimaru/facility_location/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). `CloseFacilityMove` (empties a facility by moving all its consumers) is defined and registered in `optimaru/facility_location/cpp/src/solver.cpp` (`runEngine`) — use it as the template. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/facility_location/cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`optimaru/facility_location/cpp/include/fl/constraints/` (currently `facility_capacity_cost`, `facility_setup_cost`,
`distance_from_facility_cost`):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
   an `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `optimaru/facility_location/cpp/include/fl/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` — `fl_plugin_test` checks they agree bit for bit — and a positive and a
   zero hand-calculated case to `fl_test` / `fl_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `optimaru/facility_location/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, level="hard")])`;
5. mirror it in the CP-SAT model, `external_solver.facility_location` in
   [external-solver](https://github.com/optimaru/external-solver) (a plugin subclassing the
   spec; defaults come from `constraint_info()`), so the benchmark in
   [`benchmarks/facility-location/cpsat`](../../../benchmarks/facility-location/cpsat) stays like
   for like.

## Benchmark

[`benchmarks/facility-location`](../../../benchmarks/facility-location) compares this use case with OR-Tools CP-SAT on the same instances.
