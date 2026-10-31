# Changelog

## 0.2.0

0.2.0 puts every use case on one constraint contract. Every constraint has a
`*_cost` name, the same three knobs and defaults that are declared once in
C++. The six use cases that were pure-Python simulated annealing are now C++
engine use cases like the other sixteen.

**This release breaks compatibility.** No old constraint name is kept as an
alias: an old name raises `ValueError` (`unknown constraint`). The
[complete old → new table](#constraint-renames) is at the end. Raw costs and
default scores are unchanged, except where a use case's notes below say
otherwise. The migrated use cases were compared with optimaru 0.1.5 on their
samples, and each port with its frozen 0.1.x scorer.

### The constraint contract (all 22 use cases)

**Names.** A constraint is one component: `<Subject>Cost` for the C++ struct
and the Python spec class, `<subject>_cost` for its Python name, and the
camelCase struct name for the C++ registry (`vehicleCapacityCost`). The
module-level default instances use the snake name
(`vrp_routing.constraints.vehicle_capacity_cost`). Model fields keep their
names; for example, `max_stops` is still a field and `max_stops_cost` is the
constraint.

**Three knobs, nothing else.** A selection sets these per constraint:

- `weight`: `None` (the default) is the component's default weight. A number
  is in units of the constraint's level. It must be finite and `>= 0`.
- `hard`: `None` keeps the default level, `True` makes the constraint hard
  and `False` makes it soft.
- `exponent`: `1.0` by default. It must be finite and `> 0`, and it preserves
  sign. A unit constraint is shaped per unit; a global constraint is shaped on
  its single total, before the weight is applied.

Hard contributions are rounded per unit (per global constraint for a global),
not once on the hard total. The result differs from 0.1.x only for fractional
hard weights, or when a shaped value is fractional.

**Levels: hard, medium, soft.** Some domains have a medium level between hard
and soft. Medium is a default-only level: there is no medium knob, and
`hard=None` keeps it. A medium constraint has default weight 1 in medium
units. `hard=True` charges 1 hard unit per raw unit, and `hard=False` demotes
it to soft at 1 soft unit per raw unit. An explicit weight stays in medium
units. In 0.1.x these constraints carried the fold factor as their weight, so
`hard=True` charged the whole factor in hard units, `hard=False` did nothing,
and an explicit weight silently became a plain soft weight. Each domain now
declares once how medium folds into the engine's hard/soft score:

| Fold | Domains |
| --- | --- |
| `into_soft`: soft = medium × unit + soft | bed-allocation, vehicle-routing, pickup-delivery, project-job-scheduling (1e9); food-packaging (1e10); meeting-scheduling, tournament-scheduling (1e6) |
| `below_hard`: hard = hard × unit + medium | task-assigning (1e9) |

Every `constraint_costs()` entry of a medium domain carries
`medium_fold = {kind, unit}`. `score()` and `score_costs()` always apply the
fold, even for a selection without a medium term.

**Rewards** have a raw cost `<= 0` (a quantity to maximize, scored as a
negative penalty) and are marked `reward=True, level_flippable=False`.
`hard=True` on a reward raises `ValueError` on every path, CP-SAT included.
The rewards are:

- conference-scheduling: `audience_type_diversity_cost`,
  `audience_level_diversity_cost` and `language_diversity_cost`
- employee-scheduling: `desired_day_cost` and `desired_shift_cost`
- knapsack: `selected_value_cost`
- max-cut: `cut_weight_cost`
- the school-timetabling quickstart model: `teacher_time_efficiency_cost`

**Structural constraints** keep their level (`level_flippable=False`). In the
engine these are school-timetabling's `fixed_slots_cost` and
`parallel_groups_cost`. Some constraints are structural only in a CP-SAT
reference model:

- pickup-delivery: `paired_request_cost`, `same_vehicle_cost` and
  `pickup_before_dropoff_cost`
- semiconductor-scheduling: the 12 hard rules other than
  `max_queue_time_cost` and `max_cycle_time_cost`
- school-timetabling: 8 of the hard constraints

**Opt-in constraints** have default weight 0 and `default_enabled=False`:

- vehicle-routing and pickup-delivery: `enclosing_circle_radius_cost`,
  `route_duration_cost` and `latency_cost`
- pickup-delivery: `squared_order_age_cost`
- assembly-line-balancing: `workload_imbalance_cost`
- max-cut: `partition_imbalance_cost`

**Defaults live in C++ only.** Each component declares
`kDefaultWeight` / `kDefaultLevel`. The monolithic calculators read those
constants, and Python reads them through the new catalog:

- `<package>.constraint_info()` returns one
  `ConstraintInfo(name, default_level, default_weight, level_flippable, reward, default_enabled)`
  namedtuple per constraint. `default_level` is `'hard' | 'medium' | 'soft'`.
  `default_enabled` is True when the constraint is in the default set with a
  weight > 0.
- A catalog can be wider than the default set: the default rows come first,
  in `list_constraints()` order, then any catalog-only rows with
  `default_enabled=False` (employee-scheduling).
- Python spec classes declare no `default_weight` / `default_hard` /
  `default_level`. They carry `label_en`, `label_ko`, `description_en` and
  `description_ko`.
- Default weights stored on canonical problem models are removed (see
  *Removed APIs*). Weights come only from constraint selections.

**`constraint_costs()` is the only per-constraint surface.** Every package
returns the same dict:
`{name: {costs, weight, exponent, hard, global, level, reward, level_flippable, name, label}}`,
plus `medium_fold` on a medium domain. `name` is the plain Python name.
`label` is the display text of one instance of a parametrized constraint
(`''` means none). A name selected more than once (employee-scheduling) is
keyed `<name>#<k>` from its second instance on.

**One shared spec base.** `python/optimaru/_constraint.py` is the single
source. Every package carries a byte-identical copy as `<package>/_constraint.py`,
and `scripts/check_constraint_base.py` enforces this in CI. In every package:

- `get_constraints()` refuses an empty list and a constraint picked twice
  ("picked more than once").
- `select(constraints, defaults, *, repeatable=False)` and `instance_keys()`
  are new. `repeatable=True` allows one name several times with different
  parameters (employee-scheduling).
- Knob types are checked the same way in Python and C++: `weight=True` or a
  string, and `hard=1` or `'x'`, raise `TypeError`
  (`constraint '<name>': weight must be a number or None, not True`).
  `numpy.bool_` is accepted for `hard`.
- Out-of-range values raise `ValueError`.

**Scores of a selection.** On the `constraints=` path, bed-allocation,
food-packaging, pickup-delivery, project-job-scheduling and vehicle-routing
report the final `score` and every `on_new_best` score from the
calculator's three level accumulators (`levelScores()`), before the medium
fold. A demoted or re-weighted term therefore shows on the level the
selection gave it, and a large soft total (1e9 or more; 1e10 for
food-packaging) no longer appears as medium. Results at the defaults are
identical.

### Engine (C++, `include/optimaru`)

- `optimaru::Level { Hard, Medium, Soft }` and `levelName()`
  (`api/score/level.hpp`). `MediumFold::none() / intoSoft(unit) / belowHard(unit)`
  is registered per solution type with `MediumFoldOf<Solution>`.
- `ConstraintMetadata` (the shared base of `UnitConstraint` and
  `GlobalConstraint`) has these fields: `name`, `weight`, `level`, `reward`,
  `levelFlippable`, `exponent` and `label`. It gains the validated setters
  `setWeight`, `setLevel` and `setExponent`, and `isHard()` is now a member
  function. The constructors take `(name, weight, Level)`.
- `ConstraintAssembly`: `weight()`, `level()`, `hard()`, `soft()`,
  `exponent()`, `drop()`, `only()`, `names()` and `info()` (the catalog).
  `hard()` throws on a reward, and `hard()` / `soft()` throw on a
  non-flippable constraint. `optimaru::catalogInfo(defaults, catalog)` builds
  a catalog that is wider than the default set.
- `DecomposedScoreCalculator` keeps three accumulators and folds medium with
  the domain's `MediumFold`. `levelScores()` reports the three levels before
  the fold. `constraintTotals()` and `constraintCosts()` report each
  constraint's level and label. A hard reward is rejected at reset, and so is
  a medium constraint on a domain without a fold.
- The pybind11 helpers (`python/constraint_binding.hpp`) gain
  `bindConstraintInfo`, `applyKnobs` / `readKnobs` for components a domain
  factory builds, the `medium_fold` entry and `instanceKeys`.
  `python/level_guard.hpp` holds `levelForHard()`, the pybind-free
  `hard=True/False` guard that the bindings raise as `ValueError`.
  `pythonName()` maps camelCase registry names only.
- `test/constraint_level_test.cpp` is new. It covers levels and both folds,
  the knob guards, the catalog, exponent shaping (including sign preservation
  on a reward), the random-walk incremental vs. recompute check for both
  folds, and preview parity on a LIST-kind domain with a medium level.
- Removed: the `(name, weight, bool hard)` constructors and the assignable
  `isHard` member.

### CP-SAT reference models (`benchmarks/`)

- Every CP-SAT constraint is a `cpsat-common` `ConstraintPlugin` that
  subclasses the domain's spec. It takes `default_level`, `default_weight`,
  `level_flippable` and `reward` from the domain's `constraint_info()`. A
  plugin that restates a default, or whose domain has no `constraint_info()`,
  raises `TypeError`. The ClassVar/instance default path is removed, and so is
  `ConstraintRegistry.objective(default_weights=...)`.
- `hard=False` lands on the soft level, from a hard or a medium default, as
  in the engine. In 0.1.x it moved a term to the next lower level. Several
  CP-SAT defaults that had drifted from the engine now match it:
  sports-league workday derbies (1 → 1000), the meeting and maintenance
  weights, and the bed / vehicle-routing / pickup-delivery / project-job /
  food-packaging / tournament medium constants.
- Catalog rows use the server vocabulary. `kind` is `either` for a flippable
  constraint, otherwise its fixed level. `default_enabled` comes from the
  engine row, and `params` is `{weight: None, hard: None, exponent: 1.0}`.
- `exponent != 1` and non-integer weights are still rejected on CP-SAT.

### The six C++ ports

assembly-line-balancing, bin-packing, knapsack, max-cut, quadratic-assignment
and semiconductor-scheduling are now C++ engine use cases. Each has a C++
domain, full and incremental score calculators, one plugin component per
constraint, a pybind11 extension `<package>._core` and a CP-SAT reference on
`cpsat-common`. They share these API changes:

- `SolverConfig` is the engine's config, set by attribute, with no keyword
  constructor. Its knobs include `spent_limit_seconds`, `step_count_limit`,
  `unimproved_*`, `random_seed`, `parallel_solver_count`, the acceptor knobs,
  `ruin_recreate*`, the move weights, `best_score_limit`, `full_assert` and
  `on_new_best(step, score)`. The exact set differs per package.
- Removed: `max_iterations`, `stagnation_limit`, `temperature`,
  `cooling_rate` and the `anneal` / `configure` helpers.
  `max_iterations=0` becomes `spent_limit_seconds=0` (`step_count_limit=0`
  means no limit), and `max_iterations=N` becomes `step_count_limit=N`.
  Without a time limit, a solve runs the 30 s default budget.
- `solve()` returns the engine `Score` (`.hard`, `.soft`, `is_feasible()`)
  instead of a tuple. It takes the pydantic model (written back in place) or
  the core object. `calculate_score(problem)` returns the engine `Score`, and
  `calculate_score(problem, constraints)` returns a `constraints.Score`.
  `DEFAULTS` and the top-level `score` are removed; use
  `constraints.get_constraints()` and `constraints.score()`.
- `constraint_costs()` returns the standard dict described above, instead of
  `{name: [terms]}`.
- Unit constraints are shaped per unit and global constraints on their total.
  At exponent 1 the results are identical to 0.1.x. With another exponent
  they can differ; each use case's notes below give the details.
- A solve with `constraints=...` is single-start: `parallel_solver_count` is
  ignored there, as in facility-location.
- `models.to_core()` / `from_core()` convert to and from the engine object.
  The JSON schemas of the models are unchanged, except for bin-packing's
  removed weight fields.
- Seeds do not reproduce 0.1.x trajectories. Each package keeps its old
  Python scorer as `python/tests/reference_<package>.py`, an oracle that the
  C++ costs are tested against on every sample and on seeded random
  instances.

### Per use case

#### assembly-line-balancing (ported to C++)

- Renames: `station_count_limit` → `max_stations_cost`, `minimize_stations` →
  `used_stations_cost`, `workload_balance` → `workload_imbalance_cost`. Every
  other constraint gains the `_cost` suffix. The model fields
  `same_station_groups` and `different_station_pairs` keep their names.
- Defaults: the eight hard constraints are hard 1, and `used_stations_cost` is
  soft 1. `workload_imbalance_cost` is soft 0 and opt-in.
  `list_constraints()` keeps the 0.1.5 order.
- An unassigned task (station `-1`) now contributes nothing, as everywhere
  in the engine. 0.1.5 scored `-1` as a station index, so the unassigned web
  demo went from -18hard to 0hard.
- Exponent granularity:
  - Per station instead of per task: `max_stations_cost`,
    `task_station_bounds_cost`, `fixed_task_stations_cost` and
    `forbidden_task_stations_cost`.
  - Per station instead of per pair: `different_station_pairs_cost`.
  - On the total instead of per group: `same_station_groups_cost`.
- The engine object (`AssemblyLineBalancingProblem` / `Task`) refuses
  `max_stations < 1`, `min_station < 0`, `max_station < min_station` and
  `fixed_station < 0`, with the same rules as the pydantic model. The problem
  exposes `assignment()` / `set_assignment()`.
- `from_core()` of a core that was never scored reports 0hard/0soft.
- CP-SAT: `same_station_groups` now costs (distinct stations − 1), and a
  duplicate forbidden station counts once. The prefix symmetry breaking is
  applied only without station bounds. The model now has an
  engine-comparison runner.

#### bed-allocation

- `unassigned_stays_cost` is a real medium constraint (fold 1e9, see above).
- `Solver.solve_with_constraints` reports `plan.score` from the three level
  accumulators.
- `required_patient_equipment_cost` keeps its quirky raw cost. When a
  required item is missing, it charges nights × the required items the room
  *has*.

#### bin-packing (ported to C++)

- Renames: `bin_capacity` → `bin_capacity_cost`, `color_compatibility` →
  `color_conflict_cost`, `item_conflicts` → `item_conflict_cost`,
  `minimize_bins` → `used_bins_cost`. All four are in
  `constraints.__all__`.
- `capacity_weight`, `used_bin_weight`, `color_weight` and `conflict_weight`
  are removed from `BinPackingProblem` and `BinPackingProblemModel`
  (`extra="forbid"` refuses old payloads). Every default is 1.
- Search is late acceptance with change/swap moves, an `EmptyBinMove` and
  adaptive ruin-and-recreate. `on_new_best(step, score)` replaces
  `on_new_best(problem)`, and the stagnation restarts are gone.
  `solve()` also takes `stop_at_first_feasible` and `on_new_best`.
- Multi-start: worker 0 starts from the weight-descending best fit, and
  worker i > 0 starts from the same order with equal weights shuffled.
- Exponent granularity is per bin. `color_conflict_cost` and
  `item_conflict_cost` were shaped per (bin, colour) and per pair. For
  example, red ×3 + blue ×2 in one bin costs 9 at exponent 2 (0.1.x: 5).
- `constraint_costs()` lists one cost per bin 0..n−1 for the per-bin
  constraints and `[count]` for `used_bins_cost`.
- Construction is the 0.1.x best-fit decreasing with one fix: it never opens
  bin n. After a search, bins are relabelled 0..k−1.
- Stricter core checks: scoring or solving raises `ValueError` for any of:
  - `bin_capacity <= 0`
  - a repeated item id
  - a weight `<= 0`
  - a conflict that names an unknown item or the same item twice
  - a bin outside −1..n−1
- `problem.items[i]` returns a copy; write bins with `set_assignment()` and
  read them with `assignment()`. `problem.score` is read-only. `Score` reprs
  as `<Score 0hard/-2soft>`.

#### conference-scheduling

- Renames: all 37 constraints. Most gain `_cost`; seven get new subjects,
  for example `same_day_talks` → `different_day_talks_cost`.
- The Python `Breakdown` and `compute_breakdown()` are removed; use
  `constraint_costs()`. The C++ `cs::Breakdown` stays, with members renamed
  to the registry names, and its totals read `kDefaultWeight`.
- The three diversity constraints are rewards. `hard=True` raises, and
  `hard=False` is a no-op. `audience_type_diversity_cost` still rewards
  same-timeslot pairs that share an audience type (the 0.1.x arithmetic).

#### employee-scheduling

- Renames: all 30 constraints. The C++ registry names are camelCase
  (`"Missing required skill"` → `requiredSkillCost`). Parametrized display
  names became the per-instance `label`. `constraint_totals()` and
  `rule_totals` report `{name, label, cost, level, is_hard}`.
- One class per constraint. The hand-written pydantic `*Rule` models are
  generated from the specs as `<SpecClass>Rule` (for example,
  `MaxConsecutiveDaysRule` → `MaxConsecutiveDaysCostRule`,
  `UnavailableWorkerRule` → `EmployeeUnavailabilityCostRule`), so 0.1.5
  imports raise `ImportError`. Use `models.rule_model(name)` or pass spec
  instances. `rule.to_spec()` returns the spec.
- JSON: `policy: "hard" | "soft"` is replaced by `hard: true | false | null`,
  and `weight` defaults to `null` (the C++ default, 1). Scope keys
  (`worker_indices` / `shift_indices`) are accepted only on per-employee rules
  and `shift_coverage_cost`. A global rule refuses them, even as `null`.
- `DecomposedCalculator.add(name, weight=None, hard=None, exponent=1.0, **params)`.
  `constraint_costs(schedule, constraints=None)` is new, and a rule picked
  more than once is keyed `<name>#<k>`.
- `constraint_info()` has 30 rows: the 8 defaults with `default_enabled=True`,
  then the 22 rule-only constraints with `default_enabled=False`.
  `list_constraints()` / `CONSTRAINT_NAMES` deliberately list all 30 names
  the rule factory accepts. The default set is `get_constraints()`.
- The 10 constraints that existed only as rule models now have spec classes.
  `UndesiredDayCost` / `DesiredDayCost` gain `per_day`, and
  `MinWeeklyMinutesCost` gains `week_anchor`, which the C++ path now uses.
  Spec parameters validate their bounds at construction.
- Documentation fix: `worker_location_count_cost` and
  `worker_day_count_in_range_cost` are soft by default, as they always were.
  The location-run labels now name their location.

#### facility-location

- Renames: `facility_capacity` → `facility_capacity_cost` and
  `distance_from_facility` → `distance_from_facility_cost`.
  `facility_setup_cost` keeps its name. The default instances `capacity`,
  `setup_cost` and `distance` are now `facility_capacity_cost`,
  `facility_setup_cost` and `distance_from_facility_cost`.
- `setup_cost_weight` and `distance_from_facility_weight` are removed from the
  problem, both in C++ and in the pydantic model. The defaults are the
  component constants: setup 2, distance 6, capacity 1.
- C++ `defaultConstraints()` takes no arguments.

#### flight-crew-scheduling

- Renames: every constraint gains `_cost`.
- The C++ constants `fcs::REQUIRED_SKILL_WEIGHT`, `FLIGHT_CONFLICT_WEIGHT`,
  `TRANSFER_WEIGHT`, `UNAVAILABILITY_WEIGHT`, `FIRST_NOT_HOME_WEIGHT` and
  `LAST_NOT_HOME_WEIGHT` are removed; use `<Component>::kDefaultWeight`.

#### food-packaging

- Renames: `max_end_date_time` → `max_end_time_cost`,
  `ideal_end_date_time` → `after_ideal_end_time_cost`, `minimize_makespan` →
  `squared_makespan_cost`, `maximize_jobs_assigned` →
  `unassigned_jobs_cost`, and `operator_cleaning_conflict` gains `_cost`.
- `after_ideal_end_time_cost` and `unassigned_jobs_cost` are medium. The fold
  is 1e10; the docs that said 1e9 were wrong.
- `fp.analyze()` and `fp.ConstraintBreakdown` are removed; use
  `fp.constraint_costs(schedule)`. The C++ `ConstraintBreakdown` stays for
  tests, with members renamed to the registry names.
- `solve(schedule, constraints=...)` reports the three level accumulators.

#### knapsack (ported to C++)

- Renames:
  - `capacity` → `knapsack_capacity_cost`
  - `resource_capacities` → `resource_capacity_cost`
  - `item_dependencies` → `item_dependency_cost`
  - `maximize_value` → `selected_value_cost` (a reward)
  - the others gain `_cost`

  The model fields keep their names.
- Defaults: six hard constraints at weight 1, and `selected_value_cost` soft
  at weight 1.
- Every knapsack constraint is global, so `constraint_costs()` has one raw
  total per constraint. 0.1.5 had one term per resource, entry, group or
  pair. The exponent now shapes each total; for example, 3 missing mandatory
  items at exponent 2 cost 9 (0.1.5: 3).
- `ruin_recreate_as_move_weight` defaults to 0.05.
- Removed: `_greedy_selection`; use `knapsack._core.greedy_selection`.
  `with_initial_solution` fills omitted items with that greedy.
  `from_core()` returns each item's `resource_usage` keys sorted.

#### maintenance-scheduling

- Renames: every constraint gains `_cost`. This also removes the clash between
  constraint and field names: the fields `min_start_date`, `max_end_date` and
  `ideal_end_date` keep their names.
- The CP-SAT plugin registry is in `list_constraints()` order.

#### max-cut (ported to C++)

- Renames:
  - `partition_balance` → `partition_imbalance_cost` (opt-in, weight 0)
  - `maximize_cut_weight` → `cut_weight_cost` (a reward)
  - the others gain `_cost`
- `fixed_partition_cost`, `same_partition_pairs_cost` and
  `different_partition_pairs_cost` are reported per side (`[side 0, side 1]`)
  instead of per vertex or pair. For example, 3 misplaced fixed vertices on
  one side cost 9 at exponent 2 (0.1.5: 3).
- `calculate_score` and `constraint_costs` raise `ValueError` on a partial
  partition, for a model or a core; `solve` still fills missing sides.
  `MaxCutProblemModel` refuses partition entries for unknown vertices and for
  sides other than 0/1.
- In exact-size mode (`min_partition_size == max_partition_size`), the solver
  forces swap-only moves.
- Multi-start runs independent searches from the same start.
- A raising `on_new_best` (or any worker error) under
  `parallel_solver_count > 1` raises from `solve()` instead of aborting the
  process.

#### meeting-scheduling

- Renames:
  - `avoid_overtime` → `overtime_cost`
  - `required_room_capacity` → `room_capacity_cost`
  - `start_and_end_on_same_day` → `day_crossing_cost`
  - `do_meetings_as_soon_as_possible` → `meeting_end_time_cost`
  - `one_break_between_consecutive_meetings` → `consecutive_meetings_cost`
  - `overlapping_meetings` → `meeting_overlap_cost`
  - `assign_larger_rooms_first` → `larger_room_capacity_cost`
  - `room_stability` → `room_change_cost`
  - the others gain `_cost`
- The two attendance-conflict constraints are medium (fold 1e6).
- `compute_breakdown()` and `Breakdown` are removed; use
  `constraint_costs()`.
- `overtime_cost` keeps the Java quirk: its raw cost is the meeting's last
  grain index.
- CP-SAT: `solve()` folded the medium level with 1e9; it now uses 1e6.

#### order-picking

- Renames:
  - `required_number_of_buckets` → `trolley_capacity_cost`
  - `minimize_distance_from_previous_pick_task` →
    `distance_from_previous_pick_task_cost`
  - `minimize_distance_from_last_pick_task_to_path_origin` →
    `distance_from_last_pick_task_to_path_origin_cost`
  - `minimize_order_split_by_trolley` → `order_split_by_trolley_cost`
- `ConstraintTotals` and `calculate_totals()` are removed; use
  `constraint_costs()`. Its `order_split_by_trolley_cost` costs hold the raw
  (order, trolley) pair count; multiply by the weight, 1000, for the old
  total. `OrderPickingResponse.total_distance` / `order_split_count` keep
  their values.
- The C++ `ConstraintTotals` members are renamed to the registry names.
- `order_split_by_trolley_cost` counts every (order, trolley) pair, so it is
  never 0 while picks are assigned (documented).

#### pickup-delivery

- Renames:
  - `service_finished_after_max_end_time` → `max_end_time_cost`
  - `vehicle_end_time` → `vehicle_max_end_time_cost`
  - `minimize_travel_time` → `travel_time_cost`
  - `minimize_enclosing_circle_radius` → `enclosing_circle_radius_cost`
  - `minimize_route_duration` → `route_duration_cost`
  - `minimize_latency` → `latency_cost`
  - `minimize_lateness` → `squared_order_age_cost`: it measures squared order
    age, not lateness
  - the others gain `_cost`; `vehicle_fixed_cost` is unchanged
- `unassigned_requests_cost` is medium (fold 1e9).
- Four opt-in objectives: `enclosing_circle_radius_cost`,
  `route_duration_cost`, `latency_cost` and `squared_order_age_cost`.
- Every no-arg C++ constructor now takes the weight.
- `squared_order_age_cost` squares inside its raw cost, so an exponent
  compounds it.
- CP-SAT: the three coupling constraints are structural.
  `enclosing_circle_radius_cost`, `squared_order_age_cost`,
  `pinned_stops_cost` and `required_vehicle_cost` are engine-only.

#### project-job-scheduling

- Renames: `total_project_delay` → `project_delay_cost` and
  `total_makespan` → `makespan_cost` (the latest sink end, a max). The
  resource constraints gain `_cost`.
- `project_delay_cost` is medium (fold 1e9).
- CP-SAT: `hard=False` on a hard constraint lands on soft (it used to land on
  medium).

#### quadratic-assignment (ported to C++)

- Renames:
  - `fixed_assignments` → `pinned_assignments_cost`
  - `forbidden_assignments` → `forbidden_assignments_cost`
  - `location_capacities` → `location_capacity_cost`
  - `minimize_assignment_cost` → `assignment_cost`
  - `minimize_flow_distance` → `flow_distance_cost`
- Exponent granularity is unchanged. Hard contributions are rounded per
  location, so results differ only when a hard per-location value is
  fractional. For example, two broken pins at weight 0.5 give -2 (0.1.5:
  -1), and `location_capacity_cost` costs [2, 2] at exponent 0.5 give -2
  (0.1.5: -3).
- Search is late acceptance over swap moves only. It starts from the identity
  or a warm-start bijection, and the result is asserted to be a bijection.
  `acceptor_type` and `entity_tabu_size` are exposed.
- The input assignment must be representable even without `warm_start`:
  known ids, and no facility on two locations. Otherwise `to_core()` raises
  `ValueError`. A partial assignment is scored on the core; model-level
  scoring names the missing locations.
- `QuadraticAssignmentProblem.score` is `Score | None`.
  `from_core()` fills `score_hard` / `score_soft` only from a scored core,
  and `total_cost` whenever the assignment is a bijection. A guard raises
  `OverflowError` when the objective could overflow int64.
- CP-SAT: duplicate forbidden pairs count once, and the objective bounds
  cover negative data. `flow_distance_cost` uses linear channeling, which
  presolves much faster.
- The QAPLIB tables are 0.1.x numbers that have not been re-measured.

#### school-timetabling

- Renames: one public name per constraint, replacing both the engine and the
  CP-SAT/server/DB spellings. For example, `unassigned_lesson` /
  `weekly_period_counts` → `unassigned_lessons_cost`, and `slot_not_allowed`
  → `disallowed_slot_cost`. The `H1`…`S6` display strings are gone.
- `fixed_slots_cost` and `parallel_groups_cost` are real components. They
  are always 0 by construction, hard and not flippable (`hard=False` raises).
  `list_constraints()` and `constraint_info()` hold 17 names.
- Removed: `CPSAT_ALIASES`, `STRUCTURAL_IN_ENGINE`, `ENGINE_ONLY`,
  `resolve_name`, `canon.Breakdown`, `canon.compute_breakdown` and
  `canon.Timetable.weights`.
- `weight=None` is always the component default (hard 1; soft 20/3/30/10/5/1),
  never `config.weights`. `canon.ObjectiveWeights`, `SolverConfig.weights` and
  `solve(weights=)` remain only as the monolithic fast path's soft weights:
  - Their fields are the `*_cost` names, and any non-negative finite number
    is accepted.
  - A bool, NaN or infinity, a negative weight and an unknown key raise.
  - Non-default fast-path weights together with `constraints=` raise.
- `Timetable.penalties` keys are the soft constraint names, and partial
  timetabling reports `penalties['unassigned_lessons_cost']`.
  `UnassignedLessonsCost(hard=False)` with `weight=None` costs 1 per unplaced
  period on both paths. Pass `weight=UNASSIGNED_LESSONS_SOFT_WEIGHT` (10000)
  for the 0.1.5 partial-timetabling behaviour.
- `DecomposedCalculator.constraint_totals()` returns
  `{name, label, cost, level, is_hard}` in `list_constraints()` order, and
  `list_constraints()` returns a list.
- C++: `defaultConstraints()` / `addDefaultConstraints()` /
  `addCanonicalConstraints()` take no `ObjectiveWeights`, and
  `teacherDailyExcess` + `teacherWeeklyExcess` are one member,
  `teacherMaxPeriodsCost`.
- The quickstart model's labels and the `evaluate.py` keys use the `*_cost`
  names. `teacher_time_efficiency_cost` is reported as the raw reward
  (`<= 0`).
- CP-SAT (`teacher_scheduling`): the registry classes subclass the
  `st_timetabling` specs. The DTOs carry exactly `type` / `weight` / `hard` /
  `exponent`. Eight hard constraints are structural there.

#### semiconductor-scheduling (ported to C++)

- The solve, the scorer and the serial decoder run in C++. The frozen 0.1.x
  oracle shows that raw costs, item lists, default scores, decoded schedules
  and seed states are unchanged.
- Renames:
  - `batch_family_compatibility` → `batch_family_conflict_cost`
  - `setup_transitions` → `missing_setup_time_cost`
  - `maintenance_windows` → `machine_unavailability_cost`
  - `inter_tool_transport` → `missing_transport_time_cost`
  - `auxiliary_resource_availability` → `auxiliary_resource_unavailability_cost`
  - `minimum_batch_fill` → `min_batch_size_cost`
  - `machine_load_balance` → `machine_load_imbalance_cost`
  - `total_cycle_time` → `cycle_time_cost`
  - `maximum_*` / `minimum_*` → `max_*` / `min_*`
  - the others gain `_cost`
- Every constraint is global: the exponent shapes the total (0.1.x shaped
  each item). `constraint_costs()` returns the standard dict, and the 0.1.x
  per-item lists are available as `constraint_items(problem)`.
- Search is the engine's simulated annealing with an automatic temperature.
  `on_new_best(step, Score)` replaces `on_new_best(state)`, and the returned
  `Score` is also an iterable, comparable (hard, soft) pair.
- Times, durations, priorities and costs are exact int64. A derived value that
  leaves int64 raises `OverflowError`, and `to_core()` refuses inputs beyond
  |2**62|. A solve that raises leaves the schedule in data mode.
- CP-SAT: the 12 hard rules other than max queue time and max cycle time are
  structural. Missing setup time is enforced pairwise.

#### sports-league-scheduling

- Renames: `matches_on_same_day` → `team_conflict_cost` and
  `classic_matches` → `workday_classic_matches_cost`; the hop and run rules
  gain `_cost`.
- The Python `Breakdown` and `compute_breakdown()` are removed; use
  `constraint_costs()`. The C++ `Breakdown` members are renamed.
- CP-SAT: the workday-derby default weight is fixed (1 → 1000, as in the
  engine), so CP-SAT objectives for defaults with workday derbies change.

#### task-assigning

- Renames:
  - `no_missing_skills` → `required_skill_cost`
  - `minimize_unassigned_tasks` → `unassigned_tasks_cost`
  - `minimize_makespan` → `squared_makespan_cost`: the sum of squared end
    times
  - the priority end-time constraints gain `_cost`
- `unassigned_tasks_cost` is medium (it was hard at weight 1), with the
  `below_hard` fold of 1e9. `required_skill_cost` default weight 1e9 → 1,
  because the 1e9 is now the fold.
- Explicit weights are in units of their level:
  - `RequiredSkillCost(weight=w)` is w × 1e9 engine-hard units per missing
    skill.
  - `RequiredSkillCost(hard=False)` is 1 soft unit per missing skill.
  - A soft constraint made hard decodes into `Score.hard`.
- `squared_makespan_cost` keeps its default weight of 1e7, so it stays above
  the priority terms.
- C++: `priorityWeight()` moved to `ta/constraints.hpp`.
  `PriorityTaskEndTime` takes `optimaru::Level` instead of `bool hard`.
- CP-SAT: the levels are hard/medium/soft (were hard, soft0, soft1, soft2).
  `hard=False` on `unassigned_tasks_cost` or `required_skill_cost` lands on
  soft.

#### tournament-scheduling

- Renames:
  - `one_assignment_per_date_per_team` → `team_conflict_cost`
  - `unavailability_penalty` → `team_unavailability_cost`
  - `fair_assignment_count_per_team` → `assignment_count_imbalance_cost`
  - `evenly_confrontation_count` → `confrontation_count_imbalance_cost`

  The fact class `UnavailabilityPenalty` is no longer shadowed by a spec
  class.
- `assignment_count_imbalance_cost` is medium (fold 1e6).
- CP-SAT: `build_model()` is new. The two imbalance costs still minimise a
  variance surrogate (documented).

#### vehicle-routing

- Renames:
  - `maximize_visits_assigned` → `unassigned_visits_cost`
  - `service_finished_after_max_end_time` → `max_end_time_cost`
  - `vehicle_end_time` → `vehicle_max_end_time_cost`
  - `minimize_travel_time` → `travel_time_cost`
  - the other `minimize_*` constraints become `*_cost`
  - the others gain `_cost`; `vehicle_fixed_cost` is unchanged
- `unassigned_visits_cost` is medium (fold 1e9). The opt-in objectives are
  `enclosing_circle_radius_cost`, `route_duration_cost` and `latency_cost`.
- On the `constraints=` path, `plan.score` and `on_new_best` report the
  unfolded levels.

### Removed APIs

- Per-constraint breakdowns other than `constraint_costs()`:
  - conference-scheduling, meeting-scheduling and sports-league-scheduling:
    `compute_breakdown()` and `Breakdown`
  - school-timetabling: `canon.compute_breakdown()` and `canon.Breakdown`
  - food-packaging: `analyze()` and `ConstraintBreakdown`
  - order-picking: `ConstraintTotals` and `calculate_totals()`
- The six ports' simulated-annealing knobs (`max_iterations`,
  `stagnation_limit`, `temperature`, `cooling_rate`), the `anneal` /
  `configure` helpers, the `SolverConfig` keyword constructor, `DEFAULTS` and
  the top-level `score`. Also knapsack's `_greedy_selection`, and
  quadratic-assignment's `common.py` and pure `_core.py`.
- Problem-level weight fields:
  - facility-location: `setup_cost_weight` and `distance_from_facility_weight`
  - bin-packing: `capacity_weight`, `used_bin_weight`, `color_weight` and
    `conflict_weight`
  - school-timetabling: `canon.Timetable.weights`
- Python spec ClassVars `default_hard` / `default_weight` / `default_level`,
  and per-package copies of the spec base.
- school-timetabling: `CPSAT_ALIASES`, `STRUCTURAL_IN_ENGINE`, `ENGINE_ONLY`
  and `resolve_name`.
- employee-scheduling: the hand-written `*Rule` models and the JSON `policy`
  field.
- flight-crew-scheduling: the C++ `*_WEIGHT` constants.
- Engine: the `(name, weight, bool hard)` constraint constructors and the
  assignable `isHard` member (use `level` / `setLevel()` / `isHard()`).
  `pythonName()` no longer maps spaces and dashes.
- cpsat-common: the ClassVar/instance default path of `ConstraintPlugin`, and
  `ConstraintRegistry.objective(default_weights=...)`.

### Constraint renames

The complete table (249 rows), from the 0.2.0 rename table. *Old name* is the
0.1.x Python/catalog name (school-timetabling also lists the CP-SAT/server/DB
spelling, and its quickstart model had display labels only), *New name* is the
0.2.0 Python name. The C++ structs follow the new class names and the C++ registry
names are their camelCase form (`BedConflictCost` -> `bedConflictCost`). No old
name is accepted any more.

| Use case | Old name | New name | Old class | New class |
| --- | --- | --- | --- | --- |
| assembly-line-balancing | `station_capacity` | `station_capacity_cost` | `StationCapacity` | `StationCapacityCost` |
| assembly-line-balancing | `task_precedence` | `task_precedence_cost` | `TaskPrecedence` | `TaskPrecedenceCost` |
| assembly-line-balancing | `station_count_limit` | `max_stations_cost` | `StationCountLimit` | `MaxStationsCost` |
| assembly-line-balancing | `task_station_bounds` | `task_station_bounds_cost` | `TaskStationBounds` | `TaskStationBoundsCost` |
| assembly-line-balancing | `fixed_task_stations` | `fixed_task_stations_cost` | `FixedTaskStations` | `FixedTaskStationsCost` |
| assembly-line-balancing | `forbidden_task_stations` | `forbidden_task_stations_cost` | `ForbiddenTaskStations` | `ForbiddenTaskStationsCost` |
| assembly-line-balancing | `same_station_groups` | `same_station_groups_cost` | `SameStationGroups` | `SameStationGroupsCost` |
| assembly-line-balancing | `different_station_pairs` | `different_station_pairs_cost` | `DifferentStationPairs` | `DifferentStationPairsCost` |
| assembly-line-balancing | `minimize_stations` | `used_stations_cost` | `MinimizeStations` | `UsedStationsCost` |
| assembly-line-balancing | `workload_balance` | `workload_imbalance_cost` | `WorkloadBalance` | `WorkloadImbalanceCost` |
| bed-allocation | `same_bed_in_same_night` | `bed_conflict_cost` | `SameBedInSameNight` | `BedConflictCost` |
| bed-allocation | `female_in_male_room` | `female_in_male_room_cost` | `FemaleInMaleRoom` | `FemaleInMaleRoomCost` |
| bed-allocation | `male_in_female_room` | `male_in_female_room_cost` | `MaleInFemaleRoom` | `MaleInFemaleRoomCost` |
| bed-allocation | `different_gender_in_same_gender_room_in_same_night` | `mixed_gender_room_cost` | `DifferentGenderInSameGenderRoomInSameNight` | `MixedGenderRoomCost` |
| bed-allocation | `department_minimum_age` | `department_min_age_cost` | `DepartmentMinimumAge` | `DepartmentMinAgeCost` |
| bed-allocation | `department_maximum_age` | `department_max_age_cost` | `DepartmentMaximumAge` | `DepartmentMaxAgeCost` |
| bed-allocation | `required_patient_equipment` | `required_patient_equipment_cost` | `RequiredPatientEquipment` | `RequiredPatientEquipmentCost` |
| bed-allocation | `assign_every_patient_to_a_bed` | `unassigned_stays_cost` | `AssignEveryPatientToABed` | `UnassignedStaysCost` |
| bed-allocation | `preferred_maximum_room_capacity` | `preferred_max_room_capacity_cost` | `PreferredMaximumRoomCapacity` | `PreferredMaxRoomCapacityCost` |
| bed-allocation | `department_specialty` | `department_specialty_cost` | `DepartmentSpecialty` | `DepartmentSpecialtyCost` |
| bed-allocation | `department_specialty_not_first_priority` | `department_specialty_priority_cost` | `DepartmentSpecialtyNotFirstPriority` | `DepartmentSpecialtyPriorityCost` |
| bed-allocation | `preferred_patient_equipment` | `preferred_patient_equipment_cost` | `PreferredPatientEquipment` | `PreferredPatientEquipmentCost` |
| bin-packing | `bin_capacity` | `bin_capacity_cost` | `BinCapacity` | `BinCapacityCost` |
| bin-packing | `color_compatibility` | `color_conflict_cost` | `ColorCompatibility` | `ColorConflictCost` |
| bin-packing | `item_conflicts` | `item_conflict_cost` | `ItemConflicts` | `ItemConflictCost` |
| bin-packing | `minimize_bins` | `used_bins_cost` | `MinimizeBins` | `UsedBinsCost` |
| conference-scheduling | `talk_type_compatibility` | `talk_type_incompatibility_cost` | `TalkTypeCompatibility` | `TalkTypeIncompatibilityCost` |
| conference-scheduling | `room_unavailable_timeslot` | `room_unavailability_cost` | `RoomUnavailableTimeslot` | `RoomUnavailabilityCost` |
| conference-scheduling | `room_conflict` | `room_conflict_cost` | `RoomConflict` | `RoomConflictCost` |
| conference-scheduling | `speaker_unavailable_timeslot` | `speaker_unavailability_cost` | `SpeakerUnavailableTimeslot` | `SpeakerUnavailabilityCost` |
| conference-scheduling | `speaker_conflict` | `speaker_conflict_cost` | `SpeakerConflict` | `SpeakerConflictCost` |
| conference-scheduling | `talk_prerequisite_talks` | `talk_prerequisite_talks_cost` | `TalkPrerequisiteTalks` | `TalkPrerequisiteTalksCost` |
| conference-scheduling | `talk_mutually_exclusive_talks_tags` | `talk_mutually_exclusive_talks_tags_cost` | `TalkMutuallyExclusiveTalksTags` | `TalkMutuallyExclusiveTalksTagsCost` |
| conference-scheduling | `consecutive_talks_pause` | `consecutive_talks_pause_cost` | `ConsecutiveTalksPause` | `ConsecutiveTalksPauseCost` |
| conference-scheduling | `crowd_control` | `crowd_control_risk_cost` | `CrowdControl` | `CrowdControlRiskCost` |
| conference-scheduling | `speaker_required_timeslot_tags` | `speaker_required_timeslot_tags_cost` | `SpeakerRequiredTimeslotTags` | `SpeakerRequiredTimeslotTagsCost` |
| conference-scheduling | `speaker_prohibited_timeslot_tags` | `speaker_prohibited_timeslot_tags_cost` | `SpeakerProhibitedTimeslotTags` | `SpeakerProhibitedTimeslotTagsCost` |
| conference-scheduling | `talk_required_timeslot_tags` | `talk_required_timeslot_tags_cost` | `TalkRequiredTimeslotTags` | `TalkRequiredTimeslotTagsCost` |
| conference-scheduling | `talk_prohibited_timeslot_tags` | `talk_prohibited_timeslot_tags_cost` | `TalkProhibitedTimeslotTags` | `TalkProhibitedTimeslotTagsCost` |
| conference-scheduling | `speaker_required_room_tags` | `speaker_required_room_tags_cost` | `SpeakerRequiredRoomTags` | `SpeakerRequiredRoomTagsCost` |
| conference-scheduling | `speaker_prohibited_room_tags` | `speaker_prohibited_room_tags_cost` | `SpeakerProhibitedRoomTags` | `SpeakerProhibitedRoomTagsCost` |
| conference-scheduling | `talk_required_room_tags` | `talk_required_room_tags_cost` | `TalkRequiredRoomTags` | `TalkRequiredRoomTagsCost` |
| conference-scheduling | `talk_prohibited_room_tags` | `talk_prohibited_room_tags_cost` | `TalkProhibitedRoomTags` | `TalkProhibitedRoomTagsCost` |
| conference-scheduling | `theme_track_conflict` | `theme_track_conflict_cost` | `ThemeTrackConflict` | `ThemeTrackConflictCost` |
| conference-scheduling | `theme_track_room_stability` | `theme_track_room_change_cost` | `ThemeTrackRoomStability` | `ThemeTrackRoomChangeCost` |
| conference-scheduling | `sector_conflict` | `sector_conflict_cost` | `SectorConflict` | `SectorConflictCost` |
| conference-scheduling | `audience_type_theme_track_conflict` | `audience_type_theme_track_conflict_cost` | `AudienceTypeThemeTrackConflict` | `AudienceTypeThemeTrackConflictCost` |
| conference-scheduling | `content_audience_level_flow_violation` | `content_audience_level_flow_violation_cost` | `ContentAudienceLevelFlowViolation` | `ContentAudienceLevelFlowViolationCost` |
| conference-scheduling | `content_conflict` | `content_conflict_cost` | `ContentConflict` | `ContentConflictCost` |
| conference-scheduling | `same_day_talks` | `different_day_talks_cost` | `SameDayTalks` | `DifferentDayTalksCost` |
| conference-scheduling | `popular_talks` | `popular_talks_room_size_cost` | `PopularTalks` | `PopularTalksRoomSizeCost` |
| conference-scheduling | `speaker_preferred_timeslot_tags` | `speaker_preferred_timeslot_tags_cost` | `SpeakerPreferredTimeslotTags` | `SpeakerPreferredTimeslotTagsCost` |
| conference-scheduling | `speaker_undesired_timeslot_tags` | `speaker_undesired_timeslot_tags_cost` | `SpeakerUndesiredTimeslotTags` | `SpeakerUndesiredTimeslotTagsCost` |
| conference-scheduling | `talk_preferred_timeslot_tags` | `talk_preferred_timeslot_tags_cost` | `TalkPreferredTimeslotTags` | `TalkPreferredTimeslotTagsCost` |
| conference-scheduling | `talk_undesired_timeslot_tags` | `talk_undesired_timeslot_tags_cost` | `TalkUndesiredTimeslotTags` | `TalkUndesiredTimeslotTagsCost` |
| conference-scheduling | `speaker_preferred_room_tags` | `speaker_preferred_room_tags_cost` | `SpeakerPreferredRoomTags` | `SpeakerPreferredRoomTagsCost` |
| conference-scheduling | `speaker_undesired_room_tags` | `speaker_undesired_room_tags_cost` | `SpeakerUndesiredRoomTags` | `SpeakerUndesiredRoomTagsCost` |
| conference-scheduling | `talk_preferred_room_tags` | `talk_preferred_room_tags_cost` | `TalkPreferredRoomTags` | `TalkPreferredRoomTagsCost` |
| conference-scheduling | `talk_undesired_room_tags` | `talk_undesired_room_tags_cost` | `TalkUndesiredRoomTags` | `TalkUndesiredRoomTagsCost` |
| conference-scheduling | `speaker_makespan` | `speaker_makespan_cost` | `SpeakerMakespan` | `SpeakerMakespanCost` |
| conference-scheduling | `audience_type_diversity` | `audience_type_diversity_cost` | `AudienceTypeDiversity` | `AudienceTypeDiversityCost` |
| conference-scheduling | `audience_level_diversity` | `audience_level_diversity_cost` | `AudienceLevelDiversity` | `AudienceLevelDiversityCost` |
| conference-scheduling | `language_diversity` | `language_diversity_cost` | `LanguageDiversity` | `LanguageDiversityCost` |
| employee-scheduling | `required_skill` | `required_skill_cost` | `RequiredSkill` / `RequiredSkillRule` | `RequiredSkillCost` |
| employee-scheduling | `unavailable_employee` | `employee_unavailability_cost` | `UnavailableEmployee` / `UnavailableWorkerRule` | `EmployeeUnavailabilityCost` |
| employee-scheduling | `overlapping_shift` | `shift_overlap_cost` | `OverlappingShift` / `OverlappingShiftRule` | `ShiftOverlapCost` |
| employee-scheduling | `one_shift_per_day` | `max_one_shift_per_day_cost` | `MaxOneShiftPerDay` / `MaxOneShiftPerDayRule` | `MaxOneShiftPerDayCost` |
| employee-scheduling | `min_rest` | `min_rest_cost` | `MinimumRest` / `MinimumRestRule` | `MinRestCost` |
| employee-scheduling | `undesired_day` | `undesired_day_cost` | `UndesiredDay` / `UndesiredDayRule` | `UndesiredDayCost` |
| employee-scheduling | `desired_day` | `desired_day_cost` | `DesiredDay` / `DesiredDayRule` | `DesiredDayCost` |
| employee-scheduling | `desired_shift` | `desired_shift_cost` | `DesiredShift` / `DesiredShiftRule` | `DesiredShiftCost` |
| employee-scheduling | `balance_assignments` | `shift_count_imbalance_cost` | `BalanceShiftAssignments` / `BalanceAssignmentsRule` | `ShiftCountImbalanceCost` |
| employee-scheduling | `max_consecutive_days` | `max_consecutive_days_cost` | `MaxConsecutiveDays` / `MaxConsecutiveDaysRule` | `MaxConsecutiveDaysCost` |
| employee-scheduling | `min_consecutive_days` | `min_consecutive_days_cost` | `MinConsecutiveDays` / `MinConsecutiveDaysRule` | `MinConsecutiveDaysCost` |
| employee-scheduling | `max_consecutive_off` | `max_consecutive_days_off_cost` | `MaxConsecutiveOff` / `MaxConsecutiveOffRule` | `MaxConsecutiveDaysOffCost` |
| employee-scheduling | `rest_after_consecutive_work` | `rest_after_consecutive_work_cost` | `RestAfterConsecutiveWork` / `RestAfterConsecutiveWorkRule` | `RestAfterConsecutiveWorkCost` |
| employee-scheduling | `max_weekly_minutes` | `max_weekly_minutes_cost` | `MaxWeeklyMinutes` / `MaxWeeklyMinutesRule` | `MaxWeeklyMinutesCost` |
| employee-scheduling | `min_weekly_minutes` | `min_weekly_minutes_cost` | `MinWeeklyMinutes` / `MinWeeklyMinutesRule` | `MinWeeklyMinutesCost` |
| employee-scheduling | `min_weekly_rest_days` | `min_weekly_rest_days_cost` | `MinWeeklyRestDays` / `MinWeeklyRestDaysRule` | `MinWeeklyRestDaysCost` |
| employee-scheduling | `assignment_cost` | `assignment_cost` | `AssignmentCost` / `AssignmentCostRule` | `AssignmentCost` |
| employee-scheduling | `unassigned_shift` | `unassigned_shifts_cost` | `UnassignedShift` / `UnassignedShiftRule` | `UnassignedShiftsCost` |
| employee-scheduling | `balance_min_max_gap` | `shift_count_gap_cost` | `BalanceMinMaxGap` / `BalanceMinMaxGapRule` | `ShiftCountGapCost` |
| employee-scheduling | `balance_weekly_minutes_gap` | `weekly_minutes_gap_cost` | `BalanceWeeklyMinutesGap` / `BalanceWeeklyMinutesGapRule` | `WeeklyMinutesGapCost` |
| employee-scheduling | `shift_coverage` | `shift_coverage_cost` | `ShiftCoverageRule` (models.py only) | `ShiftCoverageCost` |
| employee-scheduling | `location_run` | `location_run_length_cost` | `LocationRunRule` (models.py only) | `LocationRunLengthCost` |
| employee-scheduling | `rest_after_location_run` | `rest_after_location_run_cost` | `RestAfterLocationRunRule` (models.py only) | `RestAfterLocationRunCost` |
| employee-scheduling | `location_run_gap` | `location_run_gap_cost` | `LocationRunGapRule` (models.py only) | `LocationRunGapCost` |
| employee-scheduling | `worker_location_count` | `worker_location_count_cost` | `WorkerLocationCountRule` (models.py only) | `WorkerLocationCountCost` |
| employee-scheduling | `day_pattern` | `day_pattern_cost` | `DayPatternRule` (models.py only) | `DayPatternCost` |
| employee-scheduling | `target_days_off` | `target_days_off_cost` | `TargetDaysOffRule` (models.py only) | `TargetDaysOffCost` |
| employee-scheduling | `worker_day_count_in_range` | `worker_day_count_in_range_cost` | `WorkerDayCountInRuleRange` (models.py only; typo) | `WorkerDayCountInRangeCost` |
| employee-scheduling | `balance_location` | `location_imbalance_cost` | `BalanceLocationRule` (models.py only) | `LocationImbalanceCost` |
| employee-scheduling | `worker_pair_same_shift` | `worker_pairing_cost` | `WorkerPairSameShiftRule` (models.py only) | `WorkerPairingCost` |
| facility-location | `facility_capacity` | `facility_capacity_cost` | `FacilityCapacity` | `FacilityCapacityCost` |
| facility-location | `facility_setup_cost` | `facility_setup_cost` | `FacilitySetupCost` | `FacilitySetupCost` |
| facility-location | `distance_from_facility` | `distance_from_facility_cost` | `DistanceFromFacility` | `DistanceFromFacilityCost` |
| flight-crew-scheduling | `required_skill` | `required_skill_cost` | `RequiredSkill` | `RequiredSkillCost` |
| flight-crew-scheduling | `flight_conflict` | `flight_conflict_cost` | `FlightConflict` | `FlightConflictCost` |
| flight-crew-scheduling | `transfer_between_two_flights` | `transfer_between_two_flights_cost` | `TransferBetweenTwoFlights` | `TransferBetweenTwoFlightsCost` |
| flight-crew-scheduling | `employee_unavailability` | `employee_unavailability_cost` | `EmployeeUnavailability` | `EmployeeUnavailabilityCost` |
| flight-crew-scheduling | `first_assignment_not_departing_from_home` | `first_assignment_not_departing_from_home_cost` | `FirstAssignmentNotDepartingFromHome` | `FirstAssignmentNotDepartingFromHomeCost` |
| flight-crew-scheduling | `last_assignment_not_arriving_at_home` | `last_assignment_not_arriving_at_home_cost` | `LastAssignmentNotArrivingAtHome` | `LastAssignmentNotArrivingAtHomeCost` |
| food-packaging | `max_end_date_time` | `max_end_time_cost` | `MaxEndDateTime` | `MaxEndTimeCost` |
| food-packaging | `operator_cleaning_conflict` | `operator_cleaning_conflict_cost` | `OperatorCleaningConflict` | `OperatorCleaningConflictCost` |
| food-packaging | `ideal_end_date_time` | `after_ideal_end_time_cost` | `IdealEndDateTime` | `AfterIdealEndTimeCost` |
| food-packaging | `maximize_jobs_assigned` | `unassigned_jobs_cost` | `MaximizeJobsAssigned` | `UnassignedJobsCost` |
| food-packaging | `minimize_makespan` | `squared_makespan_cost` | `MinimizeMakespan` | `SquaredMakespanCost` |
| knapsack | `capacity` | `knapsack_capacity_cost` | `Capacity` | `KnapsackCapacityCost` |
| knapsack | `resource_capacities` | `resource_capacity_cost` | `ResourceCapacities` | `ResourceCapacityCost` |
| knapsack | `mandatory_items` | `mandatory_items_cost` | `MandatoryItems` | `MandatoryItemsCost` |
| knapsack | `forbidden_items` | `forbidden_items_cost` | `ForbiddenItems` | `ForbiddenItemsCost` |
| knapsack | `selection_group_bounds` | `selection_group_bounds_cost` | `SelectionGroupBounds` | `SelectionGroupBoundsCost` |
| knapsack | `item_dependencies` | `item_dependency_cost` | `ItemDependencies` | `ItemDependencyCost` |
| knapsack | `maximize_value` | `selected_value_cost` | `MaximizeValue` | `SelectedValueCost` |
| maintenance-scheduling | `crew_conflict` | `crew_conflict_cost` | `CrewConflict` | `CrewConflictCost` |
| maintenance-scheduling | `min_start_date` | `min_start_date_cost` | `MinStartDate` | `MinStartDateCost` |
| maintenance-scheduling | `max_end_date` | `max_end_date_cost` | `MaxEndDate` | `MaxEndDateCost` |
| maintenance-scheduling | `before_ideal_end_date` | `before_ideal_end_date_cost` | `BeforeIdealEndDate` | `BeforeIdealEndDateCost` |
| maintenance-scheduling | `after_ideal_end_date` | `after_ideal_end_date_cost` | `AfterIdealEndDate` | `AfterIdealEndDateCost` |
| maintenance-scheduling | `tag_conflict` | `tag_conflict_cost` | `TagConflict` | `TagConflictCost` |
| max-cut | `fixed_partition` | `fixed_partition_cost` | `FixedPartition` | `FixedPartitionCost` |
| max-cut | `same_partition_pairs` | `same_partition_pairs_cost` | `SamePartitionPairs` | `SamePartitionPairsCost` |
| max-cut | `different_partition_pairs` | `different_partition_pairs_cost` | `DifferentPartitionPairs` | `DifferentPartitionPairsCost` |
| max-cut | `partition_size_bounds` | `partition_size_bounds_cost` | `PartitionSizeBounds` | `PartitionSizeBoundsCost` |
| max-cut | `partition_balance` | `partition_imbalance_cost` | `PartitionBalance` | `PartitionImbalanceCost` |
| max-cut | `maximize_cut_weight` | `cut_weight_cost` | `MaximizeCutWeight` | `CutWeightCost` |
| meeting-scheduling | `room_conflict` | `room_conflict_cost` | `RoomConflict` | `RoomConflictCost` |
| meeting-scheduling | `avoid_overtime` | `overtime_cost` | `AvoidOvertime` | `OvertimeCost` |
| meeting-scheduling | `required_attendance_conflict` | `required_attendance_conflict_cost` | `RequiredAttendanceConflict` | `RequiredAttendanceConflictCost` |
| meeting-scheduling | `required_room_capacity` | `room_capacity_cost` | `RequiredRoomCapacity` | `RoomCapacityCost` |
| meeting-scheduling | `start_and_end_on_same_day` | `day_crossing_cost` | `StartAndEndOnSameDay` | `DayCrossingCost` |
| meeting-scheduling | `required_and_preferred_attendance_conflict` | `required_and_preferred_attendance_conflict_cost` | `RequiredAndPreferredAttendanceConflict` | `RequiredAndPreferredAttendanceConflictCost` |
| meeting-scheduling | `preferred_attendance_conflict` | `preferred_attendance_conflict_cost` | `PreferredAttendanceConflict` | `PreferredAttendanceConflictCost` |
| meeting-scheduling | `do_meetings_as_soon_as_possible` | `meeting_end_time_cost` | `DoMeetingsAsSoonAsPossible` | `MeetingEndTimeCost` |
| meeting-scheduling | `one_break_between_consecutive_meetings` | `consecutive_meetings_cost` | `OneBreakBetweenConsecutiveMeetings` | `ConsecutiveMeetingsCost` |
| meeting-scheduling | `overlapping_meetings` | `meeting_overlap_cost` | `OverlappingMeetings` | `MeetingOverlapCost` |
| meeting-scheduling | `assign_larger_rooms_first` | `larger_room_capacity_cost` | `AssignLargerRoomsFirst` | `LargerRoomCapacityCost` |
| meeting-scheduling | `room_stability` | `room_change_cost` | `RoomStability` | `RoomChangeCost` |
| order-picking | `required_number_of_buckets` | `trolley_capacity_cost` | `RequiredNumberOfBuckets` | `TrolleyCapacityCost` |
| order-picking | `minimize_distance_from_previous_pick_task` | `distance_from_previous_pick_task_cost` | `MinimizeDistanceFromPreviousPickTask` | `DistanceFromPreviousPickTaskCost` |
| order-picking | `minimize_distance_from_last_pick_task_to_path_origin` | `distance_from_last_pick_task_to_path_origin_cost` | `MinimizeDistanceFromLastPickTaskToPathOrigin` | `DistanceFromLastPickTaskToPathOriginCost` |
| order-picking | `minimize_order_split_by_trolley` | `order_split_by_trolley_cost` | `MinimizeOrderSplitByTrolley` | `OrderSplitByTrolleyCost` |
| pickup-delivery | `paired_request` | `paired_request_cost` | `PairedRequest` | `PairedRequestCost` |
| pickup-delivery | `same_vehicle` | `same_vehicle_cost` | `SameVehicle` | `SameVehicleCost` |
| pickup-delivery | `pickup_before_dropoff` | `pickup_before_dropoff_cost` | `PickupBeforeDropoff` | `PickupBeforeDropoffCost` |
| pickup-delivery | `vehicle_capacity` | `vehicle_capacity_cost` | `VehicleCapacity` | `VehicleCapacityCost` |
| pickup-delivery | `service_finished_after_max_end_time` | `max_end_time_cost` | `ServiceFinishedAfterMaxEndTime` | `MaxEndTimeCost` |
| pickup-delivery | `unassigned_requests` | `unassigned_requests_cost` | `UnassignedRequests` | `UnassignedRequestsCost` |
| pickup-delivery | `minimize_travel_time` | `travel_time_cost` | `MinimizeTravelTime` | `TravelTimeCost` |
| pickup-delivery | `max_travel_time` | `max_travel_time_cost` | `MaxTravelTime` | `MaxTravelTimeCost` |
| pickup-delivery | `max_route_duration` | `max_route_duration_cost` | `MaxRouteDuration` | `MaxRouteDurationCost` |
| pickup-delivery | `vehicle_end_time` | `vehicle_max_end_time_cost` | `VehicleEndTime` | `VehicleMaxEndTimeCost` |
| pickup-delivery | `max_stops` | `max_stops_cost` | `MaxStops` | `MaxStopsCost` |
| pickup-delivery | `max_route_radius` | `max_route_radius_cost` | `MaxRouteRadius` | `MaxRouteRadiusCost` |
| pickup-delivery | `minimize_enclosing_circle_radius` | `enclosing_circle_radius_cost` | `MinimizeEnclosingCircleRadius` | `EnclosingCircleRadiusCost` |
| pickup-delivery | `min_capacity_utilization` | `min_capacity_utilization_cost` | `MinCapacityUtilization` | `MinCapacityUtilizationCost` |
| pickup-delivery | `minimize_route_duration` | `route_duration_cost` | `MinimizeRouteDuration` | `RouteDurationCost` |
| pickup-delivery | `minimize_latency` | `latency_cost` | `MinimizeLatency` | `LatencyCost` |
| pickup-delivery | `minimize_lateness` | `squared_order_age_cost` | `MinimizeLateness` | `SquaredOrderAgeCost` |
| pickup-delivery | `vehicle_fixed_cost` | `vehicle_fixed_cost` | `VehicleFixedCost` | `VehicleFixedCost` |
| pickup-delivery | `pinned_stops` | `pinned_stops_cost` | `PinnedStops` | `PinnedStopsCost` |
| pickup-delivery | `required_vehicle` | `required_vehicle_cost` | `RequiredVehicle` | `RequiredVehicleCost` |
| project-job-scheduling | `non_renewable_resource_capacity` | `non_renewable_resource_capacity_cost` | `NonRenewableResourceCapacity` | `NonRenewableResourceCapacityCost` |
| project-job-scheduling | `renewable_resource_capacity` | `renewable_resource_capacity_cost` | `RenewableResourceCapacity` | `RenewableResourceCapacityCost` |
| project-job-scheduling | `total_project_delay` | `project_delay_cost` | `TotalProjectDelay` | `ProjectDelayCost` |
| project-job-scheduling | `total_makespan` | `makespan_cost` | `TotalMakespan` | `MakespanCost` |
| quadratic-assignment | `fixed_assignments` | `pinned_assignments_cost` | `FixedAssignments` | `PinnedAssignmentsCost` |
| quadratic-assignment | `forbidden_assignments` | `forbidden_assignments_cost` | `ForbiddenAssignments` | `ForbiddenAssignmentsCost` |
| quadratic-assignment | `location_capacities` | `location_capacity_cost` | `LocationCapacities` | `LocationCapacityCost` |
| quadratic-assignment | `minimize_assignment_cost` | `assignment_cost` | `MinimizeAssignmentCost` | `AssignmentCost` |
| quadratic-assignment | `minimize_flow_distance` | `flow_distance_cost` | `MinimizeFlowDistance` | `FlowDistanceCost` |
| school-timetabling | `unassigned_lesson` (engine) / `weekly_period_counts` (API, CP-SAT, DB, penalties key) | `unassigned_lessons_cost` | `UnassignedLesson` (engine) / `WeeklyPeriodCounts` (server, CP-SAT) | `UnassignedLessonsCost` |
| school-timetabling | `class_conflict` / `class_group_conflicts` | `class_conflict_cost` | `ClassConflict` / `ClassGroupConflicts` | `ClassConflictCost` |
| school-timetabling | `teacher_conflict` / `teacher_conflicts` | `teacher_conflict_cost` | `TeacherConflict` / `TeacherConflicts` | `TeacherConflictCost` |
| school-timetabling | `slot_not_allowed` (engine only) | `disallowed_slot_cost` | `SlotNotAllowed` | `DisallowedSlotCost` |
| school-timetabling | `room_capacity` | `room_capacity_cost` | `RoomCapacity` | `RoomCapacityCost` |
| school-timetabling | `course_daily_limit` / `daily_course_limits` | `course_max_periods_per_day_cost` | `CourseDailyLimit` / `DailyCourseLimits` | `CourseMaxPeriodsPerDayCost` |
| school-timetabling | `block_adjacency` / `block_periods` | `block_adjacency_cost` | `BlockAdjacency` / `BlockPeriods` | `BlockAdjacencyCost` |
| school-timetabling | `teacher_load_limit` / `teacher_workload` | `teacher_max_periods_cost` | `TeacherLoadLimit` / `TeacherWorkload` | `TeacherMaxPeriodsCost` |
| school-timetabling | `teacher_consecutive_limit` / `teacher_consecutive_limits` | `teacher_max_consecutive_periods_cost` | `TeacherConsecutiveLimit` / `TeacherConsecutiveLimits` | `TeacherMaxConsecutivePeriodsCost` |
| school-timetabling | `teacher_gap` | `teacher_gap_cost` | `TeacherGap` | `TeacherGapCost` |
| school-timetabling | `teacher_workday` | `teacher_workday_cost` | `TeacherWorkday` | `TeacherWorkdayCost` |
| school-timetabling | `teacher_free_day` | `teacher_free_day_cost` | `TeacherFreeDay` | `TeacherFreeDayCost` |
| school-timetabling | `class_gap` | `class_gap_cost` | `ClassGap` | `ClassGapCost` |
| school-timetabling | `avoided_period` | `avoided_period_cost` | `AvoidedPeriod` | `AvoidedPeriodCost` |
| school-timetabling | `non_preferred_period` | `non_preferred_period_cost` | `NonPreferredPeriod` | `NonPreferredPeriodCost` |
| school-timetabling | `fixed_slots` | `fixed_slots_cost` | `FixedSlots` (server, CP-SAT only) | `FixedSlotsCost` |
| school-timetabling | `parallel_groups` | `parallel_groups_cost` | `ParallelGroups` (server, CP-SAT only) | `ParallelGroupsCost` |
| school-timetabling (quickstart model) | — (label roomConflict; evaluate.py key room_conflict) | `room_conflict_cost` | — | `RoomConflictCost` |
| school-timetabling (quickstart model) | — (label teacherConflict) | `teacher_conflict_cost` | — | `TeacherConflictCost` |
| school-timetabling (quickstart model) | — (label studentGroupConflict; key group_conflict) | `class_conflict_cost` | — | `ClassConflictCost` |
| school-timetabling (quickstart model) | — (label teacherUnavailable) | `teacher_unavailability_cost` | — | `TeacherUnavailabilityCost` |
| school-timetabling (quickstart model) | — (label teacherRoomStability; key stability) | `teacher_room_change_cost` | — | `TeacherRoomChangeCost` |
| school-timetabling (quickstart model) | — (label teacherTimeEfficiency; key efficiency) | `teacher_time_efficiency_cost` | — | `TeacherTimeEfficiencyCost` |
| school-timetabling (quickstart model) | — (label studentGroupSubjectVariety; key variety) | `class_subject_repeat_cost` | — | `ClassSubjectRepeatCost` |
| semiconductor-scheduling | `machine_eligibility` | `machine_eligibility_cost` | `MachineEligibility` | `MachineEligibilityCost` |
| semiconductor-scheduling | `lot_route_precedence` | `lot_route_precedence_cost` | `LotRoutePrecedence` | `LotRoutePrecedenceCost` |
| semiconductor-scheduling | `machine_capacity` | `machine_capacity_cost` | `MachineCapacity` | `MachineCapacityCost` |
| semiconductor-scheduling | `batch_capacity` | `batch_capacity_cost` | `BatchCapacity` | `BatchCapacityCost` |
| semiconductor-scheduling | `batch_family_compatibility` | `batch_family_conflict_cost` | `BatchFamilyCompatibility` | `BatchFamilyConflictCost` |
| semiconductor-scheduling | `setup_transitions` | `missing_setup_time_cost` | `SetupTransitions` | `MissingSetupTimeCost` |
| semiconductor-scheduling | `maintenance_windows` | `machine_unavailability_cost` | `MaintenanceWindows` | `MachineUnavailabilityCost` |
| semiconductor-scheduling | `maximum_queue_time` | `max_queue_time_cost` | `MaximumQueueTime` | `MaxQueueTimeCost` |
| semiconductor-scheduling | `minimum_queue_time` | `min_queue_time_cost` | `MinimumQueueTime` | `MinQueueTimeCost` |
| semiconductor-scheduling | `inter_tool_transport` | `missing_transport_time_cost` | `InterToolTransport` | `MissingTransportTimeCost` |
| semiconductor-scheduling | `auxiliary_resource_capacity` | `auxiliary_resource_capacity_cost` | `AuxiliaryResourceCapacity` | `AuxiliaryResourceCapacityCost` |
| semiconductor-scheduling | `auxiliary_resource_availability` | `auxiliary_resource_unavailability_cost` | `AuxiliaryResourceAvailability` | `AuxiliaryResourceUnavailabilityCost` |
| semiconductor-scheduling | `pinned_assignments` | `pinned_assignments_cost` | `PinnedAssignments` | `PinnedAssignmentsCost` |
| semiconductor-scheduling | `maximum_cycle_time` | `max_cycle_time_cost` | `MaximumCycleTime` | `MaxCycleTimeCost` |
| semiconductor-scheduling | `machine_preference` | `machine_preference_cost` | `MachinePreference` | `MachinePreferenceCost` |
| semiconductor-scheduling | `minimum_batch_fill` | `min_batch_size_cost` | `MinimumBatchFill` | `MinBatchSizeCost` |
| semiconductor-scheduling | `machine_load_balance` | `machine_load_imbalance_cost` | `MachineLoadBalance` | `MachineLoadImbalanceCost` |
| semiconductor-scheduling | `weighted_tardiness` | `weighted_tardiness_cost` | `WeightedTardiness` | `WeightedTardinessCost` |
| semiconductor-scheduling | `total_cycle_time` | `cycle_time_cost` | `TotalCycleTime` | `CycleTimeCost` |
| semiconductor-scheduling | `makespan` | `makespan_cost` | `Makespan` | `MakespanCost` |
| sports-league-scheduling | `matches_on_same_day` | `team_conflict_cost` | `MatchesOnSameDay` | `TeamConflictCost` |
| sports-league-scheduling | `consecutive_home_matches` | `consecutive_home_matches_cost` | `ConsecutiveHomeMatches` | `ConsecutiveHomeMatchesCost` |
| sports-league-scheduling | `consecutive_away_matches` | `consecutive_away_matches_cost` | `ConsecutiveAwayMatches` | `ConsecutiveAwayMatchesCost` |
| sports-league-scheduling | `repeat_match_on_next_day` | `repeat_match_on_next_day_cost` | `RepeatMatchOnNextDay` | `RepeatMatchOnNextDayCost` |
| sports-league-scheduling | `start_to_away_hop` | `start_to_away_hop_cost` | `StartToAwayHop` | `StartToAwayHopCost` |
| sports-league-scheduling | `home_to_away_hop` | `home_to_away_hop_cost` | `HomeToAwayHop` | `HomeToAwayHopCost` |
| sports-league-scheduling | `away_to_away_hop` | `away_to_away_hop_cost` | `AwayToAwayHop` | `AwayToAwayHopCost` |
| sports-league-scheduling | `away_to_home_hop` | `away_to_home_hop_cost` | `AwayToHomeHop` | `AwayToHomeHopCost` |
| sports-league-scheduling | `away_to_end_hop` | `away_to_end_hop_cost` | `AwayToEndHop` | `AwayToEndHopCost` |
| sports-league-scheduling | `classic_matches` | `workday_classic_matches_cost` | `ClassicMatches` | `WorkdayClassicMatchesCost` |
| task-assigning | `no_missing_skills` | `required_skill_cost` | `NoMissingSkills` | `RequiredSkillCost` |
| task-assigning | `minimize_unassigned_tasks` | `unassigned_tasks_cost` | `MinimizeUnassignedTasks` | `UnassignedTasksCost` |
| task-assigning | `minimize_makespan` | `squared_makespan_cost` | `MinimizeMakespan` | `SquaredMakespanCost` |
| task-assigning | `critical_priority_task_end_time` | `critical_priority_task_end_time_cost` | `CriticalPriorityTaskEndTime` | `CriticalPriorityTaskEndTimeCost` |
| task-assigning | `major_priority_task_end_time` | `major_priority_task_end_time_cost` | `MajorPriorityTaskEndTime` | `MajorPriorityTaskEndTimeCost` |
| task-assigning | `minor_priority_task_end_time` | `minor_priority_task_end_time_cost` | `MinorPriorityTaskEndTime` | `MinorPriorityTaskEndTimeCost` |
| tournament-scheduling | `one_assignment_per_date_per_team` | `team_conflict_cost` | `OneAssignmentPerDatePerTeam` | `TeamConflictCost` |
| tournament-scheduling | `unavailability_penalty` | `team_unavailability_cost` | `UnavailabilityPenalty` | `TeamUnavailabilityCost` |
| tournament-scheduling | `fair_assignment_count_per_team` | `assignment_count_imbalance_cost` | `FairAssignmentCountPerTeam` | `AssignmentCountImbalanceCost` |
| tournament-scheduling | `evenly_confrontation_count` | `confrontation_count_imbalance_cost` | `EvenlyConfrontationCount` | `ConfrontationCountImbalanceCost` |
| vehicle-routing | `vehicle_capacity` | `vehicle_capacity_cost` | `VehicleCapacity` | `VehicleCapacityCost` |
| vehicle-routing | `service_finished_after_max_end_time` | `max_end_time_cost` | `ServiceFinishedAfterMaxEndTime` | `MaxEndTimeCost` |
| vehicle-routing | `maximize_visits_assigned` | `unassigned_visits_cost` | `MaximizeVisitsAssigned` | `UnassignedVisitsCost` |
| vehicle-routing | `minimize_travel_time` | `travel_time_cost` | `MinimizeTravelTime` | `TravelTimeCost` |
| vehicle-routing | `max_travel_time` | `max_travel_time_cost` | `MaxTravelTime` | `MaxTravelTimeCost` |
| vehicle-routing | `max_route_duration` | `max_route_duration_cost` | `MaxRouteDuration` | `MaxRouteDurationCost` |
| vehicle-routing | `vehicle_end_time` | `vehicle_max_end_time_cost` | `VehicleEndTime` | `VehicleMaxEndTimeCost` |
| vehicle-routing | `max_stops` | `max_stops_cost` | `MaxStops` | `MaxStopsCost` |
| vehicle-routing | `max_route_radius` | `max_route_radius_cost` | `MaxRouteRadius` | `MaxRouteRadiusCost` |
| vehicle-routing | `minimize_enclosing_circle_radius` | `enclosing_circle_radius_cost` | `MinimizeEnclosingCircleRadius` | `EnclosingCircleRadiusCost` |
| vehicle-routing | `min_capacity_utilization` | `min_capacity_utilization_cost` | `MinCapacityUtilization` | `MinCapacityUtilizationCost` |
| vehicle-routing | `minimize_route_duration` | `route_duration_cost` | `MinimizeRouteDuration` | `RouteDurationCost` |
| vehicle-routing | `minimize_latency` | `latency_cost` | `MinimizeLatency` | `LatencyCost` |
| vehicle-routing | `vehicle_fixed_cost` | `vehicle_fixed_cost` | `VehicleFixedCost` | `VehicleFixedCost` |
