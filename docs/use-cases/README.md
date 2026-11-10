# Use cases

Each use case is an independent problem built on the engine in
[`optimaru/cpp/optimaru/`](../../optimaru/cpp/optimaru) and shipped as a subpackage of
`optimaru`. This directory holds one guide per use case. Competing solvers
(OR-Tools CP-SAT / Routing) never live in the package; they are in
[`benchmarks/`](../../benchmarks).

| Use case (guide) | Python package |
| --- | --- |
| [assembly-line-balancing](assembly-line-balancing) | `optimaru.assembly_line_balancing` |
| [bed-allocation](bed-allocation) | `optimaru.bed_allocation` |
| [bin-packing](bin-packing) | `optimaru.bin_packing` |
| [conference-scheduling](conference-scheduling) | `optimaru.conference_scheduling` |
| [employee-scheduling](employee-scheduling) | `optimaru.employee_scheduling` |
| [facility-location](facility-location) | `optimaru.facility_location` |
| [flight-crew-scheduling](flight-crew-scheduling) | `optimaru.flight_crew_scheduling` |
| [food-packaging](food-packaging) | `optimaru.food_packaging` |
| [knapsack](knapsack) | `optimaru.knapsack` |
| [maintenance-scheduling](maintenance-scheduling) | `optimaru.maintenance_scheduling` |
| [max-cut](max-cut) | `optimaru.max_cut` |
| [meeting-scheduling](meeting-scheduling) | `optimaru.meeting_scheduling` |
| [order-picking](order-picking) | `optimaru.order_picking` |
| [pickup-delivery](pickup-delivery) | `optimaru.pickup_delivery` |
| [project-job-scheduling](project-job-scheduling) | `optimaru.project_job_scheduling` |
| [quadratic-assignment](quadratic-assignment) | `optimaru.quadratic_assignment` |
| [school-timetabling](school-timetabling) | `optimaru.school_timetabling` |
| [semiconductor-scheduling](semiconductor-scheduling) | `optimaru.semiconductor_scheduling` |
| [sports-league-scheduling](sports-league-scheduling) | `optimaru.sports_league_scheduling` |
| [task-assigning](task-assigning) | `optimaru.task_assigning` |
| [tournament-scheduling](tournament-scheduling) | `optimaru.tournament_scheduling` |
| [vehicle-routing](vehicle-routing) | `optimaru.vehicle_routing` |

Every use case is a C++ domain on the optimaru engine with a pybind11 Python
package; they all share one constraint contract (below). Six of them
(assembly-line-balancing, bin-packing, knapsack, max-cut, quadratic-assignment
and semiconductor-scheduling) also carry a pure-Python reference scorer as
`tests/<use case>/reference_<use case>.py`, an independent oracle the C++ costs are
tested against.

## One way to solve

Every use case has the same entry point, `optimaru.<use case>.solve`:

```python
solved = uc.solve(
    problem,
    time_limit_seconds=5.0,        # every argument below is optional
    random_seed=42,
    parallel_solver_count=1,
    stop_at_first_feasible=False,  # stop as soon as hard 0 is reached
    full_assert=False,             # check every incremental score (slow; for debugging)
    on_new_best=None,              # called with (step, score) on every new best
    config=None,                   # a SolverConfig; the keywords above override a copy of it
    constraints=None,              # constraint picks; None = the default set
)
```

It returns the solved problem, and the score is on it. A problem built from the
compiled classes (`optimaru.<use case>.<Problem>`) is solved in place and
returned, with its score in `solved.score`. A pydantic model from
`optimaru.<use case>.models` comes back as a solved model with its score fields
(`score_hard`, `score_soft`, and one per further level the use case has). Always
use the return value: some use cases write the result into the model they were
given and others return a new one.

School timetabling takes and returns dicts (a `TimetableProblem` in, a
`Timetable` out) and has two more keywords, `mode` and `initial_solution`.
`Solver(config).solve(problem)` and `solve_with_constraints(problem, picks)`
are the same solves one level down, without the keyword conveniences.

## Constraints: names and knobs

Every constraint is one named component, `<Subject>Cost` in C++ and Python and
`<subject>_cost` as its Python name (`vehicle_capacity_cost`; the C++ registry
name is the camelCase `vehicleCapacityCost`). Its raw cost is `0` when nothing
is violated and positive otherwise; a **reward** (a quantity to maximize, such
as knapsack's `selected_value_cost`) has a raw cost `<= 0`.

A selection is a list of picks, written as class calls
(`[VehicleCapacityCost(), TravelTimeCost(weight=10)]`). Each pick sets five
knobs and nothing else:

- `weight` — `None` keeps the default weight, in units of the constraint's
  level.
- `level` — `None` keeps the default level. Otherwise a `Level` from the
  package's `constraints` module, or its name: `Level.HARD` / `"hard"`,
  `Level.MEDIUM` / `"medium"`, `Level.SOFT` / `"soft"`. `medium` needs a domain
  with a medium level. A reward refuses `"hard"`, and a structural constraint
  (`level_flippable` false, e.g. school timetabling's `fixed_slots_cost`)
  refuses any change of level.
- `exponent` — `1.0` by default, `> 0`, sign-preserving; applied per unit for
  a unit constraint and to the single total for a global one, before the
  weight.
- `offset` — `0.0` by default, any finite number, in the unit of the raw cost;
  keyword-only. It shifts the shaping at the same granularity as `exponent`.
- `clip` — `True` by default; keyword-only. `True` makes the offset a
  threshold, `sign(x) * max(0, |x| - offset) ** exponent`: only the part of a
  raw cost `x` beyond the offset counts. `False` is the literal formula, the
  sign-preserving `(x - offset) ** exponent`, which is negative (a reward)
  below the offset and charges every unit the constraint evaluates, a raw 0
  included. Each use case's README says which units those are. `clip=False`
  with a non-zero offset is refused on hard, and on medium where medium
  counts toward the hard score (task assigning), because hard 0 must mean no
  violation: `ValueError: constraint 'vehicle_capacity_cost': clip=False with
  a non-zero offset cannot be hard, because hard 0 must mean no violation; use
  clip=True (a threshold) or another level`. The threshold (`clip=True`) is
  allowed on every level.

So `TravelTimeCost(offset=10, exponent=3)` adds `max(0, x - 10)**3` per
vehicle, and the soft `TravelTimeCost(offset=10, exponent=3, clip=False)` adds
`(x - 10)**3` per vehicle, an idle one included. Both formulas apply per unit,
not to the total. With `offset=0` both modes are the plain `x ** exponent`. A
weight of 0 contributes exactly 0 at any exponent and offset.

A negative offset with `clip=True` is a fixed charge per unit in use:
`TravelTimeCost(offset=-60)` adds `x + 60` for a vehicle that drives and 0 for
an idle one. That jump at 0 needs the raw cost exactly, so the engine recomputes
such a constraint for every unit a move changes instead of updating it by the
difference. The score is the same, but the solve is slower: in vehicle routing
it took about half as many steps per second as `TravelTimeCost()` on 100 to
1,000 visits. A positive threshold and `clip=False` are not recomputed this
way.

A constraint may be picked several times. Each pick is its own instance with its
own knobs (its level included) and, where the constraint has domain parameters,
its own parameters. The instances are scored independently and add up, so
`[TravelTimeCost(exponent=2), TravelTimeCost(exponent=3)]` is `x**2 + x**3`, and
two identical picks simply count twice. An empty list is an error, and so is an
unknown name. Each package's `constraints` module also holds ready-made default
instances under the snake-case names (`vehicle_capacity_cost` is
`VehicleCapacityCost()`); the docs and examples use the class calls.

Some domains also have a **medium** level between hard and soft (unassigned
visits, stays, requests, tasks, ...). Any flippable constraint may be moved onto
it, and a weight is in units of the level in effect. The domain declares once how
medium folds into the engine's hard/soft score (`medium_fold` on every
`constraint_costs()` entry). Where it folds below hard (task assigning), medium is
whole numbers like hard: each unit's contribution is rounded, so a small fractional
weight can round to nothing, and a reward refuses `"medium"`.

Defaults are declared once, in C++ (`kDefaultWeight` / `kDefaultLevel` on the
component). Python reads them from `<package>.constraint_info()`, one
`ConstraintInfo` row per constraint with the fields `name`, `default_level`,
`default_weight`, `level_flippable`, `reward` and `default_enabled`.

- `list_constraints()` lists the default set. Employee-scheduling lists all 30
  rules its factory accepts; its defaults are the `default_enabled` rows.
- `constraint_costs(solution, constraints=None)` reports every selected
  constraint's raw costs as `{key: {costs, weight, exponent, offset, clip,
  global, level, reward, level_flippable, name, label, default_weight,
  default_level}}` (`weight` / `level` / `exponent` / `offset` / `clip` are the
  pick's knobs, `default_*` what `None` means). The key of the first pick of a
  name is the name, the k-th is `name#k`, in pick order; a scoped constraint
  also lists the units it evaluates as `units`, and a domain with a medium
  level adds `medium_fold`.
- `constraints.score(costs, picks)` re-prices them in pure Python with the
  engine's arithmetic.

## Layout of a use case

```
optimaru/<use case>/
├── __init__.py, models.py,          the Python API: solve, pydantic models,
│   constraints.py, _core.pyi        constraint specs, extension stubs
└── cpp/
    ├── bindings.cpp                 pybind11 module optimaru.<use case>._core
    ├── CMakeLists.txt               core library, demo and C++ tests
    ├── include/<ns>/
    │   ├── domain.hpp               problem facts, decision variable, Score,
    │   │                            SolutionTraits specialization
    │   ├── score_calculator.hpp     full (reference) score
    │   ├── incremental_calculator.hpp
    │   │                            tuned incremental score (default path)
    │   ├── constraints.hpp          default constraint assembly
    │   ├── constraints/*_cost.hpp   one plugin component per constraint
    │   └── solver.hpp               domain Solver: Config -> engine wiring
    └── src/*.cpp                    main.cpp is the demo
tests/<use case>/
├── test_*.py                        pytest suite
└── cpp/<ns>_test.cpp, <ns>_plugin_test.cpp
docs/use-cases/<slug>/               README.md, python.md: its guide
```

The constraint-spec base every `constraints.py` builds on is
[`optimaru/_constraint.py`](../../optimaru/_constraint.py), shared by all use cases.

Build and test one use case, from the repository root:

```sh
cmake -B build -DOPTIMARU_USE_CASES=<use case> && cmake --build build -j
ctest --test-dir build

OPTIMARU_USE_CASES=<use case> uv sync      # builds only that extension, in place
uv run pytest -q tests/<use case>
```

## Starting from an existing solution

Every use case can start its search from a solution you already have instead of
constructing one. Pass what a previous solve returned, as the result model or as
its JSON:

```python
from optimaru.vehicle_routing.models import solve_request, with_initial_solution

first = solve_request(request)
better = solve_request(with_initial_solution(request, first))
```

The contract is the same everywhere:

- A complete solution skips construction: the search starts exactly from it.
- A partial solution keeps what it gives, and only what it leaves out is constructed.
  Quadratic assignment is the exception: its search only swaps, so it refuses a
  partial assignment.
- A solve's result is accepted back, even a hard-infeasible one. The exceptions are
  pickup-delivery, which refuses a route with a broken pickup and dropoff pair, and
  school timetabling, which refuses a course placed twice at one time.
- Entities are matched by id, never by position. Unknown ids and out-of-range values
  raise `ValueError`.
- `with_initial_solution` returns a new problem and leaves its input untouched.
- Pinned parts are problem facts. Most use cases keep the problem's value and
  ignore what the solution says. Employee scheduling, food packaging,
  pickup-delivery, max-cut and school timetabling raise `ValueError` when the
  solution contradicts them.
- A seed is used once: a result edited and solved again starts from construction
  unless you pass it through `with_initial_solution` again.

School timetabling takes dicts, so it uses a keyword instead:
`solve(problem, initial_solution=previous)`. Each use case's README lists its
exceptions. To stop right after the start, use a zero time limit
(`spent_limit_seconds = 0`, or `time_limit_seconds=0` where `solve()` takes
one); `step_count_limit = 0` means no limit, not zero steps.

## Extending a use case

[`docs/extending.md`](../extending.md) is the step-by-step guide: adding a move
(a `MoveOperator` registered in the use case's `solver.cpp`), a ruin operator,
changing the construction, and adding a hard or soft cost (a component under
`constraints/`, the assembly, the fast default path, the Python spec, tests and
docs). Each use case's guide repeats the cost steps with its own file names.

## How the extensions are built

[`setup.py`](../../setup.py) builds one extension per use case
(`optimaru.<use case>._core`, for every subpackage with a `_core.pyi`) from
`optimaru/<use case>/cpp/bindings.cpp` and every `cpp/src/*.cpp` except
`main.cpp`, the same sources as the core library in its `CMakeLists.txt`. `uv sync` installs the package in editable mode and
builds the extensions in place, next to the Python files. `OPTIMARU_USE_CASES`
(comma-separated) limits the build to some use cases. uv rebuilds when a
`.cpp` / `.hpp` under `optimaru/`, `setup.py`, `pyproject.toml` or
`OPTIMARU_USE_CASES` changes; `uv sync --reinstall-package optimaru` forces it.
