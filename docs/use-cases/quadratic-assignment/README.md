# Quadratic assignment

The quadratic assignment problem on this repository's C++ engine
(`optimaru/cpp/optimaru/`): place `n` facilities on `n` locations, one each,
minimizing the flow between every pair of facilities times the distance between
their locations, plus an optional direct facility-location cost, with pinned and
forbidden placements and facility-demand / location-capacity compatibility as
hard constraints. The objective is the Koopmans-Beckmann (1957) formulation of the quadratic
assignment problem.
The exact CP-SAT reference is external-solver's `external_solver.quadratic_assignment`;
[`benchmarks/quadratic-assignment/cpsat`](../../../benchmarks/quadratic-assignment/cpsat)
holds the engine vs CP-SAT comparison.

## Usage

```sh
OPTIMARU_USE_CASES=quadratic_assignment uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/quadratic_assignment
```

```python
from optimaru import quadratic_assignment as qap
from optimaru.quadratic_assignment.models import QuadraticAssignmentProblemModel

problem = QuadraticAssignmentProblemModel(
    facilities=["F1", "F2"], locations=["L1", "L2"],
    flows=[[0, 3], [2, 0]], distances=[[0, 4], [5, 0]],
)
problem = qap.solve(problem, time_limit_seconds=1.0, random_seed=42)   # a new, solved model
print(problem.score_hard, problem.score_soft)                         # 0 -22.0
print(problem.assignment, problem.total_cost)

core = problem.to_core()                                             # or the engine object
print(qap.solve(core, time_limit_seconds=1.0, random_seed=42).score)   # 0hard/-22soft
print(core.assignment(), core.total_cost())                          # per location: the facility index
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config`
(`quadratic_assignment.SolverConfig`) and `constraints`:

```python
from optimaru.quadratic_assignment.constraints import FlowDistanceCost, PinnedAssignmentsCost, get_constraints, score

qap.constraint_info()      # [ConstraintInfo(name='pinned_assignments_cost', default_level='hard', ...), ...]
problem = qap.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:-1], FlowDistanceCost(weight=2)])
raw = qap.constraint_costs(problem)                  # raw costs, re-priced in pure Python:
score(raw), score(raw, [PinnedAssignmentsCost(level="soft"), *get_constraints()[1:]])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping (clip=True: only the flow cost beyond 20 counts for the second pick).
score(raw, [FlowDistanceCost(), FlowDistanceCost(weight=10, offset=20)])
```

## Domain (assignment shape)

- `Facility.location` — the only decision variable: an index into `locations`,
  `-1` = unassigned. A `DecomposedObjective` unit is one location; its
  members are the facilities standing there.
- The solver keeps the facilities a **permutation** of the locations: it starts
  from the identity (facility `i` at location `i`) or a warm-start bijection and
  only uses swap moves (two facilities exchange their locations). Change moves,
  ruin-and-recreate, descent and value recommenders would place facilities one at
  a time and are neither enabled nor exposed; every solve checks the result is a
  bijection. `-1` appears only in a partial state a caller wrote (it contributes
  nothing; an empty pinned location still counts as a broken pin).
- Facts: `flows` (facility x facility), `distances` (location x location),
  optional `assignment_costs` (facility x location), `fixed_assignments`
  (`FixedAssignment(location, facility)`, input order; several locations may
  name one facility), `forbidden_assignments` (rows kept as given; a repeated
  pair counts once), per facility `demand` (None = not listed = 0), per location
  `capacity` (None = no limit). Signs are not validated: negative flows,
  distances or direct costs are scored as given.
- `warm_start` — start from the assignment, which must then be a full bijection
  (else `ValueError`); a plain solve ignores the input assignment's placements and
  starts from the identity. Every solve resets it. Even without `warm_start`, a
  model's assignment must be representable (known ids, no facility twice):
  `to_core()` raises `ValueError` otherwise, so `solve(model)` does too.

## Constraints

The score is the engine's `Score` type as it is, with no mapping.

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `pinned_assignments_cost` | `PinnedAssignmentsCost` | hard | 1 | per location: 1 when it is fixed to a facility that is not standing there (an empty location included), else 0 |
| `forbidden_assignments_cost` | `ForbiddenAssignmentsCost` | hard | 1 | per location: its facilities that a forbidden row forbids there (0 or 1 in a permutation; a repeated row counts once) |
| `location_capacity_cost` | `LocationCapacityCost` | hard | 1 | per location: `max(0, demand of its facility - capacity)` for a location with a capacity, else 0 |
| `assignment_cost` | `AssignmentCost` | soft | 1 | one total: `sum assignment_costs[f][location of f]` (0 without a matrix) |
| `flow_distance_cost` | `FlowDistanceCost` | soft | 1 | one total: `sum over facilities a, b of flows[a][b] * distances[loc a][loc b]` (the diagonal `a == b` included) |

The three side constraints are `UnitConstraint`s over the locations (the exponent
shapes each location's cost); the two objectives are `GlobalConstraint`s on the
engine's member-change fast path (the exponent shapes the total). Every
constraint is flippable (`level="hard"` / `level="soft"`, or `Level.HARD` /
`Level.SOFT`; `level="medium"` raises `ValueError`, the domain has no medium level), none
is a reward and all five are scored by default (`default_enabled`). One limit depends on
the data: the matrices are not validated >= 0, so when `assignment_costs` holds a negative
entry the assignment cost's total can be negative, and so can the flow cost's when a flow
or a distance is negative. On such data that objective cannot be hard, since a negative
hard total would cancel a broken pin and an infeasible layout could score hard 0: every
path refuses it with `ValueError: constraint 'assignment_cost' can have a negative raw
cost on this problem (a bonus) and cannot be hard` (`constraint_costs()` marks its entry
`"signed": True`). On soft the negative total stays a bonus.
`quadratic_assignment.constraint_info()` reports exactly this table from the C++
components (`kDefaultLevel` / `kDefaultWeight`).

### Offset, clip and repeated picks

Besides `weight`, `level` and `exponent`, every constraint takes `offset` and `clip`
(keyword-only), and may be picked several times. The three side constraints shape each location they evaluate, the
two objectives their total `x`: `sign(x) * max(0, |x| - offset) ** exponent`
(`clip=True`, the default) or `(x - offset) ** exponent`, sign-preserving (`clip=False`,
which charges a location of raw 0 too and rewards one below the offset); `offset=0` is
the plain shaping. The side constraints are scoped: `pinned_assignments_cost` evaluates
the fixed locations, `forbidden_assignments_cost` the locations named by a forbidden row
and `location_capacity_cost` the locations with a capacity, so a literal offset charges
exactly those (`constraint_costs()` reports them as `"units"`). Both formulas apply per
evaluated location (or to the one total), not to the sum over the locations.
`clip=False` with an offset is refused on hard (the three side constraints by default),
because hard 0 must mean no violation: a location with slack would otherwise earn a hard
reward that cancels another location's violation. Each pick is its own instance with its
own knobs, its level included, and the instances add up:

```python
problem = qap.solve(problem, constraints=[
    *get_constraints()[:-1],
    FlowDistanceCost(),                           # the flow cost
    FlowDistanceCost(weight=10, offset=1000),     # + 10 x (what exceeds a budget of 1000)
])
```

`qap.constraint_costs(problem, picks)` scores a pick list and keys the second pick
`flow_distance_cost#2`; every entry reports its `offset`, `clip` and the `level` in effect.
The CP-SAT reference takes the same picks (integral offsets; on an objective only while
its data cannot make the total negative).

### Python API

The package is the C++ engine with pybind11 bindings, like the other C++ use cases.

- **Names**: `pinned_assignments_cost`, `forbidden_assignments_cost`,
  `location_capacity_cost`, `assignment_cost`, `flow_distance_cost`, with the spec
  classes `PinnedAssignmentsCost`, `ForbiddenAssignmentsCost`, `LocationCapacityCost`,
  `AssignmentCost` and `FlowDistanceCost` and a module-level default instance of each.
  The model fields are `fixed_assignments`, `forbidden_assignments`,
  `location_capacities` and `assignment_costs`. The specs carry `label_en` /
  `label_ko` / `description_en` / `description_ko`; the defaults live in C++ only.
- **Scoring granularity**: the engine shapes a unit constraint per location and a
  global constraint's total. Hard contributions are rounded per location, so a
  fractional hard weight or an exponent != 1 on `location_capacity_cost` rounds each
  location's weighted, shaped value (two broken pins at weight 0.5: `-2`; costs
  `[2, 2]` at exponent 0.5: `-2`).
- `constraint_costs()` returns the standard dict
  `{name: {"costs", "weight", "exponent", "global", "level", "reward",
  "level_flippable", "name", "label", ...}}`; `"costs"` lists the side constraints per
  location in location order, zeros included, and one total for the objectives.
- An assignment that puts one facility on two locations, or names an unknown id,
  cannot be represented: `to_core()` raises `ValueError`, for a plain `solve(model)`
  without `warm_start` too. On a model, `calculate_score` / `constraint_costs` need
  every location (`ValueError` naming the missing ones), while the engine object
  scores a partial state.
- **`SolverConfig`** is the engine's (`spent_limit_seconds`,
  `unimproved_spent_limit_seconds`, `step_count_limit`,
  `unimproved_step_count_limit`, `random_seed`, `parallel_solver_count`,
  `acceptor_type` and the parameters of each rule, `late_acceptance_size`,
  `full_assert`, `best_score_limit`, `on_new_best`), set by attribute. Zero budget
  = `spent_limit_seconds = 0` (`step_count_limit = 0` means "no limit"); a bounded
  search = `step_count_limit = N`. Without a time limit a solve runs the 30 s
  budget. `on_new_best(step, score)` is called on each new best. Parallel starts are
  the engine's multi-start: every worker starts from the same permutation with
  seed + i.
- **Acceptors.** `LATE_ACCEPTANCE` (the default) and the others work on swaps
  only, and no move perturbs a permutation (ruin-and-recreate would break the
  bijection). One late acceptance history stops improving at a 2-swap local
  optimum, so the default runs in stages: each time the search converges it
  starts over from the start's score with the history doubled, as long as the
  next stage fits in the budget
  ([Late acceptance in stages](../README.md#late-acceptance-in-stages)). A
  solve that starts from a given assignment (a warm start from a returned
  solution) has no stages, since there is no worse score to start over from;
  from a local optimum it rarely improves. The other ways
  out are independent starts (`parallel_solver_count`),
  `RECORD_TO_RECORD_TRAVEL`, or `SIMULATED_ANNEALING`, whose starting
  temperature is automatic: `simulated_annealing_temperature_fraction`
  (0.01) × |start soft|, or `simulated_annealing_starting_temperature` set
  explicitly; it cools to 0 over the time limit, or over `step_count_limit` when
  that is further along (the larger of the step and time fractions). A seeded,
  step-limited run repeats only while the step fraction leads, so give it a
  time limit generous enough for the step budget; once the clock leads, the
  run depends on timing. `ENTITY_TABU` accepts only non-worsening swaps (a hill
  climber), and `GREAT_DELUGE` keeps the start score as its water level until
  `great_deluge_water_level_step` gives it a rise per step. The parameters of
  every rule are listed under
  [Choosing the acceptance rule](../README.md#choosing-the-acceptance-rule).
- `solve()` returns the solved problem: the engine object it was given, solved in place (its `score` is the engine `Score`: `.hard`, `.soft`, `is_feasible()`), or, for a model, a new model with the result and `score_hard` / `score_soft` (the model passed in is not changed).
  `calculate_score(problem, constraints)` returns the engine `Score` for the
  defaults and a `constraints.Score(hard, soft)` named tuple for a selection.
  Use `constraints.get_constraints()` / `constraints.score()` for selections. With `constraints=...` a solve is single-start
  (`parallel_solver_count` is ignored), as in every C++ package.
- `QuadraticAssignmentProblemModel.to_core()` / `from_core()` convert to and from
  the real engine object. The engine object's `score` is None until it is scored
  (`calculate_score` or a solve); `from_core()` fills `score_hard` / `score_soft`
  only from a scored core (`total_cost` whenever the assignment is a bijection).
- The CP-SAT reference counts a repeated forbidden row once, like the engine.

## C++

- `optimaru/quadratic_assignment/cpp/include/qap/domain.hpp` — domain + `optimaru::SolutionTraits` specialization
- `optimaru/quadratic_assignment/cpp/include/qap/constraints/` — one component per constraint:
  `pinned_assignments_cost.hpp`, `forbidden_assignments_cost.hpp`,
  `location_capacity_cost.hpp`, `assignment_cost.hpp`, `flow_distance_cost.hpp`
  (+ `common.hpp`: the aliases, the `prepare()` guard and the member-change global
  base); `optimaru/quadratic_assignment/cpp/include/qap/constraints.hpp` assembles the default set
  (`defaultConstraints()`, also the catalog order)
- `score_calculator` — `prepare()` (validation + flat matrices, the deduplicated
  forbidden mask, the pin lookups) and the full recompute (the reference)
- `incremental_calculator` — O(n) per moved facility (the flow term), O(1) for the rest
- `solver` — engine wrapper (identity or warm-start permutation + Late Acceptance over
  swap moves; multi-start through the engine; the bijection asserted after every solve)
- `optimaru/quadratic_assignment/cpp/src/main.cpp` — the web demo's plant layout and a random 25-facility instance

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=quadratic_assignment
cmake --build build -j4
ctest --test-dir build --output-on-failure --no-tests=error
./build/quadratic_assignment/qap_demo 2
```

`qap_test`: hand-calculated scores, a positive and a zero case
per constraint plus the edge cases (diagonal flow, repeated forbidden rows, a
facility pinned twice, missing demand / capacity, negative costs, an empty pinned
location), `prepare()` validation, incremental == full recompute (score and every
constraint's raw total, each constraint non-zero on some step) over random swap /
unassign / change sequences, and the solver (zero budget keeps the start, warm-start
rules, n = 0 / 1, single / parallel / ScoreChecks::Full solves stay bijections, brute-force
optima, determinism). `qap_plugin_test`: default assembly == monolith bit for bit,
plugin incremental == full recompute with accepted and undone moves (per-constraint
raw costs, each constraint non-zero on some step), partial assemblies, overrides
(unit exponent per location, global exponent on the total, per-location hard
rounding), the cost layout, ScoreChecks::Full plugin solves, raw costs by hand, the
catalog, and repeated / shifted components (a hand-calculated repeat, threshold and
literal offsets by hand, the scoped side constraints charging only their locations,
the literal formula refused on hard at reset and kept on soft, incremental == a fresh
recompute over a walk with accepted and undone moves, a ScoreChecks::Full solve).

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Its `assignment` must be a full bijection from locations to facilities, matched
by location id: partial assignments, unknown ids and repeated facilities raise
`ValueError`. With a zero budget (`spent_limit_seconds = 0`) the seed comes back
unchanged, even one that breaks a pin (pins are a penalized hard constraint).

```python
from optimaru.quadratic_assignment import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
warm = qap.solve(warm, time_limit_seconds=1.0, random_seed=42)
print(warm.assignment, warm.total_cost)
```

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the engine is
`optimaru/quadratic_assignment/cpp/include/qap/solver.hpp` + `optimaru/quadratic_assignment/cpp/src/solver.cpp`.

**Adding a move.** The engine runs swap moves only (`change_weight = 0`,
`swap_weight = 1` in `optimaru/quadratic_assignment/cpp/src/solver.cpp`). A new move must keep the
permutation (a 3-cycle rotation, a best-partner swap): an
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` in
`optimaru/quadratic_assignment/cpp/src/solver.cpp`, run under ScoreChecks::Full. Registered operators do not reach the
engine's multi-start threads, so a solver that registers one runs its own worker
threads (see facility-location's `CloseFacilityMove`).

**Changing the construction heuristic.** The start is the identity (or the
warm-start bijection), laid out in `start()` in `optimaru/quadratic_assignment/cpp/src/solver.cpp`; the engine's
construction heuristic finds nothing to place. A different start must still be a
full bijection.

**Adding a hard or soft cost.** [Adding a constraint, step by step](extending.md) takes one new
constraint through this use case's files in six steps: changed from Python, written as a
component, tested, made cheaper per move, moved to the default fast path and measured.

## Benchmark

[`benchmarks/quadratic-assignment`](../../../benchmarks/quadratic-assignment) compares this
use case with OR-Tools CP-SAT on generated instances of up to 25 facilities, rescored
by the same evaluator, and runs the QAPLIB instances below.

### QAPLIB benchmark

`benchmarks/quadratic-assignment/bench/run_bench.py` downloads the QAPLIB
instance and solution archives, verifies both SHA-256 checksums, then re-scores every result with this
package's `constraint_costs()` (`flow_distance_cost`). Downloaded files stay in
`benchmarks/quadratic-assignment/bench/.cache/` and are not committed.

```sh
cd benchmarks/quadratic-assignment
uv sync --project cpsat --all-groups
uv run --project cpsat python -m bench.run_bench --seconds 60
# Select more instances; CP-SAT is optional because its reference QAP model is O(n^4).
uv run --project cpsat python -m bench.run_bench \
  --instance nug30 --instance lipa40a --exact-max-size 32 --seconds 60
```

The report shows the raw objective and gap to the QAPLIB best-known solution.
That best-known value is what the instance's QAPLIB reference permutation
scores under the same evaluator, read as the facility of each location or as
the location of each facility, whichever the solution file uses (a mismatch
with the value the file states stops the run, bar one recorded erratum); an
instance without a solution file in the archive has no gap. `--all` runs the 33
instances with 30 to 50 facilities.
The checked-in local snapshot is [`bench/RESULTS.md`](../../../benchmarks/quadratic-assignment/bench/RESULTS.md).
