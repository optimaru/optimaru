# Use cases

Each directory is an independent problem built on the engine in
[`include/optimaru/`](../include/optimaru). Competing solvers (OR-Tools
CP-SAT / Routing) never live here — they are in
[`benchmarks/`](../benchmarks).

| Use case | Python package |
| --- | --- |
| [assembly-line-balancing](assembly-line-balancing) | `assembly_line_balancing` |
| [bed-allocation](bed-allocation) | `bed_allocation` |
| [bin-packing](bin-packing) | `bin_packing` |
| [conference-scheduling](conference-scheduling) | `conference_scheduling` |
| [employee-scheduling](employee-scheduling) | `es_scheduling` |
| [facility-location](facility-location) | `facility_location` |
| [flight-crew-scheduling](flight-crew-scheduling) | `flight_crew` |
| [food-packaging](food-packaging) | `food_packaging` |
| [knapsack](knapsack) | `knapsack` |
| [maintenance-scheduling](maintenance-scheduling) | `maintenance_scheduling` |
| [max-cut](max-cut) | `max_cut` |
| [meeting-scheduling](meeting-scheduling) | `meeting_scheduling` |
| [order-picking](order-picking) | `order_picking` |
| [pickup-delivery](pickup-delivery) | `pdp_routing` |
| [project-job-scheduling](project-job-scheduling) | `project_job` |
| [quadratic-assignment](quadratic-assignment) | `quadratic_assignment` |
| [school-timetabling](school-timetabling) | `st_timetabling` |
| [semiconductor-scheduling](semiconductor-scheduling) | `semiconductor_scheduling` |
| [sports-league-scheduling](sports-league-scheduling) | `sports_league` |
| [task-assigning](task-assigning) | `task_assigning` |
| [tournament-scheduling](tournament-scheduling) | `tournament_scheduling` |
| [vehicle-routing](vehicle-routing) | `vrp_routing` |

Every use case is a C++ domain on the optimaru engine with a pybind11 Python
package; they all share one constraint contract (below). Six of them
(assembly-line-balancing, bin-packing, knapsack, max-cut, quadratic-assignment
and semiconductor-scheduling) also carry a pure-Python reference scorer as
`python/tests/reference_<package>.py`, an independent oracle the C++ costs are
tested against.

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
<slug>/
├── cpp/
│   ├── CMakeLists.txt
│   ├── include/<ns>/
│   │   ├── domain.hpp                 problem facts, decision variable, Score,
│   │   │                              SolutionTraits specialization
│   │   ├── score_calculator.hpp       full (reference) score
│   │   ├── incremental_calculator.hpp tuned incremental score (default path)
│   │   ├── constraints.hpp            default constraint assembly
│   │   ├── constraints/*_cost.hpp     one plugin component per constraint
│   │   └── solver.hpp                 domain Solver: Config -> engine wiring
│   ├── src/*.cpp
│   └── test/<ns>_test.cpp, <ns>_plugin_test.cpp
└── python/
    ├── bindings.cpp                   pybind11 module <package>._core
    ├── setup.py, pyproject.toml
    ├── <package>/{__init__,models,constraints}.py, _core.pyi
    └── <package>/_constraint.py       synced copy of the shared constraint-spec
                                       base (scripts/check_constraint_base.py)
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

### Adding a move

The engine already mixes the generic moves for the variable kind: change /
swap for the assignment shape; relocate / swap / 2-opt / Or-opt / 2-opt\* tail swap
for the sequence shape. Their weights are `Solver::Config` fields (exposed to
Python as `*_move_weight`). A domain move is a
`optimaru::MoveOperator<Solution>`
([`search/operators.hpp`](../include/optimaru/search/operators.hpp)):

```cpp
class MyMove final : public optimaru::MoveOperator<MySolution> {
public:
    const char* name() const override { return "myMove"; }
    // Do ONE random move through ctx.editor (never touch the solution
    // directly), remember how to undo it, report the touched entity ids.
    bool apply(optimaru::OperatorContext<MySolution>& ctx, std::mt19937_64& rng,
               std::vector<int>& touched) override;
    void undo_last(optimaru::OperatorContext<MySolution>& ctx) override;
};
```

Register it where the use case's `Solver` builds the engine (in
`cpp/src/solver.cpp`):

```cpp
engine.add_move_operator(std::make_unique<MyMove>());
```

The engine picks among the built-in mix and every registered operator
adaptively (roulette wheel rewarded by move outcome). For large-neighbourhood
search, implement `optimaru::RuinOperator<Solution>` instead — it only
chooses which entities to ruin; the engine removes and greedily recreates
them — and register it with `engine.add_ruin_operator(...)`. Run the solver
once with `full_assert` on (`ScoreChecks::Full`) after adding a
move: it fails fast if the move leaves the incremental score stale.

### Adding or changing the construction heuristic

The engine's construction heuristic assigns every still-unassigned entity
(sequence shape: element) by cheapest insertion. Two levers:

1. **Order** — set `ConstructionSettings::difficulty` in the
   use case's engine config; entities are then placed in decreasing
   difficulty (First Fit Decreasing) instead of index order:

   ```cpp
   engineConfig.construction.difficulty =
       [&](int entity) { return demandOf(entity); };   // hardest first
   ```

2. **A domain constructor** — build a (partial) initial solution in the
   use case's `Solver` before calling the engine. The engine keeps what is
   already assigned and only fills the rest, so a domain constructor can
   place the hard part (e.g. pickup-delivery's `constructPairs` places each
   pickup together with its drop-off) and leave the remainder to the generic
   heuristic.

### Adding a hard or soft cost

A constraint is one named component; its level (hard / medium / soft), weight
and exponent are defaults the caller can override per solve, together with
`offset` / `clip`, and a caller may pick it several times (see
[Constraints: names and knobs](#constraints-names-and-knobs)).

1. **Facts** — if the cost needs new input, add it to `domain.hpp` (and to
   `models.py`'s `to_core()` / `from_core()` and `bindings.cpp`). Domain
   parameters belong to the canonical model, with field descriptions; weights,
   seeds and time limits never do.
2. **Component** — add `cpp/include/<ns>/constraints/<subject>_cost.hpp`.
   Most costs are a `UnitConstraint` (a pure function of one unit — an entity
   or a list — and its members); costs that belong to no unit (e.g.
   "unassigned count") are a `GlobalConstraint`. Positive cost = penalty, `0` =
   no violation:

   ```cpp
   struct MyRuleCost final : MyUnitConstraint {
       static constexpr double kDefaultWeight = 1.0;
       static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;
       explicit MyRuleCost(double weight = kDefaultWeight)
           : MyUnitConstraint("myRuleCost", weight, kDefaultLevel) {}
       double cost(const MySolution& s, int unit,
                   const std::vector<int>& members) const override { ... }
   };
   ```

   A reward sets `reward = true` in its constructor (raw cost `<= 0`, never
   hard); a structural constraint sets `levelFlippable = false`.
3. **Assembly** — include it in `constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set. Python sees it
   automatically under its snake_case name (`list_constraints()`,
   `constraint_info()`).
4. **Default fast path** — if it is in the default set, add the same raw cost
   to `score_calculator` and `incremental_calculator`, reading the
   component's `kDefaultWeight`; `<ns>_plugin_test` checks that the plugin
   assembly and the monolithic calculators agree bit for bit.
5. **Python spec** — add a `Constraint` subclass with `name = "my_rule_cost"`
   and `label_en` / `label_ko` / `description_en` / `description_ko` to
   `<package>/constraints.py`, plus a module-level default instance
   (`my_rule_cost = MyRuleCost()`), so callers can pick it, reweight it or flip
   its level: `solve(problem, constraints=get_constraints([...,
   MyRuleCost(weight=5, level="hard")]))`. The spec declares no defaults; they
   come from `constraint_info()`.
6. **Tests and benchmark parity** — a hand-calculated positive and zero case
   in `<ns>_plugin_test`, full-vs-incremental and monolith-vs-plugin parity,
   and the same cost in the CP-SAT model (a plugin subclassing the spec), so
   the comparison stays like for like. The CP-SAT models live in
   [external-solver](https://github.com/optimaru/external-solver)
   (`external_solver.<package>`); `benchmarks/<slug>/cpsat` only runs them
   against this checkout. School timetabling's CP-SAT model is the exception
   and stays in `benchmarks/school-timetabling/cpsat`.

## Building the Python packages standalone

`uv sync` in `<slug>/python` builds that package's `_core` extension from
`../cpp/src/*.cpp`. Each `setup.py` uses a small `build_ext` subclass that
keeps those objects inside the build's own temporary directory, so standalone
builds of several use cases can run in parallel. uv caches the editable build
by `pyproject.toml` / `setup.py` only: after deleting an in-place `_core*.so`
or changing C++ sources, re-sync with `uv sync --reinstall-package <package>`.
