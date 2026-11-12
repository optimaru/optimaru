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

Every use case has the same two entry points: `optimaru.<use case>.solve` for
a problem in Python, and `optimaru.<use case>.solve_request` for a pydantic
request (JSON in, JSON out). The request carries the settings of the solve
under the names of `solve`'s keywords, so the two say the same thing in the
same words.

`solve` takes the problem and the settings as keywords:

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

It returns the solved problem, and the score is on it.

- A problem built from the compiled classes (`optimaru.<use case>.<Problem>`)
  is solved in place and returned, with its score in `solved.score`.
- A pydantic problem model from `optimaru.<use case>.models` is not changed:
  `solve` returns a new, solved model with its score fields (`score_hard`,
  `score_soft`, and one per further level the use case has). The model you
  passed stays the problem it was, so it can be solved again or handed to
  `with_initial_solution(problem, solved)`. (`calculate_score(model)`, where a
  use case accepts a model there, is the one call that writes into it: it fills
  in the model's score fields.)

`parallel_solver_count=N` runs N independent starts, start *i* with
`random_seed + i`, and returns the best of them. A solve with `constraints=...`
runs one start, except in vehicle routing and pickup and delivery, where every
start scores on its own copy of the picks. `on_new_best` reports the first
start only, so the returned
score can be better than the last one it was called with. A start that reaches
a target (`stop_at_first_feasible`, `best_score_limit`) stops the others: they
hand over their best so far, and the best of all starts is returned. Which start
gets there first depends on the threads, so with a target and more than one
start the same seed can give a different solution from run to run.

`solve_request` takes a request model, `optimaru.<use case>.models.<Name>Request`,
and the constraint selection:

```python
request = uc.models.KnapsackRequest(  # each use case names its own request
    model_input=problem_model,        # the pydantic problem
    time_limit_seconds=5.0,           # the fields below are optional and mean
    random_seed=42,                   # what solve's keywords of the same name do
    parallel_solver_count=1,
    stop_at_first_feasible=False,
    full_assert=False,
    solver_parameters={},             # further SolverConfig fields by name
)
solved = uc.solve_request(request, constraints=None)
```

Every request has these fields (the shared base is `optimaru._models.SolveRequest`).
Earlier versions named two of them differently, and those names are still
accepted, in JSON, as keywords and as attributes: `timelimit` for
`time_limit_seconds` and `num_workers` for `parallel_solver_count`. The request
is not changed. What comes back:

- In 17 use cases the request wraps the problem model as `model_input`, and
  `solve_request` returns what `solve` returns for that model: a new, solved
  model.
- Employee scheduling, order picking, pickup & delivery and vehicle routing
  answer with a response model of their own (`ScheduleSolution`,
  `OrderPickingResponse`, `PdpSolution`, `RouteSolution`). Their `solve()` takes
  the compiled problem only. Order picking's request holds `trolleys` and
  `orders` itself instead of a `model_input`. An employee scheduling request
  also has `settings`, the search options of that use case; its
  `settings.random_seed` and `settings.stop_at_first_feasible` are the request's
  own fields under another name.
- School timetabling's `solve` takes and returns dicts (a `TimetableProblem` in,
  a `Timetable` out) and has three more keywords, `mode`, `weights` and
  `initial_solution`. Its request, `TimetableRequest`, holds the problem as a
  `TimetableProblemModel` and those three as fields, and `solve_request`
  returns a `TimetableResultModel`.

`Solver(config).solve(problem)` and `solve_with_constraints(problem, picks)`
are the same solves one level down, without the keyword conveniences.

### Choosing the acceptance rule

Pickup and delivery, quadratic assignment, semiconductor scheduling and
vehicle routing let you choose the rule local search accepts a move by, on
`SolverConfig`. The attributes and their defaults are the same in the four:

| `acceptor_type` | Its parameters | Default |
|---|---|---|
| `LATE_ACCEPTANCE` | `late_acceptance_size` | 0 = the use case's automatic size |
| `HILL_CLIMBING` | none | |
| `ENTITY_TABU` | `entity_tabu_size`: steps a moved entity stays tabu | 7 |
| `SIMULATED_ANNEALING` | `simulated_annealing_starting_temperature` (hard, soft), cooling to 0 over the solve; `simulated_annealing_temperature_fraction` | (0, 0) = automatic: hard 0, soft = the fraction (0.01) of the start's soft level, at least 1. The soft level alone: the medium level (unassigned visits in vehicle routing, unassigned requests in pickup and delivery) does not count. Give the temperature yourself when your picks can make the soft level positive (a reward, or an offset without `clip`) or larger than the medium level's unit |
| `GREAT_DELUGE` | `great_deluge_water_level_step` (hard, soft): how much the water level, which starts at the start's score, rises per step | (0, 0): the level stays at the start's score |
| `STEP_COUNTING_HILL_CLIMBING` | `step_counting_hill_climbing_size`: the bar is reset to the current score every this many steps | 400 |
| `RECORD_TO_RECORD_TRAVEL` | `record_to_record_start_gap`, `record_to_record_end_gap`: the gap below the best score that is still accepted, as a fraction of the start's soft score | 0.02, shrinking to 0.0 over the solve (an end gap above the start gap is the gap throughout) |

The default rule is late acceptance, and simulated annealing in semiconductor
scheduling. In vehicle routing and pickup and delivery an amount you give
yourself (a starting temperature, a water level step) is on the scale the
search compares in, where the medium level counts 10^9 per unit: only the
automatic temperature leaves the medium level out. The record-to-record gaps
are fractions of that same scale, so a start that leaves work unassigned
widens them. A great deluge needs a step chosen for the instance: at (0, 0) it
takes every move that keeps the score at or above the start's and never
tightens.

### Late acceptance in stages

A late acceptance history is a trade: a short one converges fast, a long one
finds more and needs longer. In bed allocation, facility location, food packaging,
knapsack, maintenance scheduling, max-cut, quadratic assignment and sports league
scheduling the search does not stay with one length. `late_acceptance_size`
is the history of the first stage. Once that stage has converged and no new
best has come for a while, late acceptance starts over from the start's score
with the history doubled, then doubled again, as long as the next stage still
fits in the time limit (or the step limit; a stage is taken to last twice as
long as the one before). The best solution is kept throughout, so a solve is
never worse for a later stage, and a small instance uses a budget that one
short history would leave idle. A
history stops growing at 2^20 entries (16 MB per start); from there a stage
starts over with that length. A stage walks below the best for a while and
does not know the `unimproved_*` limits: one shorter than a stage ends the
solve inside it, with the best solution so far.

A use case can also draw its own macro moves less often from the first
restart on (`AcceptanceSettings::later_stage_custom_move_share`): a later stage
takes worse solutions for a long while, and moves that each make a pass over
the whole solution would take the steps of the moves that polish. Facility
location does, at a tenth of `custom_move_probability`.

Only a solve that the construction starts runs in stages. One that starts from
a solution you give (`with_initial_solution`, a request's `initial_solution`),
also one that places only a part and leaves the rest to the construction,
keeps the one history of `late_acceptance_size` and searches with the
ruin-and-recreate kicks, as every solve did before the stages: the start's
score is then no lower than where the search stands, so starting over from it
would loosen nothing and take the place of a kick. In 0.5.0 such a solve ran
in stages too, and one whose improvements come from the kicks could return its
start whatever the budget.

Whether a stage fits is judged on how far the solve is: the larger of the time
used over the time limit and the steps used over the step limit. So with a
time limit, which stage a solve reaches depends on the clock. A seeded,
step-limited solve repeats as long as the step fraction is the larger one;
give it a time limit generous enough for the steps.

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
weight of 0 contributes exactly 0 at any exponent and offset. A unit constraint
picked at weight 0 is also left out of the moves, so it costs the search no
time, and `constraint_costs()` still reports its raw cost. (A global constraint
is left out only if it declares it can be, and a constraint that school
timetabling or employee scheduling compiles into a shared group is still
evaluated with that group.)

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
two identical picks simply count twice. A further pick of a constraint without
domain parameters usually reuses the raw cost the engine computed for the first
one and only shapes it again, so it costs much less than a new constraint.
An empty list is an error, and so is an
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
  constraints its factory accepts; its defaults are the `default_enabled` rows.
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
The two steps the `solve()` functions share, the keyword overrides on a copy of the
config and the solver call, are in [`optimaru/_solve.py`](../../optimaru/_solve.py), and
what several `models.py` need (the strict model base, the common solver settings, the
checks of `with_initial_solution`) is in [`optimaru/_models.py`](../../optimaru/_models.py).

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
docs). Each use case has `extending.md` next to its guide: one new constraint taken
through that use case's own files in six steps, from a pick in Python to the
default fast path, with the steps per second measured on the way.

## How the extensions are built

[`setup.py`](../../setup.py) builds one extension per use case
(`optimaru.<use case>._core`, for every subpackage with a `_core.pyi`) from
`optimaru/<use case>/cpp/bindings.cpp` and every `cpp/src/*.cpp` except
`main.cpp`, the same sources as the core library in its `CMakeLists.txt`. `uv sync` installs the package in editable mode and
builds the extensions in place, next to the Python files. `OPTIMARU_USE_CASES`
(comma-separated) limits the build to some use cases. uv rebuilds when a
`.cpp` / `.hpp` under `optimaru/`, `setup.py`, `pyproject.toml` or
`OPTIMARU_USE_CASES` changes; `uv sync --reinstall-package optimaru` forces it.
