# Semiconductor fab scheduling

Semiconductor-fab scheduling with re-entrant lot routes, alternative tools,
sequence-dependent recipe setups, simultaneous batches, inter-tool transport,
queue-time limits, preventive maintenance, priorities and due dates. It also
models reusable operators/reticles/fixtures, their capacities and calendars,
frozen assignments for replanning, minimum hold times and maximum lot cycle
times. Since 0.2.0 it runs on this repository's C++ engine (`include/optimaru/`):
`cpp/` holds the domain, the decoder, the 20 constraint components, the
calculators and the solver; `python/` is the pybind11 package with the Pydantic
models; [`benchmarks/semiconductor-scheduling/cpsat`](../../benchmarks/semiconductor-scheduling/cpsat)
is the exact CP-SAT reference.

The model combines patterns documented by the official
[Hexaly Flexible Job Shop example](https://www.hexaly.com/docs/last/exampletour/flexiblejobshop.html),
the [IBM CP scheduling example library](https://www.ibm.com/docs/en/icos/22.2.0?topic=programming-where-find-cp-examples),
and [Gurobi's modeling examples](https://github.com/Gurobi/modeling-examples).
The implementation is original and does not depend on a commercial runtime.

## Domain (BASIC variable, decoded schedule)

- One **operation row** per (lot, route step), lot order then route order. Its planning
  value folds the two decisions of the search into one int:
  `value = rank * K + slot` (`K` = the most eligible machines of any operation),
  `rank` = the decode priority, `slot % options` = the machine option; `-1` = unassigned
  (the decoder stops the lot there).
- The **schedule is derived**: the serial schedule-generation decoder
  (`cpp/include/fab/decoder.hpp`) places, step by step, the ready operation with the least
  (rank, -lot priority, lot id) at its earliest feasible start — past machine and resource
  downtime, resource capacity, unary overlaps, incompatible batch families and setups —
  and pulls compatible ready lots into the same batch run. Pins are obstacles placed
  first. It is an exact port of the 0.1.x Python decoder (the frozen oracle in
  `python/tests/reference_semiconductor_scheduling.py` proves it on every sample and on
  random instances).
- Two modes: a schedule is scored **as given** (`calculate_score`, `constraint_costs`:
  data mode) or, inside `solve`, as the **decode of its planning values** (search mode;
  a complete warm-start seed stands for itself while the values are the seed's).

## Constraints (Score — the engine score type as is)

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
The 0.1.x quirks are kept on purpose: a route overlap is charged by
`lot_route_precedence_cost`, by `min_queue_time_cost` (min 0) and by
`missing_transport_time_cost` (no entry); `makespan_cost` reads only the lots' last
steps; `machine_preference_cost` charges even on a duration mismatch.

### 0.3.0

Every constraint takes two more knobs, `offset` and `clip` (keyword-only), and may be
picked several times. Every constraint is global, so the offset shapes its single raw
total `x`: `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which charges a total of 0 too
and rewards a total below the offset); `offset=0` is the 0.2.0 shaping. Both apply to the
constraint's one total (every constraint is global), a raw 0 included. `clip=False` with
an offset is refused on hard (the 14 constraints that are hard by default, or a soft one
moved to hard), because hard 0 must mean no violation: a clean total would otherwise turn
into a hard reward that cancels another constraint's violation. Each pick is its own
instance with its own knobs, its level included (`level` replaced the `hard` knob;
`hard=` raises `TypeError`), and the instances add up (every instance reads the
calculator's one shared schedule view, so a repeat costs no second decode):

```python
fab.solve(problem, constraints=[
    *get_constraints()[:-1],
    MakespanCost(),                              # the makespan
    MakespanCost(weight=100, offset=480),        # + 100 x (every minute beyond 480)
])
```

`fab.constraint_costs(problem, picks)` scores a pick list and keys the second pick
`makespan_cost#2`; every entry reports its `offset`, `clip` and the `level` in effect. The
CP-SAT reference takes the same picks (integral offsets on its eight penalized
constraints; its twelve structural ones refuse an offset like a weight or a level).

### 0.2.0

- **Names**: `machine_eligibility` -> `machine_eligibility_cost`, `lot_route_precedence` ->
  `lot_route_precedence_cost`, `machine_capacity` -> `machine_capacity_cost`,
  `batch_capacity` -> `batch_capacity_cost`, `batch_family_compatibility` ->
  `batch_family_conflict_cost`, `setup_transitions` -> `missing_setup_time_cost`,
  `maintenance_windows` -> `machine_unavailability_cost`, `maximum_queue_time` ->
  `max_queue_time_cost`, `minimum_queue_time` -> `min_queue_time_cost`,
  `inter_tool_transport` -> `missing_transport_time_cost`, `auxiliary_resource_capacity` ->
  `auxiliary_resource_capacity_cost`, `auxiliary_resource_availability` ->
  `auxiliary_resource_unavailability_cost`, `pinned_assignments` -> `pinned_assignments_cost`,
  `maximum_cycle_time` -> `max_cycle_time_cost`, `machine_preference` ->
  `machine_preference_cost`, `minimum_batch_fill` -> `min_batch_size_cost`,
  `machine_load_balance` -> `machine_load_imbalance_cost`, `weighted_tardiness` ->
  `weighted_tardiness_cost`, `total_cycle_time` -> `cycle_time_cost`, `makespan` ->
  `makespan_cost` (classes likewise: `MachineEligibility` -> `MachineEligibilityCost`, ...,
  `BatchFamilyCompatibility` -> `BatchFamilyConflictCost`, `SetupTransitions` ->
  `MissingSetupTimeCost`, `MaintenanceWindows` -> `MachineUnavailabilityCost`,
  `InterToolTransport` -> `MissingTransportTimeCost`, `AuxiliaryResourceAvailability` ->
  `AuxiliaryResourceUnavailabilityCost`, `MinimumBatchFill` -> `MinBatchSizeCost`,
  `MachineLoadBalance` -> `MachineLoadImbalanceCost`, `TotalCycleTime` -> `CycleTimeCost`).
  The model fields (`batch_capacity`, `pinned_assignments`, `makespan`,
  `total_cycle_time`, ...) keep their names. The old names are unknown (no aliases).
- **Raw costs and the default score are unchanged** (golden check and the oracle tests).
- **Exponent granularity**: every constraint is global, so `exponent` shapes its TOTAL
  (`weight * total ** exponent`); 0.1.x shaped each item (a lot, a pair, a window) before
  summing. Hard contributions are rounded per constraint (0.1.x rounded the hard sum once;
  this differs only for fractional hard weights). At exponent 1 and integral weights
  nothing changes.
- **`constraint_costs()` shape**: the standard C++-package dict
  `{name: {"costs": [total], "weight", "exponent", "hard", "global", "level", "reward",
  "level_flippable", "name", "label"}}` instead of `{name: [items]}`; the 0.1.x per-item
  lists are `constraint_items(problem)`.
- **`SolverConfig`**: the engine's knobs (`spent_limit_seconds`, `step_count_limit`,
  `unimproved_*`, `random_seed`, `parallel_solver_count`, `acceptor_type`,
  `simulated_annealing_starting_temperature`, `simulated_annealing_temperature_fraction`,
  `late_acceptance_size`, `machine_move_probability`, `priority_swap_moves`,
  `custom_move_probability`, `ruin_recreate*`, `best_score_limit`, `full_assert`,
  `on_new_best`) replace the simulated-annealing knobs `max_iterations`,
  `stagnation_limit`, `temperature` and `cooling_rate`; `on_new_best` is called with
  `(step, Score)` instead of the search state. `max_iterations=0` becomes
  `spent_limit_seconds=0` (the start is returned as is) and `max_iterations=N` becomes
  `step_count_limit=N`. The `anneal` / `configure` helpers and `common.py` are gone.
- **Search**: the engine's simulated annealing over the decoded state (auto temperature:
  1% of the start's soft score, cooling over the time limit, or over `step_count_limit` when
  that is further along; change moves steered to a
  machine switch or a small priority shift, plus a priority swap of two lots' operations)
  replaces the Python simulated annealing; `parallel_solver_count` runs independent
  threads (seed + i), the strictly best wins. The temperature follows the larger of the
  step fraction and the time fraction, so a seeded, step-limited run repeats only while
  the step fraction leads: give it a time limit generous enough for the step budget. Once
  the clock leads, the run depends on timing. With `constraints=...` the solve is single
  start. A complete warm-start seed is recognised by its values (not by object identity),
  so returning to the seed's values later in the search scores the supplied schedule
  again.
- `solve` / `calculate_score` return a `Score` (iterable and comparable as `(hard, soft)`)
  and accept the Pydantic model (written back in place, as before) or the engine object
  `_core.SemiconductorSchedule` (`to_core()` / `from_core()` now convert to and from it).
- **Integer range**: times, durations, priorities and costs are int64 in the engine.
  `to_core()` refuses an input beyond |2**62| with `ValueError`; a derived time or a cost
  (a tardiness product, an overload area, a sum) that leaves int64 raises `OverflowError`
  instead of wrapping (0.1.x computed exact Python integers). A solve that fails (an
  overflow, or the decoder giving up after 10 000 passes) leaves the schedule as it was:
  both scoring paths score its assignment list again.

## cpp/

- `include/fab/domain.hpp` — facts, `OperationRow`, `SemiconductorSchedule`, the
  `optimaru::SolutionTraits` specialization
- `include/fab/decoder.hpp` — the serial schedule-generation decoder (incremental:
  truncate to a step and decode on), `defaultChoices`, `seedState`
- `include/fab/constraints/` — one component per constraint, each owning its unit
  formula: `machine_eligibility_cost.hpp`, `lot_route_precedence_cost.hpp`,
  `machine_capacity_cost.hpp`, `batch_capacity_cost.hpp`, `batch_family_conflict_cost.hpp`,
  `missing_setup_time_cost.hpp`, `machine_unavailability_cost.hpp`,
  `max_queue_time_cost.hpp`, `min_queue_time_cost.hpp`, `missing_transport_time_cost.hpp`,
  `auxiliary_resource_capacity_cost.hpp`, `auxiliary_resource_unavailability_cost.hpp`,
  `pinned_assignments_cost.hpp`, `max_cycle_time_cost.hpp`, `machine_preference_cost.hpp`,
  `min_batch_size_cost.hpp`, `machine_load_imbalance_cost.hpp`,
  `weighted_tardiness_cost.hpp`, `cycle_time_cost.hpp`, `makespan_cost.hpp`
  (+ `common.hpp`: the shared `ScheduleView` and the global base);
  `include/fab/constraints.hpp` assembles the default set (`defaultConstraints()`)
- `score_calculator` — the monolithic full recompute (`rawCostsOf`: the 0.1.x scorer)
- `evaluator` + `incremental_calculator` — lazy suffix re-decode from the earliest
  `readyStep` of the changed rows, re-costing only the rows, lots, machines, groups,
  resources and pins whose placements moved
- `moves/priority_swap.hpp`, `solver` — the engine wrapper (simulated annealing, the
  value recommender, the priority swap, multi-start)
- `src/main.cpp` — a 12-lot demo

```sh
cmake -S use-cases/semiconductor-scheduling/cpp -B build/semiconductor-scheduling -DCMAKE_BUILD_TYPE=Release
cmake --build build/semiconductor-scheduling -j
ctest --test-dir build/semiconductor-scheduling --output-on-failure --no-tests=error
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

## python/

```sh
cd use-cases/semiconductor-scheduling/python
uv sync --python 3.14 --all-groups --all-extras      # builds the C++ extension
uv run --python 3.14 --with pytest python -m pytest -q tests
```

```python
import semiconductor_scheduling as fab
from semiconductor_scheduling.models import SemiconductorScheduleModel

problem = SemiconductorScheduleModel.model_validate_json(open("fab.json").read())
score = fab.solve(problem, time_limit_seconds=2.0, random_seed=42)   # fills problem.assignments
print(score, problem.makespan)
print([(a.lot_id, a.operation_id, a.machine_id, a.start, a.batch_id) for a in problem.assignments])
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
`stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config`
(`fab.SolverConfig`) and `constraints`. See [python/README.md](python/README.md).

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Assignments are matched by lot and operation id. Any result of `solve` is
accepted, feasible or not. Unknown ids, duplicates, ineligible machines and wrong
durations raise `ValueError`.

```python
warm = fab.with_initial_solution(problem, problem.model_dump(mode="json"))
print(fab.solve(warm, time_limit_seconds=2.0, random_seed=42))
print(warm.makespan, warm.total_weighted_tardiness)
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

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **BASIC** (the composite rank x slot value); the domain solver is
`cpp/include/fab/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The change move's value comes from the value recommender in
`cpp/src/solver.cpp` (a machine switch or a priority shift); `PrioritySwapMove`
(`cpp/include/fab/moves/priority_swap.hpp`) is a custom `optimaru::MoveOperator`
registered with `engine.add_move_operator(...)` — use it as the template.

**Changing the construction.** The start state is built before the engine runs
(`beginSearch` in `cpp/src/solver.cpp`: the default machine choices at identity ranks, or
the seed state of a warm start); the engine's construction heuristic finds nothing to
assign.

**Adding a hard or soft cost.**
1. add `cpp/include/fab/constraints/<subject>_cost.hpp` with `struct <Subject>Cost final :
   ScheduleViewConstraint` declaring `static constexpr double kDefaultWeight`,
   `static constexpr optimaru::Level kDefaultLevel`, its `Term`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor passing the registry
   name (`"<subject>Cost"`), and its raw formula as a static unit function (positive =
   penalty, 0 = no violation);
2. add the `Term` (`score_terms.hpp`), the include and the `emplace` in
   `cpp/include/fab/constraints.hpp`, and call the unit function in `rawCostsOf`
   (`src/score_calculator.cpp`) and in the evaluator's re-cost (`src/evaluator.cpp`);
   the monolithic default score (`ScoreCalculator::scoreOf`) reads `kDefaultWeight`;
3. a hand-calculated positive and zero case in `cpp/test/fab_test.cpp` (and the catalog
   row in `fab_plugin_test.cpp`);
4. the Python spec in `python/semiconductor_scheduling/constraints.py`
   (`class <Subject>Cost(Constraint)` with `name`, `label_en/ko`, `description_en/ko`, a
   module-level instance and a place in `_DEFAULTS`);
5. its CP-SAT plugin in
   [`benchmarks/semiconductor-scheduling/cpsat`](../../benchmarks/semiconductor-scheduling/cpsat)
   with a parity case.

## Benchmark

[`benchmarks/semiconductor-scheduling`](../../benchmarks/semiconductor-scheduling) compares this use case with OR-Tools CP-SAT on the same instances, rescored by the same evaluator.
