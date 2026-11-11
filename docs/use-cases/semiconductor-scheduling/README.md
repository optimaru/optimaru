# Semiconductor fab scheduling

Semiconductor-fab scheduling with re-entrant lot routes, alternative tools,
sequence-dependent recipe setups, simultaneous batches, inter-tool transport,
queue-time limits, preventive maintenance, priorities and due dates. It also
models reusable operators/reticles/fixtures, their capacities and calendars,
frozen assignments for replanning, minimum hold times and maximum lot cycle
times. It runs on this repository's C++ engine (`optimaru/cpp/optimaru/`):
`optimaru/semiconductor_scheduling/cpp/` holds the domain, the decoder, the 20 constraint components, the
calculators and the solver; `optimaru/semiconductor_scheduling/` is the pybind11 package with the Pydantic
models. The exact CP-SAT reference is external-solver's `external_solver.semiconductor_scheduling`;
[`benchmarks/semiconductor-scheduling/cpsat`](../../../benchmarks/semiconductor-scheduling/cpsat)
holds the engine vs CP-SAT comparison.

The model combines standard patterns from the flexible job-shop scheduling
literature (machine flexibility, Brucker & Schlie 1990) with the features of
wafer-fab scheduling listed above.
The implementation is original and does not depend on a commercial runtime.

## Usage

```sh
OPTIMARU_USE_CASES=semiconductor_scheduling uv sync --python 3.14   # builds only this use case's C++ extension
uv run --python 3.14 --with pytest python -m pytest -q tests/semiconductor_scheduling
```

```python
from optimaru import semiconductor_scheduling as fab
from optimaru.semiconductor_scheduling.models import SemiconductorScheduleModel

problem = SemiconductorScheduleModel.model_validate_json(open("fab.json").read())
problem = fab.solve(problem, time_limit_seconds=2.0, random_seed=42)   # a new model with the assignments
print(problem.score_hard, problem.score_soft, problem.makespan)
print([(a.lot_id, a.operation_id, a.machine_id, a.start, a.batch_id) for a in problem.assignments])
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config`
(`fab.SolverConfig`) and `constraints`. See [python.md](python.md).

## Domain (assignment shape, decoded schedule)

- One **operation row** per (lot, route step), lot order then route order. Its decision
  value folds the two decisions of the search into one int:
  `value = rank * K + slot` (`K` = the most eligible machines of any operation),
  `rank` = the decode priority, `slot % options` = the machine option; `-1` = unassigned
  (the decoder stops the lot there).
- The **schedule is derived**: the serial schedule-generation decoder
  (`optimaru/semiconductor_scheduling/cpp/include/fab/decoder.hpp`) places, step by step, the ready operation with the least
  (rank, -lot priority, lot id) at its earliest feasible start — past machine and resource
  downtime, resource capacity, unary overlaps, incompatible batch families and setups —
  and pulls compatible ready lots into the same batch run. Pins are obstacles placed
  first. It matches the pure-Python reference decoder (the test oracle in
  `tests/semiconductor_scheduling/reference_semiconductor_scheduling.py`) on every sample and on random
  instances.
- Two modes: a schedule is scored **as given** (`calculate_score`, `constraint_costs`:
  data mode) or, inside `solve`, as the **decode of its decision values** (search mode;
  a complete warm-start seed stands for itself while the values are the seed's).

## Constraints

The score is the engine's `Score` type as it is.

Every constraint is a `GlobalConstraint` (the schedule is a function of all values) and
reports its total as one raw cost:

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `machine_eligibility_cost` | `MachineEligibilityCost` | hard | 1 | operations without an assignment, on a machine no option names, or with the wrong duration (the LAST option of a repeated machine is read) |
| `lot_route_precedence_cost` | `LotRoutePrecedenceCost` | hard | 1 | first step before release + each step starting before the previous ends (time) |
| `machine_capacity_cost` | `MachineCapacityCost` | hard | 1 | overload area above 1 on unary tools (lot-time) |
| `batch_capacity_cost` | `BatchCapacityCost` | hard | 1 | overload area above the batch capacity on batch tools |
| `batch_family_conflict_cost` | `BatchFamilyConflictCost` | hard | 1 | overlap time of different families on a batch tool |
| `missing_setup_time_cost` | `MissingSetupTimeCost` | hard | 1 | missing setup between consecutive runs (same-family batch pairs skipped) |
| `machine_unavailability_cost` | `MachineUnavailabilityCost` | hard | 1 | processing time inside machine downtime windows |
| `max_queue_time_cost` | `MaxQueueTimeCost` | hard | 1 | waiting beyond a step's max_queue_time |
| `min_queue_time_cost` | `MinQueueTimeCost` | hard | 1 | hold time short of a step's min_queue_time (an overlap counts with min 0) |
| `missing_transport_time_cost` | `MissingTransportTimeCost` | hard | 1 | transport time between groups that does not fit the wait (an overlap counts without an entry) |
| `auxiliary_resource_capacity_cost` | `AuxiliaryResourceCapacityCost` | hard | 1 | overload area above a resource's capacity (a resource listed twice is used twice) |
| `auxiliary_resource_unavailability_cost` | `AuxiliaryResourceUnavailabilityCost` | hard | 1 | resource use inside its unavailable windows |
| `pinned_assignments_cost` | `PinnedAssignmentsCost` | hard | 1 | pins whose operation is missing or moved (machine, start or end) |
| `max_cycle_time_cost` | `MaxCycleTimeCost` | hard | 1 | completion - release beyond max_cycle_time |
| `machine_preference_cost` | `MachinePreferenceCost` | soft | 1 | preference cost of the chosen options |
| `min_batch_size_cost` | `MinBatchSizeCost` | soft | 1 | lots missing below minimum_batch_size per (family, batch id or start) run |
| `machine_load_imbalance_cost` | `MachineLoadImbalanceCost` | soft | 1 | max - min load per equipment group with 2+ machines |
| `weighted_tardiness_cost` | `WeightedTardinessCost` | soft | 10 | priority x lateness of completed lots (= `total_weighted_tardiness`) |
| `cycle_time_cost` | `CycleTimeCost` | soft | 1 | completion - release of completed lots (= `total_cycle_time`) |
| `makespan_cost` | `MakespanCost` | soft | 1 | latest completion among the lots' last steps (= `makespan`) |

Every constraint is flippable (`level="hard"` / `level="soft"`, or `Level.HARD` /
`Level.SOFT`; `level="medium"` raises `ValueError`, the domain has no medium level), none is
a reward, and all 20 are scored by default (`default_enabled`);
`semiconductor_scheduling.constraint_info()` reports exactly this table from the C++
components (`kDefaultLevel` / `kDefaultWeight`).
Some overlaps are charged more than once, on purpose: a route overlap is charged by
`lot_route_precedence_cost`, by `min_queue_time_cost` (min 0) and by
`missing_transport_time_cost` (no entry); `makespan_cost` reads only the lots' last
steps; `machine_preference_cost` charges even on a duration mismatch.

### Offset, clip and repeats

Every constraint takes the knobs `weight`, `level`, `exponent`, `offset` and `clip`
(`offset` and `clip` keyword-only), and may be picked several times. Every constraint is
global, so the exponent and the offset shape its single raw total `x`, not each item (a
lot, a pair, a window): `weight * sign(x) * max(0, |x| - offset) ** exponent`
(`clip=True`, the default) or `weight * (x - offset) ** exponent`, sign-preserving
(`clip=False`, which charges a total of 0 too and rewards a total below the offset);
`offset=0` is the plain power. Hard contributions are rounded per constraint. `clip=False`
with an offset is refused on hard (the 14 constraints that are hard by default, or a soft
one moved to hard), because hard 0 must mean no violation: a clean total would otherwise
turn into a hard reward that cancels another constraint's violation. Each pick is its own
instance with its own knobs, its level included, and the instances add up (every instance
reads the calculator's one shared schedule view, so a repeat costs no second decode):

```python
problem = fab.solve(problem, constraints=[
    *get_constraints()[:-1],
    MakespanCost(),                              # the makespan
    MakespanCost(weight=100, offset=480),        # + 100 x (every minute beyond 480)
])
```

`fab.constraint_costs(problem, picks)` scores a pick list and keys the second pick
`makespan_cost#2`; every entry reports its `offset`, `clip` and the `level` in effect. The
CP-SAT reference takes the same picks (integral offsets on its eight penalized
constraints; its twelve structural ones refuse an offset like a weight or a level).

### Scoring and solving

- **`constraint_costs()`**: the standard C++-package dict
  `{name: {"costs": [total], "weight", "exponent", "offset", "clip", "global", "level",
  "reward", "level_flippable", "name", "label", ...}}`; the per-item lists (one item per lot,
  route pair, machine, window, ...) are `constraint_items(problem)`.
- **`SolverConfig`**: the engine's knobs (`spent_limit_seconds`, `step_count_limit`,
  `unimproved_*`, `random_seed`, `parallel_solver_count`, `acceptor_type` and the
  parameters of each rule (see
  [Choosing the acceptance rule](../README.md#choosing-the-acceptance-rule)),
  `late_acceptance_size`, `machine_move_probability`, `priority_swap_moves`,
  `custom_move_probability`, `change_move_weight`, `swap_move_weight`, `ruin_recreate*`,
  `best_score_limit`, `full_assert`,
  `on_new_best`); `on_new_best` is called with `(step, Score)`. `spent_limit_seconds=0`
  returns the start as is.
- **Search**: the engine's simulated annealing over the decoded state (auto temperature:
  1% of the start's soft score, cooling over the time limit, or over `step_count_limit` when
  that is further along; change moves steered to a
  machine switch or a small priority shift, plus a priority swap of two lots' operations);
  `parallel_solver_count` runs independent
  threads (seed + i), the strictly best wins. The temperature follows the larger of the
  step fraction and the time fraction, so a seeded, step-limited run repeats only while
  the step fraction leads: give it a time limit generous enough for the step budget. Once
  the clock leads, the run depends on timing. With `constraints=...` the solve is single
  start. A complete warm-start seed is recognised by its values (not by object identity),
  so returning to the seed's values later in the search scores the supplied schedule
  again.
- `solve` / `calculate_score` accept the Pydantic model or the
  engine object `_core.SemiconductorSchedule` (`to_core()` / `from_core()` convert to and
  from it). `solve` returns the solved problem: the engine object it was given, solved in
  place, or, for a model, a new model with the result (the model passed in is not changed;
  `calculate_score` writes the scores into the model it is given). The score is `score_hard` /
  `score_soft` on the model and `score` on the engine object. `calculate_score` returns a
  `Score` (iterable and comparable as `(hard, soft)`).
- **Integer range**: times, durations, priorities and costs are int64 in the engine.
  `to_core()` refuses an input beyond |2**62| with `ValueError`; a derived time or a cost
  (a tardiness product, an overload area, a sum) that leaves int64 raises `OverflowError`
  instead of wrapping. A solve that fails (an
  overflow, or the decoder giving up after 10 000 passes) leaves the schedule as it was:
  both scoring paths score its assignment list again.

## C++

- `optimaru/semiconductor_scheduling/cpp/include/fab/domain.hpp` — facts, `OperationRow`, `SemiconductorSchedule`, the
  `optimaru::SolutionTraits` specialization
- `optimaru/semiconductor_scheduling/cpp/include/fab/decoder.hpp` — the serial schedule-generation decoder (incremental:
  truncate to a step and decode on), `defaultChoices`, `seedState`
- `optimaru/semiconductor_scheduling/cpp/include/fab/constraints/` — one component per constraint, each owning its unit
  formula: `machine_eligibility_cost.hpp`, `lot_route_precedence_cost.hpp`,
  `machine_capacity_cost.hpp`, `batch_capacity_cost.hpp`, `batch_family_conflict_cost.hpp`,
  `missing_setup_time_cost.hpp`, `machine_unavailability_cost.hpp`,
  `max_queue_time_cost.hpp`, `min_queue_time_cost.hpp`, `missing_transport_time_cost.hpp`,
  `auxiliary_resource_capacity_cost.hpp`, `auxiliary_resource_unavailability_cost.hpp`,
  `pinned_assignments_cost.hpp`, `max_cycle_time_cost.hpp`, `machine_preference_cost.hpp`,
  `min_batch_size_cost.hpp`, `machine_load_imbalance_cost.hpp`,
  `weighted_tardiness_cost.hpp`, `cycle_time_cost.hpp`, `makespan_cost.hpp`
  (+ `common.hpp`: the shared `ScheduleView` and the global base);
  `optimaru/semiconductor_scheduling/cpp/include/fab/constraints.hpp` assembles the default set (`defaultConstraints()`)
- `score_calculator` — the monolithic full recompute (`rawCostsOf`)
- `evaluator` + `incremental_calculator` — lazy suffix re-decode from the earliest
  `readyStep` of the changed rows, re-costing only the rows, lots, machines, groups,
  resources and pins whose placements moved
- `moves/priority_swap.hpp`, `solver` — the engine wrapper (simulated annealing, the
  value recommender, the priority swap, multi-start)
- `optimaru/semiconductor_scheduling/cpp/src/main.cpp` — a 12-lot demo

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=semiconductor_scheduling
cmake --build build -j
ctest --test-dir build --output-on-failure --no-tests=error
```

`fab_test`: decoder fixtures, a hand-calculated positive and zero case per constraint,
incremental == full recompute of every constraint's raw cost on 300 random instances x
1000 moves (each constraint non-zero in some decoded state; the random instances cluster
pins so that pin overlaps reach the capacity and family-conflict terms), solver contracts
(zero budget, seeds never lost, multi-start, ScoreChecks::Full, a failed solve), int64 overflow.
`fab_plugin_test`: default assembly == monolith (data and search mode), plugin incremental
== full per constraint, components alone, overrides, `constraint_totals()`, raw costs by
hand, the catalog, and repeated / shifted components (a hand-calculated repeat, threshold
and literal offsets by hand, the literal formula refused on hard at reset and kept on soft,
incremental == a fresh recompute over random walks, ScoreChecks::Full solves).

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Assignments are matched by lot and operation id. Any result of `solve` is
accepted, feasible or not. Unknown ids, duplicates, ineligible machines and wrong
durations raise `ValueError`.

```python
warm = fab.with_initial_solution(problem, problem.model_dump(mode="json"))
warm = fab.solve(warm, time_limit_seconds=2.0, random_seed=42)
print(warm.score_hard, warm.score_soft, warm.makespan, warm.total_weighted_tardiness)
```

The search works on machine choices and priority ranks that the decoder turns into
a schedule, and the decoder left-shifts operations and re-derives batches, so
decoding a seed can give a different schedule, sometimes a worse one. A complete
seed is therefore scored as supplied: `solve` returns it unchanged unless the
search finds a strictly better schedule, so the score is never worse than the
seed's. The search itself is not guaranteed to stay near the seed or to improve
it. A partial seed cannot be scored, so it is decoded: it keeps its machines and
relative order, but its start times are recomputed.

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape (the composite rank x slot value); the domain solver is
`optimaru/semiconductor_scheduling/cpp/include/fab/solver.hpp` + `optimaru/semiconductor_scheduling/cpp/src/solver.cpp`.

**Adding a move.** The change move's value comes from the value recommender in
`optimaru/semiconductor_scheduling/cpp/src/solver.cpp` (a machine switch or a priority shift); `PrioritySwapMove`
(`optimaru/semiconductor_scheduling/cpp/include/fab/moves/priority_swap.hpp`) is a custom `optimaru::MoveOperator`
registered with `engine.add_move_operator(...)` — use it as the template.

**Changing the construction.** The start state is built before the engine runs
(`beginSearch` in `optimaru/semiconductor_scheduling/cpp/src/solver.cpp`: the default machine choices at identity ranks, or
the seed state of a warm start); the engine's construction heuristic finds nothing to
assign.

**Adding a hard or soft cost.**
1. add `optimaru/semiconductor_scheduling/cpp/include/fab/constraints/<subject>_cost.hpp` with `struct <Subject>Cost final :
   ScheduleViewConstraint` declaring `static constexpr double kDefaultWeight`,
   `static constexpr optimaru::Level kDefaultLevel`, its `Term`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor passing the registry
   name (`"<subject>Cost"`), and its raw formula as a static unit function (positive =
   penalty, 0 = no violation);
2. add the `Term` (`score_terms.hpp`), the include and the `emplace` in
   `optimaru/semiconductor_scheduling/cpp/include/fab/constraints.hpp`, and call the unit function in `rawCostsOf`
   (`optimaru/semiconductor_scheduling/cpp/src/score_calculator.cpp`) and in the evaluator's re-cost (`optimaru/semiconductor_scheduling/cpp/src/evaluator.cpp`);
   the monolithic default score (`ScoreCalculator::scoreOf`) reads `kDefaultWeight`;
3. a hand-calculated positive and zero case in `tests/semiconductor_scheduling/cpp/fab_test.cpp` (and the catalog
   row in `fab_plugin_test.cpp`);
4. the Python spec in `optimaru/semiconductor_scheduling/constraints.py`
   (`class <Subject>Cost(Constraint)` with `name`, `label_en/ko`, `description_en/ko`, a
   module-level instance and a place in `_DEFAULTS`);
5. its CP-SAT plugin in external-solver's
   [`external_solver.semiconductor_scheduling.constraints`](https://github.com/optimaru/external-solver/blob/v0.2.3/src/external_solver/semiconductor_scheduling/constraints.py)
   with a parity case.

## Benchmark

[`benchmarks/semiconductor-scheduling`](../../../benchmarks/semiconductor-scheduling) compares this use case with OR-Tools CP-SAT on the same instances, rescored by the same evaluator.
