# Use cases

Each directory is an independent problem built on the engine in
[`include/optimaru/`](../include/optimaru). Competing solvers (OR-Tools
CP-SAT / Routing) never live here — they are in
[`benchmarks/`](../benchmarks).

| Use case | Kind | Python package |
| --- | --- | --- |
| [assembly-line-balancing](assembly-line-balancing) | pure Python | `assembly_line_balancing` |
| [bed-allocation](bed-allocation) | C++ engine | `bed_allocation` |
| [bin-packing](bin-packing) | pure Python | `bin_packing` |
| [conference-scheduling](conference-scheduling) | C++ engine | `conference_scheduling` |
| [employee-scheduling](employee-scheduling) | C++ engine | `es_scheduling` |
| [facility-location](facility-location) | C++ engine | `facility_location` |
| [flight-crew-scheduling](flight-crew-scheduling) | C++ engine | `flight_crew` |
| [food-packaging](food-packaging) | C++ engine | `food_packaging` |
| [knapsack](knapsack) | pure Python | `knapsack` |
| [maintenance-scheduling](maintenance-scheduling) | C++ engine | `maintenance_scheduling` |
| [max-cut](max-cut) | pure Python | `max_cut` |
| [meeting-scheduling](meeting-scheduling) | C++ engine | `meeting_scheduling` |
| [order-picking](order-picking) | C++ engine | `order_picking` |
| [pickup-delivery](pickup-delivery) | C++ engine | `pdp_routing` |
| [project-job-scheduling](project-job-scheduling) | C++ engine | `project_job` |
| [quadratic-assignment](quadratic-assignment) | pure Python | `quadratic_assignment` |
| [school-timetabling](school-timetabling) | C++ engine | `st_timetabling` |
| [semiconductor-scheduling](semiconductor-scheduling) | pure Python | `semiconductor_scheduling` |
| [sports-league-scheduling](sports-league-scheduling) | C++ engine | `sports_league` |
| [task-assigning](task-assigning) | C++ engine | `task_assigning` |
| [tournament-scheduling](tournament-scheduling) | C++ engine | `tournament_scheduling` |
| [vehicle-routing](vehicle-routing) | C++ engine | `vrp_routing` |

**C++ engine** use cases run the optimaru metaheuristics; **pure Python**
ones are small reference implementations with their own simulated-annealing
loop and do not use the engine.

## Layout of a C++ engine use case

```
<slug>/
├── cpp/
│   ├── CMakeLists.txt
│   ├── include/<ns>/
│   │   ├── domain.hpp                 problem facts, planning variable, Score,
│   │   │                              SolutionDescriptor specialization
│   │   ├── score_calculator.hpp       full (reference) score
│   │   ├── incremental_calculator.hpp tuned incremental score (default path)
│   │   ├── constraints.hpp            default constraint assembly
│   │   ├── constraints/<name>.hpp     one plugin component per constraint
│   │   └── solver.hpp                 domain Solver: Config -> engine wiring
│   ├── src/*.cpp
│   └── test/<ns>_test.cpp, <ns>_plugin_test.cpp
└── python/
    ├── bindings.cpp                   pybind11 module <package>._core
    ├── setup.py, pyproject.toml
    └── <package>/{__init__,models,constraints}.py, _core.pyi
```

Build and test:

```sh
cmake -S <slug>/cpp -B build/<slug> && cmake --build build/<slug> -j
ctest --test-dir build/<slug>

cd <slug>/python
uv sync --all-groups --all-extras      # builds the pybind extension
uv run pytest -q
```

## Starting from an existing solution

Every use case can start its search from a solution you already have instead of
constructing one. Pass what a previous solve returned, as the result model or as
its JSON:

```python
from vrp_routing.models import solve_request, with_initial_solution

first = solve_request(request)
better = solve_request(with_initial_solution(request, first))
```

The contract is the same everywhere:

- A complete solution skips construction: the search starts exactly from it.
- A partial solution keeps what it gives, and only what it leaves out is constructed.
- A solve's result is accepted back, even a hard-infeasible one. The exceptions are
  pickup-delivery, which refuses a route with a broken pickup and dropoff pair, and
  school timetabling, which refuses a course placed twice at one time.
- Entities are matched by id, never by position. Unknown ids and out-of-range values
  raise `ValueError`.
- `with_initial_solution` returns a new problem and leaves its input untouched.
- Pinned parts are problem facts. Most use cases keep the problem's value and
  ignore what the solution says. Food packaging, pickup-delivery, max-cut and
  school timetabling raise `ValueError` when the solution contradicts them.
- A seed is used once: a result edited and solved again starts from construction
  unless you pass it through `with_initial_solution` again.

School timetabling takes dicts, so it uses a keyword instead:
`solve(problem, initial_solution=previous)`. Each use case's README lists its
exceptions. To stop right after the start, use a zero time limit:
`step_count_limit = 0` means no limit. The pure-Python use cases keep a minimum
budget, so they stop with `SolverConfig(max_iterations=0)` instead.

## Extending a C++ engine use case

### Adding a move

The engine already mixes the generic moves for the variable kind: change /
swap for BASIC variables; relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap
for LIST variables. Their weights are `Solver::Config` fields (exposed to
Python as `*_move_weight`). A domain move is a
`optimaru::MoveOperator<Solution>`
([`preview/api/move/operators.hpp`](../include/optimaru/preview/api/move/operators.hpp)):

```cpp
class MyMove final : public optimaru::MoveOperator<MySolution> {
public:
    const char* name() const override { return "myMove"; }
    // Do ONE random move through ctx.director (never touch the solution
    // directly), remember how to undo it, report the touched entity ids.
    bool apply(optimaru::OperatorContext<MySolution>& ctx, std::mt19937_64& rng,
               std::vector<int>& touched) override;
    void undoLast(optimaru::OperatorContext<MySolution>& ctx) override;
};
```

Register it where the use case's `Solver` builds the engine (in
`cpp/src/solver.cpp`):

```cpp
engine.addMoveOperator(std::make_unique<MyMove>());
```

The engine picks among the built-in mix and every registered operator
adaptively (roulette wheel rewarded by move outcome). For large-neighbourhood
search, implement `optimaru::RuinOperator<Solution>` instead — it only
chooses which entities to ruin; the engine removes and greedily recreates
them — and register it with `engine.addRuinOperator(...)`. Run the solver
once with `full_assert` on (`EnvironmentMode::FULL_ASSERT`) after adding a
move: it fails fast if the move leaves the incremental score stale.

### Adding or changing the construction heuristic

The engine's construction heuristic assigns every still-unassigned entity
(LIST: element) by cheapest insertion. Two levers:

1. **Order** — set `ConstructionHeuristicConfig::entityDifficulty` in the
   use case's engine config; entities are then placed in decreasing
   difficulty (First Fit Decreasing) instead of index order:

   ```cpp
   engineConfig.constructionHeuristic.entityDifficulty =
       [&](int entity) { return demandOf(entity); };   // hardest first
   ```

2. **A domain constructor** — build a (partial) initial solution in the
   use case's `Solver` before calling the engine. The engine keeps what is
   already assigned and only fills the rest, so a domain constructor can
   place the hard part (e.g. pickup-delivery's `constructPairs` places each
   pickup together with its drop-off) and leave the remainder to the generic
   heuristic.

### Adding a hard or soft cost

A constraint is one named component; its level (hard / soft) and weight are
defaults the caller can override per solve.

1. **Facts** — if the cost needs new input, add it to `domain.hpp` (and to
   `models.py`'s `to_core()` / `from_core()` and `bindings.cpp`).
2. **Component** — add `cpp/include/<ns>/constraints/<name>.hpp`. Most costs
   are a `UnitConstraint` (a pure function of one unit — an entity or a list
   — and its members); costs that belong to no unit (e.g. "unassigned
   count") are a `GlobalConstraint`. Positive cost = penalty, `0` = no
   violation:

   ```cpp
   struct MyRule final : MyUnitConstraint {
       explicit MyRule(double weight = 1.0)
           : MyUnitConstraint("myRule", weight, /*hard=*/false) {}
       double cost(const MySolution& s, int unit,
                   const std::vector<int>& members) const override { ... }
   };
   ```

3. **Assembly** — include it in `constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set. Python sees it
   automatically under its snake_case name (`list_constraints()`).
4. **Default fast path** — if it is in the default set, add the same raw cost
   to `score_calculator` and `incremental_calculator`;
   `<ns>_plugin_test` checks that the plugin assembly and the monolithic
   calculators agree bit for bit.
5. **Python spec** — add a `Constraint` subclass with `name = "my_rule"` to
   `<package>/constraints.py` so callers can pick it, reweight it or flip its
   level: `solve(problem, constraints=get_constraints([..., MyRule(weight=5,
   hard=True)]))`.
6. **Tests and benchmark parity** — a hand-calculated positive and zero case
   in `<ns>_plugin_test`, and the same cost in the CP-SAT model under
   `benchmarks/<slug>/cpsat` so the comparison stays like for like.

## Extending a pure Python use case

Everything is in `python/<package>/`:

- **Move** — the `neighbor(state, rng)` function inside `solve()` in
  `__init__.py` returns one random neighbour of the current state; add a new
  kind of change there (choose among kinds with `rng`).
- **Construction heuristic** — the `initial` state built at the top of
  `solve()` (e.g. knapsack's greedy by value/weight ratio).
- **Hard / soft cost** — add a `Constraint` subclass with `name`, `label`
  and `default_hard` to `constraints.py`, register it in the default list
  there, and return its raw cost under the same name from
  `constraint_costs()` in `__init__.py`.

The annealing loop itself is `common.anneal` (temperature, cooling rate,
stagnation restart, multi-start via `parallel_solver_count`).
