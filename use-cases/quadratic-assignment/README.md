# Quadratic assignment

The quadratic assignment problem on this repository's C++ engine
(`include/optimaru/`): place `n` facilities on `n` locations, one each,
minimizing the flow between every pair of facilities times the distance between
their locations, plus an optional direct facility-location cost, with pinned and
forbidden placements and facility-demand / location-capacity compatibility as
hard constraints. The model is aligned with the quadratic-assignment examples
listed in the
[IBM CP Optimizer model library](https://www.ibm.com/docs/en/icos/22.2.0?topic=programming-where-find-cp-examples)
and Gurobi's public quadratic-assignment modeling examples.
[`benchmarks/quadratic-assignment/cpsat`](../../benchmarks/quadratic-assignment/cpsat)
holds the exact CP-SAT reference.

## Domain (BASIC variable)

- `Facility.location` — the only planning variable: an index into `locations`,
  `-1` = unassigned. A `DecomposedScoreCalculator` unit is one location; its
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

## Constraints (HardSoftScore — the engine score type as-is, no mapping)

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
constraint is flippable (`level="hard"` / `level="soft"`), none is a reward and all
five are scored by default (`default_enabled`); `quadratic_assignment.constraint_info()`
reports exactly this table from the C++ components (`kDefaultLevel` /
`kDefaultWeight`).

### 0.2.0

The package was a pure-Python reference (simulated annealing over
`constraint_costs()`); it is now the C++ engine with pybind11 bindings, like the
other C++ use cases.

- **Names** (no aliases): `fixed_assignments` -> `pinned_assignments_cost`,
  `forbidden_assignments` -> `forbidden_assignments_cost`, `location_capacities` ->
  `location_capacity_cost`, `minimize_assignment_cost` -> `assignment_cost`,
  `minimize_flow_distance` -> `flow_distance_cost`; the spec classes and
  module-level instances follow (`FixedAssignments` -> `PinnedAssignmentsCost`,
  `ForbiddenAssignments` -> `ForbiddenAssignmentsCost`, `LocationCapacities` ->
  `LocationCapacityCost`, `MinimizeAssignmentCost` -> `AssignmentCost`,
  `MinimizeFlowDistance` -> `FlowDistanceCost`). The model fields
  `fixed_assignments`, `forbidden_assignments`, `location_capacities` and
  `assignment_costs` keep their names. The specs carry `label_en` / `label_ko` /
  `description_en` / `description_ko` instead of `label`, and no `default_hard` /
  `default_weight` (the defaults live in C++ only).
- **Raw costs and the default score are unchanged** (each constraint's total for
  every complete assignment, `total_cost` included). Changed by design:
  - **exponent granularity: unchanged here.** The engine shapes a unit
    constraint per unit and a global constraint's total; the per-location lists
    hold exactly the non-zero terms 0.1.5 shaped (plus zeros) and the two
    objectives were one total already, so every exponent scores as in 0.1.5 (at
    the soft level; see the rounding note);
  - hard contributions are rounded per location (0.1.5 rounded the hard total
    once): different only when a hard constraint's weighted, shaped per-location
    value is fractional — a fractional hard weight (two broken pins at weight 0.5:
    `-2`, was `-1`), or an exponent != 1 on `location_capacity_cost` (costs
    `[2, 2]` at exponent 0.5: `-2`, was `-3`);
  - `constraint_costs()` returns the standard dict
    `{name: {"costs", "weight", "exponent", "hard", "global", "level", "reward",
    "level_flippable", "name", "label"}}`; `"costs"` lists the side constraints per
    location in location order, zeros included (0.1.5: `fixed_assignments` per fixed
    location, `forbidden_assignments` per assignment entry), and one total for the
    objectives;
  - an assignment that puts one facility on two locations, or names an unknown
    id, cannot be represented: `to_core()` raises `ValueError` (0.1.5 scored a
    facility on two locations, and an unknown location key as 0). This holds for
    a plain `solve(model)` without `warm_start` too (0.1.5 ignored the assignment
    there); on a model,
    `calculate_score` / `constraint_costs` need every location (`ValueError`
    naming the missing ones; 0.1.5 raised `KeyError`), while the engine object
    scores a partial state;
  - a constraint picked twice raises `ValueError`.
- **`SolverConfig`** is the engine's (`spent_limit_seconds`,
  `unimproved_spent_limit_seconds`, `step_count_limit`,
  `unimproved_step_count_limit`, `random_seed`, `parallel_solver_count`,
  `acceptor_type`, `late_acceptance_size`, `entity_tabu_size`, `full_assert`,
  `best_score_limit`, `on_new_best`), set by attribute. The simulated-annealing
  knobs `max_iterations`, `stagnation_limit`, `temperature`, `cooling_rate`, the
  keyword constructor and the `anneal` / `configure` helpers are gone. Zero budget
  = `spent_limit_seconds = 0` (`step_count_limit = 0` means "no limit"); a bounded
  search = `step_count_limit = N`. Without a time limit a solve now runs the 30 s
  budget (0.1.5 stopped after 50 000 iterations). `on_new_best(step, score)`
  (0.1.5: `on_new_best(state)`). Parallel starts are the engine's multi-start:
  every worker starts from the same permutation with seed + i (0.1.5 perturbed the
  start with 3k swaps and ran one chain).
- **Acceptors.** `LATE_ACCEPTANCE` (the default) and the others work on swaps
  only, and no move perturbs a permutation (ruin-and-recreate would break the
  bijection). So late acceptance stops improving at a 2-swap local optimum: most
  of a long budget can be idle, and re-solving a returned solution rarely
  improves it. Use the `unimproved_*` limits to stop early, independent starts
  (`parallel_solver_count`), `RECORD_TO_RECORD_TRAVEL`, or `SIMULATED_ANNEALING`,
  whose starting temperature is automatic: `simulated_annealing_temperature_fraction`
  (0.01) × |start soft|, or `simulated_annealing_starting_temperature` set
  explicitly; it cools to 0 over the time limit, or over `step_count_limit` when
  that is further along (the larger of the step and time fractions). A seeded,
  step-limited run repeats only while the step fraction leads, so give it a
  time limit generous enough for the step budget; once the clock leads, the
  run depends on timing. `ENTITY_TABU` accepts only non-worsening swaps (a hill
  climber), and `GREAT_DELUGE` keeps the start score as its water level.
- `solve()` returns the engine `Score` (`.hard`, `.soft`, `is_feasible()`; 0.1.5:
  a tuple) and takes the engine object or the model (written back in place).
  `calculate_score(problem, constraints)` returns the engine `Score` for the
  defaults and a `constraints.Score(hard, soft)` named tuple for a selection.
  `DEFAULTS` and the top-level `score` are gone (use `constraints.get_constraints()`
  / `constraints.score()`). With `constraints=...` a solve is single-start
  (`parallel_solver_count` is ignored), as in every C++ package.
- `QuadraticAssignmentProblemModel.to_core()` / `from_core()` convert to and from
  the real engine object (0.1.5: model copies); the JSON input/output schema is
  unchanged. The engine object's `score` is None until it is scored
  (`calculate_score` or a solve); `from_core()` fills `score_hard` / `score_soft`
  only from a scored core (`total_cost` whenever the assignment is a bijection).
- The CP-SAT reference counts a repeated forbidden row once, like the engine
  (0.1.5's CP-SAT model counted it twice).

## cpp/

- `include/qap/domain.hpp` — domain + `optimaru::SolutionDescriptor` specialization
- `include/qap/constraints/` — one component per constraint:
  `pinned_assignments_cost.hpp`, `forbidden_assignments_cost.hpp`,
  `location_capacity_cost.hpp`, `assignment_cost.hpp`, `flow_distance_cost.hpp`
  (+ `common.hpp`: the aliases, the `prepare()` guard and the member-change global
  base); `include/qap/constraints.hpp` assembles the default set
  (`defaultConstraints()`, also the catalog order)
- `score_calculator` — `prepare()` (validation + flat matrices, the deduplicated
  forbidden mask, the pin lookups) and the full recompute (the reference)
- `incremental_calculator` — O(n) per moved facility (the flow term), O(1) for the rest
- `solver` — engine wrapper (identity or warm-start permutation + Late Acceptance over
  swap moves; multi-start through the engine; the bijection asserted after every solve)
- `src/main.cpp` — the web demo's plant layout and a random 25-facility instance

```bash
cmake -S use-cases/quadratic-assignment/cpp -B build/quadratic-assignment -DCMAKE_BUILD_TYPE=Release
cmake --build build/quadratic-assignment -j4
ctest --test-dir build/quadratic-assignment --output-on-failure --no-tests=error
./build/quadratic-assignment/qap_demo 2
```

`qap_test`: hand-calculated scores (the 0.1.x fixtures), a positive and a zero case
per constraint plus the edge cases (diagonal flow, repeated forbidden rows, a
facility pinned twice, missing demand / capacity, negative costs, an empty pinned
location), `prepare()` validation, incremental == full recompute (score and every
constraint's raw total, each constraint non-zero on some step) over random swap /
unassign / change sequences, and the solver (zero budget keeps the start, warm-start
rules, n = 0 / 1, single / parallel / FULL_ASSERT solves stay bijections, brute-force
optima, determinism). `qap_plugin_test`: default assembly == monolith bit for bit,
plugin incremental == full recompute with accepted and undone moves (per-constraint
raw costs, each constraint non-zero on some step), partial assemblies, overrides
(unit exponent per location, global exponent on the total, per-location hard
rounding), the cost layout, FULL_ASSERT plugin solves, raw costs by hand and the
catalog.

## Usage

```sh
cd use-cases/quadratic-assignment/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import quadratic_assignment as qap
from quadratic_assignment.models import QuadraticAssignmentProblemModel

problem = QuadraticAssignmentProblemModel(
    facilities=["F1", "F2"], locations=["L1", "L2"],
    flows=[[0, 3], [2, 0]], distances=[[0, 4], [5, 0]],
)
print(qap.solve(problem, time_limit_seconds=1.0, random_seed=42))   # 0hard/-22soft; the model, in place
print(problem.assignment, problem.total_cost)

core = problem.to_core()                                             # or the engine object
qap.solve(core, time_limit_seconds=1.0, random_seed=42)
print(core.assignment(), core.total_cost())                          # per location: the facility index
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
`stop_at_first_feasible`, `on_new_best`, a full `config`
(`quadratic_assignment.SolverConfig`) and `constraints`:

```python
from quadratic_assignment.constraints import FlowDistanceCost, PinnedAssignmentsCost, get_constraints, score

qap.constraint_info()      # [ConstraintInfo(name='pinned_assignments_cost', default_level='hard', ...), ...]
qap.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:-1], FlowDistanceCost(weight=2)])
raw = qap.constraint_costs(problem)                  # raw costs, re-priced in pure Python:
score(raw), score(raw, [PinnedAssignmentsCost(level="soft"), *get_constraints()[1:]])
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Its `assignment` must be a full bijection from locations to facilities, matched
by location id: partial assignments, unknown ids and repeated facilities raise
`ValueError`. With a zero budget (`spent_limit_seconds = 0`) the seed comes back
unchanged, even one that breaks a pin (pins are a penalized hard constraint).

```python
from quadratic_assignment import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
qap.solve(warm, time_limit_seconds=1.0, random_seed=42)
print(warm.assignment, warm.total_cost)
```

## QAPLIB benchmark used by Hexaly

`benchmarks/quadratic-assignment/bench/run_bench.py` downloads the same QAP
template archive published by Hexaly and the corresponding QAPLIB solution
archive, verifies both SHA-256 checksums, then re-scores every result with this
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
Hexaly's website publishes a 1.1% aggregate gap at 60 seconds, not its
per-instance assignments, so the script labels that number as an external
aggregate rather than pretending it is a directly reproduced solver run.
The checked-in local snapshot in [`bench/RESULTS.md`](../../benchmarks/quadratic-assignment/bench/RESULTS.md)
holds **0.1.x numbers** (the pure-Python engine): it has not been re-measured for the 0.2.0 C++ engine,
because the QAPLIB archive download was not reachable when 0.2.0 was prepared.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **BASIC**; the domain solver that configures the engine is
`cpp/include/qap/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine runs swap moves only (`changeMoveWeight = 0`,
`swapMoveWeight = 1` in `cpp/src/solver.cpp`). A new move must keep the
permutation (a 3-cycle rotation, a best-partner swap): an
`optimaru::MoveOperator<…>` registered with `engine.addMoveOperator(...)` in
`cpp/src/solver.cpp`, run under FULL_ASSERT. Registered operators do not reach the
engine's multi-start threads, so a solver that registers one runs its own worker
threads (see facility-location's `CloseFacilityMove`).

**Changing the construction heuristic.** The start is the identity (or the
warm-start bijection), laid out in `start()` in `cpp/src/solver.cpp`; the engine's
construction heuristic finds nothing to place. A different start must still be a
full bijection.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/qap/constraints/`:
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a
   `QapUnitConstraint` over the locations, or a `QapMemberChangeGlobalConstraint`
   for a whole-assignment total; positive raw cost = penalty, 0 = no violation)
   declaring `static constexpr double kDefaultWeight` and
   `static constexpr optimaru::Level kDefaultLevel`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the
   registry name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/qap/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set (that is also the
   catalog order);
3. for a default constraint, add the same raw cost to `score_calculator` /
   `incremental_calculator`, weighted by the component's `kDefaultWeight` —
   `qap_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `qap_test` / `qap_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to
   `python/quadratic_assignment/constraints.py` (plus its module-level default
   instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, level="hard")])`;
5. mirror it in the CP-SAT model in
   [`benchmarks/quadratic-assignment/cpsat`](../../benchmarks/quadratic-assignment/cpsat)
   (a plugin subclassing the spec; defaults come from `constraint_info()`), with an
   exact encoding so `test_constraint_parity.py` can pin permutations.

## Benchmark

[`benchmarks/quadratic-assignment`](../../benchmarks/quadratic-assignment) compares this
use case with OR-Tools CP-SAT on the same small instances, rescored by the same
evaluator, and runs the QAPLIB instances above.
