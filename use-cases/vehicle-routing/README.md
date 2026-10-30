# Vehicle Routing

Domain model, constraints and API details: [`python/`](python/README.md) and [`cpp/`](cpp/README.md).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/vehicle-routing/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import vrp_routing as vrp

locations = [vrp.Location(37.5665, 126.9780),   # index 0: vehicle home
             vrp.Location(37.5700, 126.9820)]   # index 1: a customer
vehicles = [vrp.Vehicle("V1", capacity=10, home_location_index=0, departure_time=7 * 3600)]
visits = [vrp.Visit("C1", "Customer 1", location_index=1, demand=3,
                    min_start_time=8 * 3600, max_end_time=18 * 3600, service_duration=600)]
plan = vrp.VehicleRoutePlan("demo", locations, vehicles, visits)

config = vrp.SolverConfig()
config.spent_limit_seconds = 5.0
config.random_seed = 42
vrp.Solver(config).solve(plan)                  # solves in place
print(plan.score, plan.routes())
```

`Solver(config).solve(plan)` works on the engine objects; for JSON in/out use `vrp_routing.models.solve_request(...)`, which also takes `constraints=` (see below). The C++ side builds and tests with:

```sh
cmake -S use-cases/vehicle-routing/cpp -B build/vehicle-routing && cmake --build build/vehicle-routing -j
ctest --test-dir build/vehicle-routing
```

## Starting from an existing solution

```python
from vrp_routing.models import solve_request, with_initial_solution

solution = solve_request(request)                        # request: a RoutePlanRequest
next_request = with_initial_solution(request, solution)  # or solution.model_dump(mode="json")
better = solve_request(next_request)
```

`with_initial_solution` returns a new request whose vehicles carry `initial_route`, a list of visit ids. The solve keeps those routes as its starting point and construction places only the visits in no route, so a partial solution works and the `unassigned` visits get another try. With `timelimit=0` a complete solution comes back as it went in. An unknown id or a visit routed twice raises `ValueError`.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-c-engine-use-case).
The planning variable is **LIST**; the domain solver that configures the
engine is `cpp/include/vrp/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap moves
(weights in `Solver::Config`). This use case relies on the engine's generic moves only; it has no domain move yet. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.addRuinOperator(...)`.

**Changing the construction heuristic.** `entityDifficulty` is already set in `cpp/src/solver.cpp` (tightest time window and largest demand first). Set
`engineConfig.constructionHeuristic.entityDifficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/vrp/constraints/` (currently `load_utilization`, `maximize_visits_assigned`, `minimize_travel_time`, `route_costs`, `route_geometry`, `route_limits`, `route_sums`, `service_finished_after_max_end_time`, `vehicle_capacity`):
1. add `constraints/<name>.hpp` (a `UnitConstraint` or `GlobalConstraint`;
   positive cost = penalty, the level and weight are constructor defaults);
2. include it in `cpp/include/vrp/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator` — `vrp_plugin_test` checks
   they agree bit for bit;
4. add a `Constraint` spec with the snake_case `name` to
   `python/vrp_routing/constraints.py`; callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRule(weight=5, hard=True)])`.

The CVRP benchmark scores plain distance, so a new cost does not change it.

## Benchmark

[`benchmarks/vehicle-routing`](../../benchmarks/vehicle-routing) compares this use case with
the OR-Tools Routing solver on the same instances, rescored by the same evaluator.
