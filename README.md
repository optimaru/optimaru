# optimaru

A metaheuristic optimization engine for real-world routing, scheduling,
timetabling and allocation, with 22 ready-to-use models you call from Python.

optimaru comes in a free, open-source edition, this repository, and an Enterprise
edition, the optimaru API: the same engine run as a hosted service. A program
that uses this package moves onto it with one setting, an API key
([Solving on the optimaru API](#solving-on-the-optimaru-api)).

Documentation: <https://optimaru.github.io/optimaru/> (built from [`docs/`](docs)
with MkDocs; the API reference is generated from the sources).

## Quick start

```bash
pip install optimaru
```

One package with every use case as a subpackage — `optimaru.vehicle_routing`,
`optimaru.school_timetabling`, … — compiled for Linux, macOS and Windows
(Python 3.11+, pydantic 2.11+). No C++ toolchain is needed. Every use case has
two entry points: `solve(problem, ...)` for a problem built in Python, and
`solve_request(request)` for a pydantic request (JSON in, JSON out).

```python
from optimaru import vehicle_routing as vrp
from optimaru.vehicle_routing.constraints import (
    TravelTimeCost, UnassignedVisitsCost, VehicleCapacityCost, get_constraints,
)

# Depot at index 0, then three customers.
locations = [vrp.Location(37.5665, 126.9780), vrp.Location(37.5700, 126.9820),
             vrp.Location(37.5512, 126.9882), vrp.Location(37.5133, 127.1001)]
#                    id    name  location demand ready     due        service (s)
visits = [vrp.Visit(f"C{i}", f"C{i}", i + 1, 2, 8 * 3600, 20 * 3600, 600) for i in range(3)]
vehicles = [vrp.Vehicle("V1", 6, 0, departure_time=7 * 3600)]
plan = vrp.VehicleRoutePlan("demo", locations, vehicles, visits)

config = vrp.SolverConfig()
config.spent_limit_seconds = 2
solver = vrp.Solver(config)

solver.solve(plan)                      # default constraints
print(plan.score, plan.routes())        # 0hard/0medium/-966soft {'V1': ['C0', 'C1', 'C2']}

# Pick and reweight constraints; the solver does not change.
solver.solve_with_constraints(plan, get_constraints([
    VehicleCapacityCost(),
    UnassignedVisitsCost(),
    TravelTimeCost(weight=10),
]))
print(plan.score)                       # 0hard/0medium/-9660soft
```

Every constraint takes the same knobs — `weight`, `level` (`"hard"`,
`"medium"`, `"soft"`), `exponent`, `offset` and `clip` — and may be picked
several times. `constraint_info()` on a use case lists its constraints and
their defaults. More in [`docs/pypi.md`](docs/pypi.md) and the
[notebooks](examples).

## Solving on the optimaru API

The optimaru API is the Enterprise edition of the engine: the same engine, run as a
hosted service. The body of its solve request is the Python call (the first argument and
the keywords of `solve` or `solve_request`, as JSON), so a program that uses this
package moves onto it with one setting, an API key. Give it in the environment:

```bash
export OPTIMARU_API_KEY=om_live_...
```

or in code:

```python
import optimaru

optimaru.use_api("om_live_...")
```

With a key, `solve(model, ...)` and `solve_request(request, ...)` send the problem to
the API, wait for the solve job and return what they return on this machine: a new
solved model, or the response model. The rest of the program stays as it is. Without a
key nothing is sent.

- `OPTIMARU_API_URL`, or `optimaru.use_api(key, url=...)`, names another API than
  `https://app.optimaru.com`. It must be an https URL (http is accepted for localhost).
  `optimaru.use_api()` without a key reads `OPTIMARU_API_KEY`, and
  `optimaru.use_local()` solves on this machine again, whatever the environment says.
- What cannot be sent is refused instead of being solved here without notice: a problem
  built from the compiled classes (it is solved in place, which a result that comes back
  as JSON cannot do; pass its model, `Model.from_core(problem)`, and use the model that
  comes back), the `solve(problem)` of the four use cases whose pydantic layer is a whole
  request (vehicle routing, pickup and delivery, order picking, employee scheduling: call
  `solve_request`), a callback (`on_new_best`, employee scheduling's `select`) and a
  non-finite number.
- School timetabling stays on this machine. The API serves it through an endpoint of its
  own, whose request is not this package's Python call, so with a key set it is solved here
  and says so once in a `RuntimeWarning`.
- The setting moves `solve` and `solve_request`. The compiled `Solver` class
  (`Solver(config).solve(problem)`, `solve_with_constraints`) is the engine itself and
  always runs on this machine.
- `config` travels as the `SolverConfig` fields that differ from the defaults and
  `constraints` as the picks in their order, so a solve with the same seed and a step
  limit gives the same result there as here on the same engine version. A `solve` keyword
  or a `SolverConfig` field left at its default takes the default of the engine the API
  runs; a request carries all its own fields.
- `use_api()` applies to the process that calls it. Worker processes that start fresh
  (spawn, forkserver: the default on macOS and Windows, and on Linux from Python 3.14)
  need their own call; the environment variable reaches them by itself.
- What the API refuses or fails at raises `optimaru.ApiError`: the HTTP `status`, the
  API's error `code` (`"INVALID_CREDENTIALS"`, `"PLAN_LIMIT_EXCEEDED"`,
  `"VALIDATION_ERROR"`, ...; `"JOB_FAILED"` carries the engine's own message for an input
  it refused), its `detail`, and `retry_after` when the API said how long to wait.
- A solve is a job on the API that the call waits for, from when the job is created, for
  at most twice its time limit (times its parallel starts, for a request) and ten minutes
  more; then it is cancelled and `ApiError` says so (`"JOB_TIMEOUT"`). A look at the job
  that fails for a reason that passes (the network, a 5xx, a 408, a 429) is tried again for
  a tenth of that wait, and at least a minute, but never past the wait; the read of the
  result is tried again the same way. Creating the job is not tried again, since that could solve it twice.
  Interrupting a solve (Ctrl-C) once its job exists cancels the job.
- The problem and the settings leave the machine, over https, to the API; the first
  solve sent there is logged at INFO on the `optimaru` logger. No redirect is followed,
  so the key goes to that API only. `calculate_score`, `constraint_costs` and the other
  helpers keep running here.
- A dict in what comes back (routes by vehicle, an assignment by location) has the same
  entries as here, possibly in another order: the API keeps a JSON object's keys
  unordered. Read it by key.
- The API has limits of its own: how long one job may run, how many parallel starts it
  may take, and what the plan of the key allows. Past one it answers with an `ApiError`
  that says which.
- The API runs a released engine. A `SolverConfig` field or a model field that a newer
  source checkout has and the API's engine does not is refused there by its name.

## Contributing: add a constraint or a move

Both plug into a use case without touching the engine.

| To add | You write | Guide |
| --- | --- | --- |
| A constraint (hard, medium or soft cost) | one `<Subject>Cost` component in `optimaru/<use case>/cpp/include/<ns>/constraints/`, a line in `defaultConstraints()`, a spec class in `constraints.py` | [Adding a cost](docs/extending.md#adding-a-cost) |
| A move | one `MoveOperator` with `apply()` and `undo_last()`, registered with `add_move_operator()` in the use case's `solver.cpp` | [Adding a move](docs/extending.md#adding-a-move) |
| A ruin operator or a construction heuristic | a `RuinOperator`, or a difficulty order / domain constructor | [A ruin instead of a move](docs/extending.md#a-ruin-instead-of-a-move), [Construction instead of search](docs/extending.md#construction-instead-of-search) |

[`CONTRIBUTING.md`](CONTRIBUTING.md) has the setup, the short version of both
recipes and the rules for a pull request;
[`docs/extending.md`](docs/extending.md) is the full step-by-step guide. For a
constraint, each use case also has a worked example in its own files,
`docs/use-cases/<slug>/extending.md`
([bed allocation's](docs/use-cases/bed-allocation/extending.md), for one): a
constraint changed from Python, then a new one written as a component, tested, made
cheaper per move and moved to the default fast path, with the steps per second
measured at each stage.

## Use cases

[`docs/use-cases/README.md`](docs/use-cases/README.md) lists all 22 use cases
and the constraint contract they share. Each use case's guide in
`docs/use-cases/<slug>/` has a short usage example, and `extending.md` beside it
adds a constraint step by step in that use case's files.

Routing and logistics: vehicle routing (CVRPTW), pickup & delivery (PDPTW),
order picking, facility location. Scheduling: employee shifts, flight crew,
maintenance, project-job (RCPSP), semiconductor fab, food packaging, task
assigning, meeting. Timetabling: school timetabling, conference, sports league,
tournament. Allocation and packing: bed allocation, bin packing, knapsack,
assembly-line balancing, quadratic assignment, max-cut.

```sh
uv sync                                    # editable install; builds all 22 extensions in place
uv run pytest                              # every Python test
export OPTIMARU_USE_CASES=knapsack         # or build only the use cases you work on
uv sync && uv run pytest tests/knapsack    # (uv rebuilds when the variable changes)
cmake -B build && cmake --build build -j && ctest --test-dir build   # engine + use-case C++ tests
```

## Benchmarks

[`benchmarks/README.md`](benchmarks/README.md) lists every comparison. In
short: every use case but vehicle routing is compared with OR-Tools CP-SAT on
the same instances and rescored by the use case's own evaluator; vehicle
routing is compared with the OR-Tools Routing solver on CVRPLIB Set X and
Augerat's Set A. Pickup & delivery, project-job scheduling, bin
packing, quadratic assignment and assembly-line balancing also run on public
instance sets (Li & Lim, RG30, Falkenauer, QAPLIB, SALBP).

## Features

- **Solution shapes** (`SolutionTraits<Solution>`): assignment (each entity
  takes a value) and sequence (ordered lists of elements).
- **Scoring**: `Score` (int64 hard, double soft); constraints are written as an
  incremental `Objective` (assignment and list hooks). Move evaluation stays
  incremental (O(change)).
- **Constraint plugins**: `DecomposedObjective` scores named unit and
  global components (`<Subject>Cost`). Each declares its default weight and
  level (`kDefaultWeight` / `kDefaultLevel`: hard, medium or soft); a caller
  re-states only five knobs per solve (`ConstraintAssembly`): `weight`,
  `level`, `exponent`, and `offset` / `clip`, which shift the shaping of a
  raw cost `x >= 0` as a threshold (`max(0, x - offset) ** exponent`) or as
  the literal `(x - offset) ** exponent`, per unit (each vehicle, slot, ...),
  not on the total. A constraint may be added several times
  (`add_instance()`), each instance with its own knobs, and the instances add
  up; instances that are the same component with other knobs
  (`add_repeats()`) have their raw cost computed once where the engine
  handles them the same way; a name held several times is edited per instance
  (`weight(name, instance, w)`, `drop_all(name)`), and its name alone throws.
  Rewards (raw cost <= 0) never go hard, the literal formula with an offset
  never lands on a level that counts toward the hard score (so hard 0 still
  means no violation), structural constraints keep their level, and a domain
  with a medium level declares once how it folds into the hard/soft score
  (`MediumFold`).
- **Score explanation**: `DecomposedObjective::constraint_totals()` gives the
  weighted total of each constraint, and `constraint_costs()` the raw per-unit
  costs, from which a caller re-prices an assembly without scoring again.
- **Verification**: `ScoreChecks::Full` compares every incremental score with
  a full recompute and throws `ScoreMismatchError` on a difference.
- **Construction**: first fit, first fit decreasing, cheapest insertion for
  sequences.
- **Acceptors**: hill climbing, late acceptance, entity tabu, simulated
  annealing, great deluge, step counting hill climbing, record-to-record travel.
  Late acceptance can run in stages of growing history
  (`AcceptanceSettings::history_growth`), which fits its length to the budget;
  in the stages after the first, a use case's own move operators can be drawn
  less often (`later_stage_custom_move_share`).
- **Limits** (`Limits`): wall time, stall time, steps, stall steps, target
  score; the first one reached ends the solve.
- **Moves**: change / swap (assignment); relocate / swap / 2-opt / Or-opt /
  2-opt\* tail swap / CROSS-exchange (sequence), mixed by weight in `MoveMix`.
- **Nearby selection**: `NeighborSettings::distance` biases moves toward
  k-nearest neighbours.
- **Ruin & recreate**: adaptive ALNS — random/related/worst/SISR/route ruin +
  regret-k (default k=3, nearby-restricted) recreate + polish, adaptive operator
  weights (roulette wheel by default; segmented roulette, alpha-UCB, uniform).
- **Solving**: `Solver<Solution>(settings).solve(solution, objective)`;
  `solve_multistart()` (N independent starts, best wins); `request_stop()`
  ends a running solve from another thread.
- **Custom operators**: implement `MoveOperator` / `RuinOperator`
  (`search/operators.hpp`) and register them with `Solver::add_move_operator` /
  `add_ruin_operator`; `set_acceptor_factory` swaps the acceptor. (Simulated
  annealing needs none for its starting temperature: with
  `start_temperature_fraction` the engine sizes it from the start's soft level.)

## Repository layout

The layout follows the usual pattern for a compiled Python package: one package,
`optimaru/`, with the C++ next to the Python it backs, and one `tests/` tree that
mirrors it. The engine is dependency-free, header-only C++17 and contains **no
domain code**; every use case is a subpackage that consumes it as a library.

```
optimaru/                          the Python package (pip install optimaru)
├── __init__.py                    loads each use case on first access
├── _constraint.py                 the constraint-spec base every use case shares
├── _solve.py                      the steps the use cases' solve() functions share
├── _models.py                     what the use cases' pydantic models share
├── _api.py                        solving on the optimaru API: the key, the request, the job
├── cpp/optimaru/                  the engine (header-only C++17), below
└── <use case>/                    22 subpackages, e.g. vehicle_routing/
    ├── __init__.py, models.py,    the Python API: solve, pydantic models,
    │   constraints.py, _core.pyi  constraint specs, stubs of the extension
    └── cpp/
        ├── bindings.cpp           the pybind11 module optimaru.<use case>._core
        ├── include/<ns>/          domain, calculators, constraints/, solver
        ├── src/                   the core library sources (main.cpp: demo)
        └── CMakeLists.txt         core library, demo and C++ tests
tests/                             mirrors the package
├── engine/                        engine tests (C++)
├── test_constraint.py             the constraint-spec base
├── test_api.py                    solving on the optimaru API, against a local stand-in
└── <use case>/                    pytest suite; cpp/ holds the C++ tests
docs/                              the documentation site (mkdocs.yml): the
                                   extending guide, one guide per use case
                                   (docs/use-cases/<slug>/), the PyPI page
CONTRIBUTING.md                    setup, adding a constraint or a move, PR rules
examples/                          one Jupyter notebook per use case
benchmarks/<slug>/                 the same problems against OR-Tools (CP-SAT,
                                   or the Routing solver for VRP); competing
                                   solvers live only here. cpp/ holds the use
                                   case's C++ quality bench, where it has one
benchmarks/cpsat-common/           comparison runner; checks external-solver
                                   runs against this checkout
setup.py, CMakeLists.txt           the wheel's extensions; the C++ build
```

## Engine layout

```
optimaru/cpp/optimaru/
├── optimaru.hpp                  umbrella include for consumers
├── solver.hpp                    Solver: construction, then search; multi-start
├── settings.hpp                  Settings, Limits, AcceptanceSettings, MoveMix,
│                                 NeighborSettings, RuinRecreateSettings, ...
├── solver_config.hpp             what every use case's Solver::Config shares:
│                                 its bases, their Settings, parallel starts
├── portable_random.hpp           random_index, and seeded draws that match on
│                                 every standard library
├── worker_threads.hpp            thread start-up / shutdown for parallel pools
├── model/                        the solution as the engine sees it
│   ├── solution_traits.hpp       SolutionTraits, Shape (Assignment | Sequence),
│   │                             pinning
│   ├── editor.hpp                every write, bracketed by the objective's hooks
│   ├── element_index.hpp         element -> (list, position)
│   ├── snapshot.hpp              whole and list-wise restore
│   └── neighborhoods.hpp         k-nearest-neighbour lists
├── score/                        scoring
│   ├── score.hpp                 Score, ScoreChecks, ScoreMismatchError
│   ├── levels.hpp                Level (hard / medium / soft), MediumFold
│   ├── hard_medium_soft_score.hpp  a use case's three-level Score,
│   │                             to and from the engine's Score
│   ├── objective.hpp             Objective: the incremental scoring contract
│   ├── decomposed_objective.hpp  per-unit and global constraint plugins
│   └── constraint_assembly.hpp   default constraint set: weight / level /
│                                 exponent / offset / clip / drop by name,
│                                 repeated instances, info() catalog
├── construct/                    the initial solution: first fit (decreasing),
│                                 cheapest insertion
├── accept/                       Acceptor and one file per rule: hill climbing,
│                                 late acceptance, entity tabu, simulated
│                                 annealing, great deluge, step counting,
│                                 record-to-record travel; history_stages, late
│                                 acceptance in stages of growing history
├── select/                       adaptive operator selection: MoveOutcome,
│                                 roulette wheel, segmented roulette, alpha-UCB,
│                                 uniform
├── stop/                         when a solve ends (StopCondition) and the
│                                 throttle long-running moves consult it through
├── search/                       the step loop (local_search) and the plugin
│                                 points for custom operators (MoveOperator /
│                                 RuinOperator)
├── moves/                        every move the step loop draws: the sequence
│                                 moves (list_move) and their previews, descent,
│                                 ejection chain, iterated local search, route
│                                 elimination
├── ruin_recreate/                one destroy-and-repair unit: the ruin operators
│                                 (random, related, worst, SISR, route), the
│                                 greedy / regret-k recreate, and
│                                 ruin_recreate_move, which applies them as an
│                                 ordinary move (the stagnation kick uses the
│                                 same parts)
├── python/                       pybind11 helpers: constraint specs ->
│                                 ConstraintAssembly, constraint_info(),
│                                 constraint_costs(), names, the level guard,
│                                 the Score class
└── testing/                      for the C++ tests: check(), and the
                                  preview-vs-applied-score harness for a use
                                  case's objective tests
```

`optimaru::` is one flat namespace apart from the `optimaru::testing` and
`optimaru::python` helpers (and small `detail` helper namespaces);
the directories organize the headers, the namespaces don't. Functions and
members are snake_case, types PascalCase.

## Tests

`tests/engine/engine_test.cpp` verifies every feature against brute force on two toy
domains (task assignment, TSP): six of the seven acceptors (all but
record-to-record travel), all five limits, FFD ordering, multi-start
(and its exception safety), `ScoreChecks::Full` catching a deliberately
corrupted objective, list moves under `ScoreChecks::Full`, adaptive operator-weight
convergence, LNS escaping a local optimum that traps hill climbing, and the
nearby+LNS quality win on 100-city random TSP. `tests/engine/constraint_level_test.cpp`
covers the constraint contract: levels and both medium folds, the knob guards
(rewards, structural levels, the literal offset kept off the hard score,
weight, exponent, offset and clip checks), the
catalog, exponent and offset / clip shaping, repeated instances,
incremental-vs-recompute and preview parity with a medium level and with
repeated and shifted components, and the score range (a saturating hard total,
out-of-range contributions).

```sh
cmake -B build -DOPTIMARU_USE_CASES=none && cmake --build build && ctest --test-dir build
```

`OPTIMARU_USE_CASES` takes `all` (the default), `none` or a list such as
`knapsack,vehicle_routing` (commas, spaces or `;` between the names); each use case adds its core library, demo
and the C++ tests in `tests/<use case>/cpp/`, built under `build/<use case>/`. Four use cases
(bed allocation, conference scheduling, meeting scheduling, school timetabling) also build a
quality bench, `<ns>_quality_bench`, from `benchmarks/<slug>/cpp/`; it solves instance files
that Python writes ([benchmarks/README.md](benchmarks/README.md)).

## Building and releasing

The distribution is built from this repository by [`setup.py`](setup.py): one
extension per use case, from its `cpp/bindings.cpp` and every `cpp/src/*.cpp`
except `main.cpp` (the demo).
`OPTIMARU_USE_CASES=knapsack,vehicle_routing` builds only those extensions (CI
tests one use case per job that way); a release builds all of them.

**Releasing:** bump `version` in [`pyproject.toml`](pyproject.toml) in a PR and
merge it. On the push to `main`, if there is no `v<version>` tag yet,
[`wheels.yml`](.github/workflows/wheels.yml) builds the wheels and sdist, uploads
them to PyPI, then creates the `v<version>` tag and GitHub release. An unchanged
version neither builds nor publishes on `main`; PRs always build.

## License

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE) for attribution.

The engine began as a C++ port of parts of
[Timefold Solver](https://github.com/TimefoldAI/timefold-solver) and has since
been restructured with its own layout, names and API; portions still derive
from it. Several use cases derive from examples in
[Timefold Quickstarts](https://github.com/TimefoldAI/timefold-quickstarts).
Both are Apache-2.0; [NOTICE](NOTICE) lists exactly which parts derive from
which. When you redistribute optimaru or a derivative of it, include both
[LICENSE](LICENSE) and [NOTICE](NOTICE). optimaru is not affiliated with or
endorsed by Timefold BV.

The musllinux and Windows wheels on PyPI also bundle the C++ runtime libraries
their extensions load (libstdc++ and libgcc, GPL-3.0-or-later with the GCC
Runtime Library Exception; the Microsoft Visual C++ runtime).
[THIRD-PARTY-LICENSES](THIRD-PARTY-LICENSES) carries their terms and ships in
every wheel; pass it on with those wheels.
