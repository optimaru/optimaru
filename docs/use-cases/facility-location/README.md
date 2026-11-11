# Facility location

Derived from the Timefold Quickstarts `facility-location` example
(Apache-2.0; see the repository [NOTICE](../../../NOTICE)),
built on this repository's C++ engine (`optimaru/cpp/optimaru/`).
The classic FLP: pick one facility for every consumer.

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

`solve` returns the problem it was given, solved in place; the score is its `score` (`.hard`, `.soft`). It takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`facility_location.SolverConfig`) and `constraints` (see below). The C++ side builds and tests with:

```sh
cmake -B build -DOPTIMARU_USE_CASES=facility_location && cmake --build build -j
ctest --test-dir build
```

## Domain (assignment shape)

- `Consumer.facility` — the only decision variable. An index into `facilities`,
  `-1` = unassigned.
- `Facility` — a plain problem fact; the score calculator maintains its aggregates
  (assigned demand, consumer count).
- Distance = `ceil(Euclidean degree distance x 111 km)` (`Location.distance_to`).

## Constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost per facility (0 = no violation) |
|---|---|---|---|---|
| `facility_capacity_cost` | `FacilityCapacityCost` | hard | 1 | `max(0, assigned demand - capacity)` while used |
| `facility_setup_cost` | `FacilitySetupCost` | soft | 2 | its setup cost while it serves at least one consumer |
| `distance_from_facility_cost` | `DistanceFromFacilityCost` | soft | 6 | summed consumer-to-facility distance (m) |

Every constraint is flippable (`level="hard"` / `level="soft"`), none is a reward and all
three are scored by default (`default_enabled`);
`facility_location.constraint_info()` reports exactly this table from the C++
components (`kDefaultLevel` / `kDefaultWeight`).

The problem carries no weights: they come only from constraint selections, e.g.
`constraints=[FacilityCapacityCost(), FacilitySetupCost(), DistanceFromFacilityCost(weight=5)]`
to weigh distance 5 instead of 6.

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
- `solver` — engine wrapper (construction, late acceptance in stages, the macro moves
  `CloseFacilityMove` and `SwapFacilityMove`, adaptive ruin & recreate)
- `optimaru/facility_location/cpp/src/main.cpp` — demo instance
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

## Search

The construction gives every consumer its best facility one by one, which opens many
facilities; late acceptance then works with the engine's change / swap moves, its
ruin-and-recreate kicks, and two macro moves of the domain
(`optimaru/facility_location/cpp/src/solver.cpp`). They exist because
`facility_setup_cost` is a step function: a facility stops costing only when its last
consumer leaves, and starts costing with its first.

- **`CloseFacilityMove`** (`close_facility_moves`) empties one used facility in one move:
  its consumers go to the nearest facilities with room, or all together to one other site.
- **`SwapFacilityMove`** (`swap_facility_moves`) opens a closed facility and closes the one
  that serves its surroundings. The closed one's consumers go to their nearest facility
  with room, and then each facility with room takes the consumers that are nearer to it
  than to where they are. Without this move the search keeps the facilities it first
  settles on and only improves who goes where.

A solve that starts from nothing runs
[late acceptance in stages](../README.md#late-acceptance-in-stages), and from the first
restart on it draws the two macro moves on a tenth of the steps `custom_move_probability`
allows: a later stage takes worse solutions for a long while, and the macro moves, which
each make a pass over every consumer, would take the steps that settle who goes where. A
solve from a given assignment has no stages and keeps the full gate.

Measured on the six instances of
[`benchmarks/facility-location`](../../../benchmarks/facility-location) (30 s, three
seeds, one thread), as the mean cost above OR-Tools CP-SAT's at 30 s on 8 workers:

| search | mean cost above CP-SAT's |
| --- | ---: |
| 0.5.0: no swaps, one history | 1.91% |
| stages | 1.71% |
| swaps and stages, the full gate throughout | 0.51% |
| swaps and stages, a tenth of the gate from the first restart on (the default) | 0.07% |

A short solve gains too: after 50,000 steps the mean cost of a 200-consumer instance over
8 seeds is 3.10 million, against 3.22 million in 0.5.0. From a given assignment the mean
cost above CP-SAT's is 0.15% to 0.80% with the swaps and 2.40% to 3.00% without, on the 60-
and the 200-consumer instance (36 seeds, 300,000 steps, from a rough assignment and from a
solved 80% of the consumers). What the search still cannot do is open a facility without
closing another: a solve from a plan that needs one more facility for its new consumers
keeps the plan's facilities.

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
(weights in `Solver::Config`). `CloseFacilityMove` (empties a facility by moving all its consumers) and `SwapFacilityMove` (opens one facility and closes another) are defined and registered in `optimaru/facility_location/cpp/src/solver.cpp` (`runEngine`) — use them as the template. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `optimaru/facility_location/cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/facility-location`](../../../benchmarks/facility-location) compares this use case with OR-Tools CP-SAT on the same instances.
