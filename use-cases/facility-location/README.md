# Facility Location

The `facility-location` example model (Apache-2.0; see the repository NOTICE)
(Java/Quarkus) ported onto this repository's C++ engine (`include/optimaru/`).
The classic FLP: pick one facility for every consumer.

## Domain (BASIC variable)

- `Consumer.facility` — the only planning variable. An index into `facilities`,
  `-1` = unassigned (Java's `@PlanningVariable Facility facility` null).
- `Facility` — a shadow entity with `@InverseRelationShadowVariable(consumers)` in Java,
  but here the score calculator maintains the aggregates (assigned demand, consumer
  count), so it is a plain problem fact.
- Distance = `ceil(Euclidean degree distance x 111 km)` (Java `Location.getDistanceTo`).

## Constraints (HardSoftScore — the engine score type as-is, no mapping)

| Constraint | Level | What it measures |
|---|---|---|
| facility capacity | hard | `max(0, assigned demand - capacity)` per used facility |
| facility setup cost | soft x2 | setup cost of facilities with at least one consumer |
| distance from facility | soft x6 | consumer-to-facility distance (m) |

The distance weight **6** is the `ConstraintWeightOverrides` demo value of the Java
solution (the provider declares `ofSoft(5)`; the solution overrides it with
`{"distance from facility": ofSoft(6)}`). Adjust it with the
`FacilityLocationProblem.distanceFromFacilityWeight` / `setupCostWeight` fields
(default 6 / 2 = the effective quickstart values).

## cpp/

- `include/fl/domain.hpp` — domain + `optimaru::SolutionDescriptor` specialization
- `score_calculator` — full recompute (the reference)
- `incremental_calculator` — before/after hooks, O(1) per move
- `solver` — engine wrapper (CH + Late Acceptance + adaptive ruin & recreate)
- `src/main.cpp` — demo reproducing the Java `DemoDataGenerator` scale
  (30 facilities x capacity 150, 60 consumers x demand 15, London box, setup cost N(50000, 10000²))

```bash
cd cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/fl_test      # per-constraint hand checks + 3000 random moves incremental==full + FULL_ASSERT solve
./build/fl_demo 10   # 10 s solve; checks hard 0 + every consumer assigned (exit 0)
```

## python/

pybind11 package `facility_location` (module `facility_location._core`).

```bash
cd python
uv venv && uv pip install -e . pytest pydantic
uv run pytest -q
```

```python
import facility_location as fl

problem = fl.FacilityLocationProblem(facilities, consumers)
fl.solve(problem, time_limit_seconds=5.0, random_seed=42)
print(problem.score, problem.used_facility_count(), problem.total_cost())
```

The pydantic v2 serialization mirror is `facility_location.models`
(`FacilityLocationProblemModel.from_core / to_core`).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/facility-location/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import facility_location as fl

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
cmake -S use-cases/facility-location/cpp -B build/facility-location && cmake --build build/facility-location -j
ctest --test-dir build/facility-location
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **BASIC**; the domain solver that configures the
engine is `cpp/include/fl/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). `CloseFacilityMove` (empties a facility by moving all its consumers) is defined and registered in `cpp/src/solver.cpp` (`runEngine`) — use it as the template. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/fl/constraints/` (currently `distance_from_facility`, `facility_capacity`, `facility_setup_cost`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/fl/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `fl_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/facility_location/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/facility-location/cpsat`](../../benchmarks/facility-location/cpsat) so the
   benchmark stays like for like.

## Benchmark

[`benchmarks/facility-location`](../../benchmarks/facility-location) compares this use case with OR-Tools CP-SAT on the same instances.
