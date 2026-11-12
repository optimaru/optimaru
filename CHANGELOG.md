# Changelog

## Unreleased

- **Benchmarks: external-solver `v0.3.0`.** The benchmark projects pin the external-solver
  release that follows optimaru 0.5, the links into its sources point at that tag, and
  their lock files record optimaru 0.5.0. In `v0.3.0` the worker count of a CP-SAT `solve`
  is `parallel_solver_count`, the name of the engine's keyword (`num_workers` is still
  read), and its employee scheduling plugins say constraint where they said rule
  (`SchedulePlugin` was `RulePlugin`); the runners and the READMEs here use the new names.
  No table changes: the CP-SAT models are unchanged since `v0.2.3`.
- **Notebooks on 0.5.0.** Every notebook in `examples/` is saved with outputs from optimaru
  0.5.0 and asks for it; `examples/README.md` names that version. The four that build a request
  use the names of 0.5: `time_limit_seconds`, and in employee scheduling
  `default_constraints()`, `model_input.constraints` and `constraint_totals`. Twelve of the
  twenty-two print and draw exactly what they did on 0.4.3, and four move within the noise of
  solves that stop on the clock. The other six are of use cases whose search changed, and some
  of their experiments no longer showed their point; those now run on a step budget or another
  seed: knapsack's comparison of move sets (20,000 steps each, `core_repack_moves` off beside
  the other moves) and its restart rounds, bed allocation's new admissions, food packaging's
  rounds and rush-order replan, maintenance scheduling's short runs, restart rounds and
  urgent-job replan. Maintenance scheduling's comparison of ruin and recreate on and off is
  gone: over 8 and 12 seeds it makes no clear difference there on 0.5.0.
- **Fixed: a solve from a given solution in the seven use cases with stages (#110).** In
  0.5.0 bed allocation, food packaging, knapsack, maintenance scheduling, max-cut, quadratic
  assignment and sports league scheduling ran late acceptance in stages on every solve, also
  one that starts from a solution the caller gave (`with_initial_solution`, a request's
  `initial_solution`). A stage starts over from the start's score, which on such a solve is
  where the search already stands: it loosened nothing and was taken at each stall in the
  place of the ruin-and-recreate kick. A solve whose improvements come from the kicks then
  returned its start once the budget had room for stages.
  - Now only a solve that the construction starts has stages (`optimaru::nothing_placed`, or
    the use case's own seed flag). A solution that places only a part counts as given.
  - The engine also ends the stages by itself when a search stalls before it has found
    anything better than its start, whatever started it: a construction that no move improves
    has none either, which matters to a caller who sets `AcceptanceSettings::history_growth`
    directly.
  - Measured, a food packaging week with one new job, four seeds: improved on 4 of 4 at every
    step limit in 0.4.3 and again now, on 0 of 4 from 200,000 steps up in 0.5.0.
  - Measured, a bed allocation plan with five new stays, 12 seeds: a mean medium score of
    -10.33 after 10 million steps in 0.4.3 and now (the same table, seed by seed), -10.58 in
    0.5.0, where 2 of the 12 seeds ended worse under a larger step limit than under a smaller
    one.
  - On 36 instances of the seven benchmark ladders a solve from a given plan finds exactly
    what a build without stages finds, new best by new best. The 46 instances solved from
    the construction end on the same score as in 0.5.0 at 300,000 and at 3 million steps, so
    the benchmark tables stand.
  - The notebooks' restart experiments were moved to step budgets for 0.5.0 (the entry
    above) and have not been run again on this.
- **Bin packing: `TradeMove` and a fill tie-break.** `used_bins_cost` counts bins, so every
  packing with the same number of bins has the same score, and the search wandered from one
  to the next: on Falkenauer's triplets it ended one bin above the best known count. Two
  additions, both on by default. `TradeMove` (`bp/moves.hpp`, `SolverConfig.trade_moves`)
  trades up to two items of one bin for up to two of another so that the fuller of the two
  bins gets fuller, or, when no trade does that, so that both keep their loads with other
  items. And late acceptance takes a step that keeps the score when it does not lower the sum
  of the squared bin loads, and otherwise one time in eight (`bp/fill_late_acceptance.hpp`,
  `SolverConfig.fill_tie_break`). That sum is not part of the score: the score, the limits
  and `on_new_best` are as they were. On the 24 sampled Falkenauer instances (two seeds,
  10 s) no run ends above the capacity bound (39 of the 48 did before). The bound is the
  best known count the OR-Library file states for 23 of them, and one below it for
  `u120_19`. A seed's result in bin packing differs from 0.5.0.
- **Benchmarks: the bin packing table measured again.** With the two additions above the
  engine uses the best known number of bins the OR-Library file states on 23 of the 24
  Falkenauer instances of the table (on 6 before) and one bin fewer on `u120_19`, and CP-SAT
  is ahead on none (on the six triplet instances of 60 and 120 items before). The table of
  generated instances in `benchmarks/bin-packing` is measured again as well, with each part
  of the search switched off in turn.
- **Facility location: `SwapFacilityMove`, and a solve from scratch in stages.** The search
  could close a facility (`CloseFacilityMove`) but not put another in its place, so it kept
  the facilities it first settled on: on the 200-consumer comparison instance it ended with
  11 of CP-SAT's 12 facilities, 1.4% behind. `SwapFacilityMove`
  (`SolverConfig.swap_facility_moves`, on by default) opens a closed facility and closes the
  one that serves its surroundings in one move; the closed one's consumers go to their
  nearest facility with room, and each facility with room takes the consumers nearer to it.
  A solve that starts from nothing now also restarts late acceptance with a doubled history
  when it stalls (`late_acceptance_size` is the first stage's length), and from the first
  restart on it draws the macro moves on a tenth of the steps `custom_move_probability`
  allows. A solve from a given assignment has no stages and keeps the full gate.
  - Measured on the six instances of `benchmarks/facility-location` (30 s, three seeds): the
    mean cost above CP-SAT's is 0.07%, from 1.91%.
  - A short solve: after 50,000 steps the mean cost of a 200-consumer instance over 8 seeds
    is 3.10 million, against 3.22 million; it is lower at every step limit from 20,000 to 2
    million, also with 30 and with 1,200 consumers.
  - From a given assignment (36 seeds, 300,000 steps): 0.15% to 0.80% above CP-SAT's cost on
    the 60- and the 200-consumer instance, against 2.40% to 3.00%. A plan that needs one more
    facility for its new consumers keeps its facilities, as before: no move opens a facility
    without closing another.
  - A seed's result in facility location differs from 0.5.0.
- **Engine: `AcceptanceSettings::later_stage_custom_move_share`.** With `history_growth`,
  the gate on a use case's own move operators (`custom_move_probability`) is multiplied by
  this once late acceptance has started over for the first time. A later stage takes worse
  solutions for a long while, the adaptive selector counts outcomes and not what a move
  costs, and macro moves that each make a pass over the solution then take the steps of the
  moves that polish; the first stage, the descent from the start, keeps the gate as set. 1
  by default (no change); facility location sets 0.1.
- **Benchmarks: the facility location table measured again.** The engine has CP-SAT's score
  on the four instances up to 200 consumers (on two before) and is 0.02% and 0.29% behind on
  the two larger ones (1.4% to 3.3% behind on four before).
- **Python: every demo instance is `generate_demo_data()`.** The function that returns a use
  case's demo instance had five names; six use cases already called it `generate_demo_data`,
  five did not: `order_picking.build_demo_data`, `meeting_scheduling.demo_data` (the package,
  `models` and `_core`), `tournament_scheduling.models.demo_data`,
  `conference_scheduling.demo_conference` and `sports_league_scheduling.demo_schedule`. They are
  `generate_demo_data` now, with the same arguments and the same instance. The five old names
  remain as plain aliases of it in the modules that had them, without a warning, and are no
  longer in `__all__`. `tests/test_layout.py` checks from the source that a demo function has
  no other name.

## 0.5.0

**Upgrading from 0.4.** Two things can break a caller; the entries below have the details.
pydantic 2.11 or newer is required (was 2.5). And JSON written by the models uses the new
names only: `time_limit_seconds` and `parallel_solver_count` in a request (were `timelimit`
and `num_workers`), `constraints` and `constraint_totals` in employee scheduling (were
`rules` and `rule_totals`). The earlier names are still read, in JSON, as keywords and as
attributes, so what a caller sends keeps working, and employee scheduling's Python names
with "rule" in them (`default_rules()`, `Rule`, `RULE_MODELS` and the others) are the
constraint names under a second name. Seven use cases search differently, so a seed's
result there differs from 0.4.3: bed allocation, food packaging, knapsack, maintenance
scheduling, max-cut, quadratic assignment and sports league scheduling. So does a solve
with constraints in vehicle routing or pickup and delivery that asks for more than one
parallel start: it runs them now. The entries on notebooks, benchmarks and guides are
about the repository, not the wheel.

- **Vehicle routing and pickup and delivery: parallel starts on a solve with constraints.**
  `parallel_solver_count` (also a request's field; `num_workers` is its earlier name) had no effect when a plan was scored
  with constraint plugins: a list passed as `constraints`, a request with a vehicle limit, a
  pinned stop or a required vehicle, or a plan with groups. Those solves now run that many
  starts, start *i* on `random_seed + i` and on its own copy of the constraints, and return
  the best, as a solve on the core rules does. With one start nothing changes. On generated
  grouped plans four starts end 0.8% to 2.4% shorter than one in the same 4 s (vehicle
  routing, four classes) and 2.1% shorter (pickup and delivery), longer in none of the 25
  pairs. In C++,
  `Solver::solve(plan, makeCalculator)` takes a factory that builds one calculator per
  start; the overload that takes a calculator stays a single start. Vehicle routing's
  `solve_request` leaves visit groups to the engine, as pickup and delivery's does since
  0.4.3. Both solvers refuse a bad `SolverConfig` before the plan is touched: the check ran
  after the unassigned pool was cleared, and in pickup and delivery after the pair
  construction.
- **Notebooks.** Every notebook in `examples/` is saved with outputs from optimaru 0.4.3 and
  asks for it; `examples/README.md` names one version again. Nineteen of the twenty-two show
  what they showed before. Three move within the run-to-run noise of solves that stop on the
  clock, and no text quotes a number that moved: conference scheduling and meeting scheduling
  print slightly different scores, and order picking's chart of rounds takes another path
  through its middle rounds to the same printed end.
- **Benchmarks: the lock files record optimaru 0.4.3.** The benchmark projects install
  optimaru from this checkout, and their locks name its version: they still said 0.4.2 after
  the release, so the first `uv sync` rewrote them. Nothing else moves; the projects stay on
  external-solver `v0.2.3`, which needs no change for 0.4.3.
- **`solve` for a problem, `solve_request` for a pydantic request, in every use case.** The
  pydantic path had two shapes: four use cases took a request through `models.solve_request`,
  the others took their problem model through `solve`, and school timetabling took dicts.
  Every use case now has both entry points, and the package exports both
  (`optimaru.<use case>.solve_request` is `models.solve_request`).
  - *New:* a request model in the other 17 and in school timetabling: `models.<Name>Request`, the problem model as
    `model_input` with the settings of one solve, and `models.solve_request(request, *,
    constraints=None)`, which returns what `solve` returns for that model. School
    timetabling's `TimetableRequest` holds the problem as the new `TimetableProblemModel`
    with `mode`, `weights` and `initial_solution`, and `solve_request` returns the new
    `TimetableResultModel`; the package now imports its models with itself.
  - *Renamed:* the settings of a request have the names of `solve`'s keywords in every use
    case: `time_limit_seconds` (was `timelimit`), `random_seed`, `parallel_solver_count`
    (was `num_workers`), `stop_at_first_feasible`, and the new `full_assert`, beside
    `solver_parameters`. The shared base is `optimaru._models.SolveRequest`. `timelimit` and
    `num_workers` are still accepted as aliases, in JSON, as keywords and as attributes; a
    dump uses the new names. `model_copy(update=...)` reads the old names too, and refuses a name given with its old name.
  - *Employee scheduling:* `ScheduleRequest` has `random_seed` and `stop_at_first_feasible`
    at the top level like the others; `settings.random_seed` and
    `settings.stop_at_first_feasible` are the same fields under another name (a top-level
    value wins once it is given). Its `solve_request` takes `constraints=`, a list of specs
    that replaces `model_input.constraints` for the solve, and it reads `solver_parameters`
    and `full_assert` too.
- **pydantic 2.11 or newer.** The request models read their earlier field names through
  `validate_by_name` / `validate_by_alias`, which pydantic has from 2.11 (the floor was 2.5).
- **Constraints, not rules.** Employee scheduling called its constraints rules in its API and
  guides; every use case now says constraints. `SchedulingInput.rules` is
  `SchedulingInput.constraints`, `default_rules()` is `default_constraints()`, `Rule`,
  `RuleBase` and `RULE_MODELS` are `ConstraintModel`, `ConstraintModelBase` and
  `CONSTRAINT_MODELS`, `rule_model()` is `constraint_model()`, and `RuleTotal` /
  `ScheduleSolution.rule_totals` are `ConstraintTotal` / `constraint_totals`. The earlier
  names are kept as aliases: `rules` and `rule_totals` are read from JSON and as
  attributes (`rule_totals` read-only), and the old functions and classes are the new ones under a second name.
  *Breaking for JSON consumers:* a dump writes the new names only, so code that reads
  `rules` from a dumped input or `rule_totals` from a dumped solution must read
  `constraints` and `constraint_totals`. The same holds for `timelimit` and `num_workers`
  in a dumped request. The guides and docstrings of the other use cases say "core
  constraints", "coupling constraints" and so on where they said rules for constraints.
- **Late acceptance in stages of growing history.** A late acceptance history is a trade
  between converging fast and finding more, and the use cases set one short length (400 to
  3,200 steps), so a small instance converged within a fraction of a second and the kicks
  alone had the rest of the budget: quadratic assignment, which has no kicks, did nothing
  for the last 29.9 of 30 seconds on 14 facilities. The engine's new
  `AcceptanceSettings::history_growth` (C++; off by default) restarts late acceptance from
  the start's score with the history that many times longer once the search has converged
  and found no new best for half as long as the next stage would take, as long as that
  stage fits in the budget; the best solution is kept as always. It is on, at 2, in **food
  packaging, knapsack, max-cut, quadratic assignment and sports league scheduling**, where
  `late_acceptance_size` is now the first stage's length. Measured on every instance of
  their five benchmark ladders at 30 s over three seeds: 62 runs better, 33 equal, 1 worse
  (a knapsack by 1 in 17,470). In the published tables sports league scheduling has 7 to
  13% less travel, quadratic assignment ties CP-SAT up to 16 facilities (it was behind at
  14 and 16), and the mean gap to the best known values of the 28 QAPLIB instances with a
  reference goes from 2.22% to 0.22%, 18 of them at the best known value. Every other use
  case searches exactly as before. In the five, the ruin & recreate kicks fire differently
  from the first step on: stagnation counts from the last improving move, and a kick waits
  at least two histories. With a time limit, the stage a solve reaches depends on the
  clock; a solve whose step limit ends it before the clock matters repeats for a seed. A
  stage does not know the `unimproved_*` limits: one shorter than a stage ends the solve
  inside it, with the best solution so far. A history stops growing at 2^20 entries (16 MB
  per start).
- **Knapsack: `CoreRepackMove`.** Sorted by value per weight, what the greedy leaves to
  decide sits in a narrow band around the first item the capacity leaves out, and an
  improving exchange there is two items for one or larger, which no flip or swap reaches:
  the 500 to 5,000 item instances of the benchmark stayed 1 to 3 below their optimum. The
  new move (`ks/moves.hpp`, `SolverConfig.core_repack_moves`, on by default) takes a window
  of that order around the break item, frees the capacity its selected items hold and
  fills it with the most valuable subset of the window, a small 0/1 knapsack solved exactly
  by dynamic programming over the weight; the acceptor judges the trade. It looks at the
  capacity only (the score judges resources, groups and dependencies), declines when its
  table would pass a million cells, and is drawn on at most one step in a thousand. All
  seven benchmark instances now end on CP-SAT's value, the four large ones within 0.01 s.
  Parallel starts run the solver's own search (a registered move does not reach the
  engine's multi-start threads).
- **Late acceptance in stages: bed allocation and maintenance scheduling too.** Measured
  on their eleven benchmark instances at 30 s over three seeds. Maintenance scheduling: 13
  of 15 runs better, the mean soft score 24%, 27%, 3.4%, 3.8% and 0.1% better from the
  smallest instance up (the two worse runs: 28 jobs on seed 1, the published one, and 48
  jobs by 2 in 259). Bed allocation: 2.8%, 1.7% and 3.7% better up to 183 stays, the
  20-stay instance on CP-SAT's value on every seed, and within 0.3% either way on the three
  larger instances, where few stages fit. Facility location was measured too and stays as
  it was: its 60-consumer instance got worse on all three seeds.
- **Stages: a restart no longer delays the next one.** The patience before a kick or a
  stage is read from the average gap between new bests, and the gap from the last best of
  one stage to the first of the next spans the whole restart, so after a stage that found
  something the search waited about twice that stage's length before it moved on. That gap
  is now left out of the average. Measured on the six ladders with stages and kicks or
  restarts to gain from, at 30 s over three seeds: sports league scheduling 2.0% less
  travel on average (11 runs better, 4 worse), max-cut 5 better and none worse, bed
  allocation and food packaging level (+0.4%, +0.1%), maintenance scheduling level (6
  better, 5 worse), quadratic assignment the same on every run. The stages are their own
  engine header now, `accept/history_stages.hpp` (`optimaru::HistoryStages`), with unit
  tests of the lengths, the cap and when a stage is due.
- **Benchmarks: the seven tables with stages and QAPLIB measured on this build**
  (2026-11-12). Of their 46 instances the engine has the best score on 45 (18 of them ties)
  and CP-SAT on one: the 14-job maintenance schedule, -3145 against -3133. In 0.4.3,
  before the entries above, CP-SAT had the better score on 14 of the 43 instances those
  tables had.
  QAPLIB: mean gap 0.26% (2.22% before), 17 of 28 at the best known value.
- **Benchmarks: three more rungs, five tables and QAPLIB measured again.** `random25`
  (quadratic assignment), `g1600` (max-cut) and `teams12` (sports league scheduling) join
  their comparison ladders.
- **Docs: adding a constraint, step by step, in every use case.** Each use case has
  `docs/use-cases/<slug>/extending.md` ("Adding a constraint" under its name on the docs
  site). It takes one new constraint through that use case's own files in six steps: change
  a constraint that exists from Python, write a new one as a component, check it, make it
  cheaper per move, put it on the default fast path, measure. Every snippet comes from a patch
  that was compiled and tested, and every figure was measured, on 0.4.3, on the use case's
  demo or a generated instance; each guide says how its two scoring paths differ (which
  constraints the default path scores, what a step is, whether a pick at weight 0 is still
  computed, when `on_reset()` runs) instead of repeating one text. The "Adding a hard or soft
  cost" list in each use case's README is now a pointer to that guide, and
  `docs/extending.md` stays the reference for the contracts. `tests/test_layout.py` requires
  the guide for every use case and holds it to the code: the number of constraints its first
  paragraph states, and every test and C++ test target it names.

## 0.4.3

- **Vehicle routing: visit groups with a partial order.** Visits that share a `group`
  (`VisitModel.group`, a name; `Visit.group_index` on a plan) are one job: they ride one
  vehicle together, or all stay unassigned. `after` (`Visit.after_visit_indices`) lists the
  visits of the same group served before a visit, a partial order on the group; an order
  with a cycle or one that leaves the group is refused. Two new hard constraints at weight
  1000 score it: `visit_group_cost` (the grouped visits not where most of their group is,
  the same counting as pickup-delivery's `request_group_cost`, now the engine's
  `optimaru::GroupPlacement`) and `visit_order_cost` (per vehicle, the pairs served the
  wrong way round). Both answer the move previews. Groups are placed whole before the
  engine's construction, and the use case's first domain move, `groupRuinRecreate`, carries
  a group between vehicles as one unit (`SolverConfig.group_ruin_recreate`,
  `group_ruin_max_visits`); it is registered only for a plan with groups, so a plan without
  them is solved exactly as before. Every entry point scores a grouped input on the
  constraint assembly (`solve_request`, `vrp.solve(plan)`, `Solver.solve(plan)`), on a
  single start, and `calculate_score(plan)` reads a grouped plan the same way (C++:
  `vrp::calculateScore`). Engine-only: the CP-SAT reference reads neither field.
  `list_constraints()` and `constraint_info()` have two more rows.
- **Every acceptance rule has its parameters.** Vehicle routing, quadratic assignment and
  semiconductor scheduling let `SolverConfig.acceptor_type` name seven rules, but each bound the
  parameters of only some of them, so `GREAT_DELUGE` kept its water level at the start's score,
  `STEP_COUNTING_HILL_CLIMBING` ran at a fixed size, and in vehicle routing
  `SIMULATED_ANNEALING` started at temperature 0, which is hill climbing. The three now share
  one set of attributes with the same defaults: `entity_tabu_size` (7),
  `simulated_annealing_starting_temperature` ((0, 0) = automatic) and
  `simulated_annealing_temperature_fraction` (0.01), `great_deluge_water_level_step` ((0, 0)),
  `step_counting_hill_climbing_size` (400), `record_to_record_start_gap` (0.02) and
  `record_to_record_end_gap` (0.0), each checked when it is set. Only one default search
  changes: simulated annealing in vehicle routing, which now starts at 1% of the start's soft
  level (the driving time; the medium level of unassigned visits does not count). In C++ the
  fields are the new `optimaru::AcceptanceConfig` base (`optimaru/solver_config.hpp`), the
  automatic temperature is the engine's (`AcceptanceSettings::start_temperature_fraction`), and
  `optimaru::python::bind_acceptor_type()` binds the enum.
- **Pickup and delivery joins the shared pieces.** Its `SolverConfig` has the acceptance
  attributes above (it had `acceptor_type` and the two record-to-record gaps), so simulated
  annealing no longer starts at temperature 0 there either: it starts at 1% of the start's
  driving time. In C++ its `Score` is the engine's `HardMediumSoftScore`, and its bindings use
  `bind_score()`, `bind_constraints()` and `bind_acceptor_type()`. Its two record-to-record
  gaps are now checked when set (finite and >= 0, a `ValueError` otherwise); nothing else
  changes in Python.
- **Pickup and delivery: a grouped plan is scored the same by every entry point.** The C++
  `Solver::solve(plan)`, and with it Python's `Solver(config).solve(plan)`, scored a plan with
  request groups on the seven core rules, which leave `request_group_cost` out, so a split
  group came back as feasible; only `pdp.solve(plan)` and `solve_request` switched to the
  default constraints. The engine now switches itself (`PdpRoutePlan::hasRequestGroups()`),
  on a single start, and `calculate_score(plan)` (C++: `pdp::calculateScore`) reads a grouped
  plan the same way instead of overwriting its score with the core rules'. As in vehicle
  routing, a request alone in its group changes nothing: such a plan stays on the core rules,
  where 0.4.2 switched for any `group`. With `pair_construction_heuristic` off, a grouped plan
  now starts as any other does there: the engine's construction places every stop, where
  `pdp.solve(plan)` used to leave the caller's unassigned pool for the search to route.
  `request_group_cost` answers the move previews since it counts with
  `optimaru::GroupPlacement`, and answers them piecewise for a plan without groups; the
  default set still applies every move, because its three request-coupling constraints do
  not preview.
- **Pickup and delivery notebook re-run on 0.4.2.** `examples/pickup-delivery.ipynb` is saved
  with outputs from optimaru 0.4.2: the same problem now ends at 5.2 hours of driving, where
  0.4.1 ended at 5.4. Its rounds of more search are 10,000 steps each (were 0.3 seconds),
  because 0.4.2 settles within one 0.3-second round and both lines of the chart stayed flat.
- **Benchmarks: the projects pin external-solver `v0.2.3`.** That release lists the engine's
  new `request_group_cost` among the pickup-delivery constraints the CP-SAT model has no
  plugin for (selecting it there is an unknown-constraint error, as with
  `required_vehicle_cost`); its models are those of `v0.2.2`, so no published result moves.
  The benchmark projects' lock files record optimaru 0.4.2. Six of the projects (conference
  scheduling, food packaging, meeting scheduling, order picking, sports league scheduling,
  task assigning) had no `[build-system]` table, so uv did not install them and the commands
  their READMEs give (`uv run <slug>-compare`, `uv run pytest -q`) did not run; they have
  the table the other fifteen have now, and declare pytest in a `dev` dependency group, so
  a plain `uv sync` installs it. `tests/test_layout.py` holds every benchmark project to a
  `[build-system]` table and to one external-solver tag, and the links into external-solver
  to that tag.
- **Layout and names: one form for what every use case has.** No behavior changes.
  - The C++ quality benches left the package tree: `optimaru/<use case>/cpp/bench/` is now
    `benchmarks/<slug>/cpp/<ns>_quality_bench.cpp` (bed allocation, conference scheduling,
    meeting scheduling, school timetabling; conference scheduling's file was
    `quality_bench.cpp`), next to the comparison it belongs to, as the C++ tests are under
    `tests/<use case>/cpp/`. The bench targets and their binaries' paths under `build/` are
    the same.
    A use case's `cpp/` holds only `CMakeLists.txt`, `bindings.cpp`, `include/` and `src/`.
  - School timetabling's C++ tests carry the use case's prefix like every other target:
    `st_canonical_test`, `st_canonical_plugin_test` (were `canonical_test`,
    `canonical_plugin_test`; files, targets and ctest names).
  - Semiconductor scheduling's tests use the names the other use cases' tests use:
    `fixtures/web_demo.json` (was `data/web-demo.json`) and `cpp/fab_fixtures.hpp` (was
    `fab_test_support.hpp`).
  - Three benchmark projects are named after their use case like the other seventeen
    that run an external-solver model:
    `flight_crew_scheduling_cpsat`, `project_job_scheduling_cpsat` and
    `sports_league_scheduling_cpsat` (were `flight_crew_cpsat`, `project_job_cpsat`,
    `sports_league_cpsat`), with the commands `flight-crew-scheduling-compare`,
    `project-job-scheduling-compare`, `project-job-scheduling-public-bench` and
    `sports-league-scheduling-compare`. `scripts/publish_benchmarks.py` no longer keeps a
    table of exceptions for them.
  - Every benchmark project declares pytest in the `dev` dependency group. Ten declared it
    as an extra and school timetabling's CP-SAT project in a group named `test`, so a plain
    `uv sync` there installed no pytest and `uv run pytest -q` could not start. The READMEs
    and CI now say `uv sync` without `--all-groups --all-extras` / `--extra dev`.
  - Guide titles: the use case's name in sentence case, without "(C++ / Python)"; every
    `python.md` is `# optimaru.<use case> (Python)`; six `cpsat/README.md` titles follow the
    other nine (`<Use case> CP-SAT`).
  - `tests/test_layout.py` holds the `cpp/` contents and the `dev` group.

## 0.4.2

- **Pickup and delivery: request groups.** Requests that share a `group`
  (`DeliveryModel.group`, a name; `Delivery.group_index` on a plan) are one job: they ride
  one vehicle together, or all stay unassigned. Their stops stay ordinary stops, visited in
  any order between other requests' stops. The new constraint `request_group_cost` (hard,
  weight 1000, flippable) counts the grouped requests that are not where most of their
  group is. Construction and the request moves place a group as one unit, each request at
  its cheapest slot pair once the earlier ones are in, and leave all of it unassigned when
  no single vehicle has room; the pair move, the pair exchange and the pair ruin stay
  inside a group. A `required_vehicle` on one request, a pinned stop or a request already
  on an initial route binds the whole group to that vehicle, and two requests of a group
  bound to different vehicles (by a required vehicle or a pinned stop) are refused.
  `solve_request` scores groups whenever a request names one, and so does `pdp.solve(plan)`
  for a plan with a `group_index` set (both then score the whole default set). Engine-only:
  the CP-SAT reference does not read `group`. `list_constraints()` and `constraint_info()`
  have one more row, before `unassigned_requests_cost`. A plan without groups is solved
  exactly as before.
- **Engine: `MemberChangeGlobalConstraint`.** A global constraint that keeps its aggregates
  from joins and leaves alone derives from `optimaru::MemberChangeGlobalConstraint<Solution>`
  (`optimaru/score/decomposed_objective.hpp`) and writes `on_member_changed()` only; ten use
  cases declared that base themselves. The comment that opens each use case's
  `constraints/common.hpp` now holds what is particular to the use case and points to the
  engine header and `docs/extending.md` for the rest. No score changes.
- **Bindings: the part every use case repeated is two engine helpers.**
  `optimaru::python::bind_constraints()` binds `constraint_costs()`, `list_constraints()` and
  `constraint_info()` off a default assembly, and `bind_score()`
  (`optimaru/python/score_binding.hpp`) binds the `Score` class of the engine's two score
  types. The Python functions, their arguments and their results are the same; the
  docstrings of `_core.constraint_costs` and `_core.list_constraints` are now the same text
  in every use case.
- **Engine: the three-level score is one type.** `optimaru::HardMediumSoftScore`
  (`optimaru/score/hard_medium_soft_score.hpp`) replaces the identical `Score` structs that
  bed allocation, food packaging, project job scheduling and vehicle routing each declared,
  with `to_engine_score()`, `to_engine_score_limit()` and `from_engine_score()` for the fold
  into the engine's two levels. Each use case's `Score`, `toEngineScore()` and
  `fromEngineScore()` keep their names and results. `kNoLevelFloor` moves to
  `optimaru/score/levels.hpp`. Nothing changes in Python.
- **Pickup and delivery: the search works in whole requests.** Three new move operators take
  a pickup and its dropoff out of a route together and put them back together, at the
  cheapest feasible slot pair, so the acceptor is offered plans with every request whole
  instead of split ones to reject. `requestRuinRecreate` takes 1 to
  `request_ruin_max_requests` requests out (random, related, the costliest, one route's, or
  adjacent stretches of nearby routes) and reinserts them in random order or by regret, with
  blinking; requests waiting unassigned join the reinsertion. `bundleRelocate` moves up to
  `bundle_max_requests` requests whose pickups follow each other on a route to one other
  route, and `bundleExchange` swaps two such bundles. A slot pair is checked in O(1) from a
  per-route timeline (departures, latest feasible arrivals, loads), against the capacity,
  the time windows and the vehicle limits that are set (`max_end_time`,
  `max_route_duration`, `max_travel_time`, `max_stops`, pinned stops, a required vehicle).
  Construction uses the same scan, so the first plan keeps those vehicle limits too (it
  checked capacity and time windows only). `SolverConfig` gains `request_ruin_recreate`,
  `request_ruin_max_requests` (20), `bundle_moves` and `bundle_max_requests` (3); the two
  switches are on by default, so a seeded solve no longer walks the path it did, and the
  two counts refuse a value below 1. `pdp_demo`'s control flag is `--stop-moves-only` now
  (it was `--no-pair-moves`) and turns every request-level operator off. Both results
  blocks of `benchmarks/pickup-delivery` are measured again (30 s, seed 0). On the 56 Li &
  Lim instances the engine's mean distance goes from 7.5% above the best known to 0.8%
  below it (the harness minimizes distance only, so a plan with more vehicles can be
  shorter than the best known), the worst instance from +22.0% to +2.4%, the instances
  more than 1% above from 43 to 2, and the vehicles used from 485 to 409 (best known:
  402). The generated instances of 25 to 400 requests come out 2% to 10% shorter.
- **Engine (C++): features no use case used are removed.** Gone from the engine headers: the
  chain solution shape (`Shape::Chain`, `ChainObjective`, the chain overload of
  `Solver::solve()`, the `chain::` link helpers), `WeightedObjective`, `explain_score()`,
  `rescore()` and `ScoreBreakdown`, background solves (`solve_in_background()`,
  `BackgroundSolve`) and derived state (`DerivedState`, `Solver::add_derived_state()`, the
  fourth argument of `Editor`). None of the 22 use cases used any of them, so the Python
  package does not change, and a seeded, step-limited solve gives the same result as before.
  What a C++ caller uses instead: `DecomposedObjective::constraint_totals()` for the score
  per constraint, and `Solver::solve()` on a thread of its own, stopped with
  `request_stop()`, for a solve in the background.
- **School timetabling benchmark: the CP-SAT package keeps only what the benchmark runs.**
  `benchmarks/school-timetabling/cpsat` still carried parts of the application it came from
  that no benchmark and no test of the model used: a workbook importer, timetable and
  school-system exports, a timetable inspection report, a sample workbook, and settings read
  from environment variables. They are removed (about 2,250 lines) with the two dependencies
  only they needed (`openpyxl`, `pydantic-settings`). The model, its constraints and the three
  benchmark tracks are unchanged.
- **More instances in every engine-vs-CP-SAT comparison.** Each of the 17
  comparisons on generated or demo instances now runs a ladder of five to
  seven instances of growing size, 99 in all (was 38), and every results
  block is measured again. The smallest rungs are ones both solvers finish;
  on the largest the exact model often returns nothing within the budget,
  and the tables say so. `benchmarks/README.md` now states how a CP-SAT run
  is timed: the 60 s stop past the budget, and the level-by-level solve of
  the models whose score does not fit one 64-bit objective.
  - Conference scheduling, maintenance scheduling, meeting scheduling, sports
    league scheduling and task assigning had only their demo data. Their
    `benchmark.py` has a seeded generator in the shape of that data, and the
    demo instance stays as one rung. Food packaging runs its demo generator at
    six sizes.
  - The benchmark projects pin external-solver `v0.2.2` (was `v0.1.0`; the
    models are the same). Its level-by-level solve used to end without a
    solution when the first level found none in its share of the budget (10 s
    of 30 s with three levels); the first level now keeps the time that is
    left. Two rungs whose whole result was that early end, the 4-line plant of
    food packaging and the 240-task instance of task assigning, are back in
    their ladders, and the five comparisons that use the level-by-level solve
    (conference scheduling, food packaging, meeting scheduling, task assigning,
    tournament scheduling) were measured on that release.
  - The comparison runner takes `--solver engine` or `--solver cp-sat`, so the
    single-threaded engine runs and the CP-SAT runs can be measured side by
    side. The publisher joins the two files (`compare/<slug>.engine.json`,
    `compare/<slug>.cp-sat.json`) after checking that they belong together,
    and refuses a comparison that lacks a run of either solver. Every result
    records its seed and workers.
  - `pickup-delivery-public-bench` and `project-job-public-bench` take `--out
    FILE` and rewrite it after every instance, so an interrupted sweep keeps
    what it measured.
  - The publisher refuses a routing file that was run on chosen instances
    (`--only`) or whose two series used different seeds, and publishes a
    pickup-delivery file of one backend with the other as not run.
  - The SALBP details workbook must name its columns: a missing header is an
    error, no longer a fallback to fixed columns.
- **A parallel start that reaches the target stops the others.** With
  `parallel_solver_count` above 1 and a target (`stop_at_first_feasible`, `best_score_limit`),
  each start used to stop on its own, so `solve()` returned only when the slowest start had
  reached the target or run out the time limit. Now the first start to reach it ends the run:
  the others stop at their next step and hand over their best so far, and the best of all
  starts is returned. That solution can score lower on the levels below the target than
  the best of all starts run to the end did, which is what the earlier return costs. Which
  start gets there first depends on the threads, so in that combination the same seed can
  give a different solution from run to run; without a target, or with one start, a seeded
  solve is as reproducible as before. C++: the same holds for
  `Solver::solve_multistart()` and `optimaru::solve_parallel_starts()`.
- **Benchmarks on whole public data sets.** The public-instance harnesses now run
  whole sets or stated samples instead of a few instances, and every results
  block is measured again on 0.4.1: CVRPLIB Set X (all 100) and Augerat's Set A
  (all 27), the Li & Lim PDPTW set (all 56, beside the best-known solutions),
  RG30 Pat1–Pat5, Falkenauer bin packing (three per class), QAPLIB (the 33
  instances with 30 to 50 facilities) and SALBP-1 (five per data set).
  `scripts/publish_benchmarks.py` writes those tables and one homepage entry per
  data set. What changed in the harnesses:
  - *Vehicle routing.* Set X does not fix the fleet, and the best-known solutions
    of 22 of its instances use more routes than the `-kN` of their name, which the
    harness used as the fleet. Every backend now gets a fleet that cannot bind on
    Set X (30% above the capacity lower bound, plus three); Set A keeps `-kN`.
    `--series a` runs Set A and `--only NAME` picks instances of a series.
  - *Pickup and delivery.* One Li & Lim unit is ten seconds instead of one, so
    travel is rounded to a tenth of a unit and a request of the `lr` and `lrc`
    classes can no longer be left out for free (their 10-unit service time was
    under a whole service minute). `--instance`, `--requests` and `--synthetic`
    repeat, `--all` runs the archive (whole instances unless `--requests` is
    given), `--json` prints the results with each run's outcome, and the engine
    runs single-threaded whatever `--workers` says, as the READMEs stated.
  - *Project scheduling.* The RG30 archive is checked by the SHA-256 of its five
    instance files: the archive itself is rebuilt upstream, and the pinned archive
    checksum no longer matched. `--all`, a repeatable `--instance` and `--json`
    are new, and the engine runs single-threaded there too.
  - *Quadratic assignment.* `--all` stopped at `kra30a`: the harness re-scores
    each QAPLIB reference permutation and read every solution file as the
    facility of each location, while four files list the location of each
    facility. Both readings are tried. `kra32`'s file states 88900 while its
    permutation scores 88700, which is recorded as an erratum; any other
    mismatch still stops the run.
  - *Bin packing.* `--sample-per-class N` picks N evenly spread instances of each
    Falkenauer class.
  - *Assembly line balancing.* The best-known values of the medium (n=50) data
    sets were read from the wrong columns of their details workbook, which has
    one more column; the columns are now found by their headers.
- **Meeting scheduling: `parallel_solver_count` runs that many starts.** It was ignored: the
  solver set the engine's start count and then called the engine's single-start entry point,
  so `solve(..., parallel_solver_count=4)` searched once, on one thread. It now runs the
  engine's multi-start, as the other use cases do: start *i* searches with `random_seed + i`
  and the best score wins. A solve with one start, and one with `constraints=...` (always
  single-start), returns what it returned before.
- **C++: every `Solver::Config` field is snake_case, and the common ones come from a shared
  base (breaking for C++ callers).** A `Config` mixed `time_limit` and `seed` with
  `parallelSolverCount`, `ruinRecreateMaxRuinCount` and `nearbyProbability`; every camelCase
  field is now snake_case (`parallel_solver_count`, `ruin_recreate_max_ruin_count`,
  `nearby_probability`, ...). The fields each of the 22 use cases declared for itself (the
  limits, the seed, the parallel starts, the score checks, `on_new_best`, and the ruin &
  recreate knobs) are declared once, in `optimaru::SolverConfigBase<Score>` and
  `optimaru::RuinRecreateConfig` of the new `optimaru/solver_config.hpp`, which also holds
  what every solver did with them: `check_config()`, `settings_from()` and
  `solve_parallel_starts()`. A use case's `Config` derives from the bases and declares only
  its own knobs. Python is not affected: every `SolverConfig` keeps its attributes and their
  defaults, and a seeded solve walks the same path.
- **Maintenance scheduling: `SolverConfig` gains `ruin_recreate_as_move_weight` and
  `ruin_recreate_as_move_max_ruin`.** The C++ solver already had the two knobs
  (destroy-and-repair as an ordinary move judged by the acceptor; 0, the default, turns it
  off), but Python could not set them.
- **Semiconductor scheduling: a wrong problem type is a `TypeError`.** `solve()`,
  `calculate_score()`, `constraint_costs()` and `constraint_items()` raised `AttributeError`
  for a value that was neither a `SemiconductorSchedule` nor a
  `models.SemiconductorScheduleModel`; they now say which two types they take, as the other
  use cases do.
- **Flight crew scheduling ignores `FCS_*` environment variables.** Three overrides a
  tuning session left in the solver (`FCS_DESCENT_WEIGHT`, `FCS_ASMOVE_WEIGHT`,
  `FCS_ASMOVE_MAXRUIN`) are gone.
- **Engine (C++): unused settings removed.** `AcceptanceSettings::rrt_steps`,
  `rrt_exponential` and `rrt_compare_to_best` were set by nothing; record-to-record travel
  keeps the schedule it always ran, linear against the best score. `Solver::clear_stop()`
  is gone too: a stopped solver stays stopped.
- **Notebooks.** The notebooks in `examples/` are saved with outputs from optimaru 0.4.1 again.
  The conference-scheduling notebook read a `hard` key that `constraint_costs()` no longer
  returns and did not run; a few sentences that named a result the outputs no longer show are
  reworded, and the food-packaging rounds use 50,000 steps.
- **Importing a use case imports its models.** In 15 use cases `import optimaru.<use case>`
  left `optimaru.<use case>.models`, and with it pydantic, for the first call that needed a
  model. They now import it with the package, as `assembly_line_balancing`, `bin_packing`,
  `knapsack`, `max_cut`, `pickup_delivery` and `quadratic_assignment` already did (school
  timetabling, whose `solve()` takes dicts, still does not). pydantic is a dependency of
  `optimaru`, so nothing new has to be installed; the first import of one of the 15 takes
  about 35 ms longer (`vehicle_routing` 15 ms to 48 ms, `employee_scheduling` 25 ms to
  72 ms, measured with `python -X importtime` on one machine), which is the time the first
  model call used to take. On the package,
  `with_initial_solution` (and `vehicle_routing.solve_request`) is now the `models` function
  itself, with its docstring and annotations, instead of a wrapper around it.
- **A star import of five modules binds their own names only.** `models` in `knapsack`,
  `max_cut`, `quadratic_assignment` and `semiconductor_scheduling`, and
  `employee_scheduling.helpers`, had no `__all__`, so `from ... import *` also bound the names
  they import (pydantic's `BaseModel` and `Field`, for example). They now list their own
  classes and functions, as the other modules do. Every `__all__` in the package is sorted.

## 0.4.1

- **One `solve()` for every use case (breaking for seven).**
  `optimaru.<use case>.solve(problem, ...)` now exists in all 22, takes the same
  keywords (`time_limit_seconds`, `random_seed`, `parallel_solver_count`,
  `stop_at_first_feasible`, `full_assert`, `on_new_best`, `config`,
  `constraints`) and returns the solved problem, with the score on it.
  - *Breaking:* in `assembly_line_balancing`, `bin_packing`,
    `facility_location`, `knapsack`, `max_cut`, `quadratic_assignment` and
    `semiconductor_scheduling`, `solve()` returned the `Score`. It now returns
    the problem it was given: write `uc.solve(problem).score` for a compiled
    problem, or read `score_hard` / `score_soft` from the solved pydantic
    model.
  - *Breaking:* given a pydantic model, `solve()` in
    `assembly_line_balancing`, `knapsack`, `max_cut`, `quadratic_assignment`
    and `semiconductor_scheduling` wrote the result into that model. It now
    returns a new solved model and leaves the one passed in as it was, as the
    other use cases do: write `model = uc.solve(model)`. Code that keeps
    reading the model it passed in does not fail, it reads the unsolved
    values. (`calculate_score(model)` still fills in the score fields of the
    model it is given.)
  - *New:* `solve()` in `bin_packing`, `conference_scheduling`,
    `facility_location`, `food_packaging`, `maintenance_scheduling` and
    `sports_league_scheduling` also takes the use case's pydantic problem
    model and returns a solved model, as the others do. (Employee scheduling,
    order picking, pickup & delivery and vehicle routing keep
    `models.solve_request` for their request models.)
  - *New:* `solve()` in `employee_scheduling`, `pickup_delivery` and
    `vehicle_routing`; the `full_assert` keyword in the 15 use cases whose
    `solve()` lacked it; `SolverConfig.full_assert` in those three and
    `SolverConfig.parallel_solver_count` in `employee_scheduling`.
  - *Stricter:* `SolverConfig` in `pickup_delivery` and `vehicle_routing`
    rejects `parallel_solver_count <= 0` and `late_acceptance_size < 0`, and in
    those two and `order_picking` a `ruin_recreate_rewards` list that does not
    hold exactly 4 values, with the `ValueError` the other use cases raise.
- **Engine: repeated instances in `ConstraintAssembly`.** A by-name edit or
  lookup (`weight()`, `level()`, `hard()`, `soft()`, `exponent()`, `offset()`,
  `clip()`, `find()`, `drop()`, and `add()`'s replacement) of a name the
  assembly holds several times now throws `std::invalid_argument`. It used to
  touch the first instance only, so `weight("x", 0)` after `add_instance()`
  left the second instance in the score. New overloads take the instance,
  counted from 0: `weight(name, instance, w)` and the same for the other
  setters, `find(name, instance)` and `drop(name, instance)`.
  `instance_count(name)` says how many there are and `drop_all(name)` removes
  them all. `find(name)` throws for such a name as well, where it used to
  return the first instance; `contains(name)` never throws. A name held once
  behaves as before, and so does the Python API,
  which never edits a repeated name.
- **Engine: repeated picks share the raw cost.** The second and later picks
  of a constraint used to run its cost and delta code once more each. They
  now join as repeats (`ConstraintAssembly::add_repeats()`, which the Python
  bindings use), and `DecomposedObjective` asks one instance of a unit
  constraint per unit and shapes the answer for each pick. Scores do not
  change. A vehicle-routing solve with `TravelTimeCost()` and
  `TravelTimeCost(exponent=2)` took about a quarter more steps per second, and
  one with a third pick at `exponent=3` about half more (400 visits). Picks
  are still computed separately when the engine handles them differently (one with a negative
  threshold next to one without), for constraints with domain parameters
  (employee scheduling), for the members of a compiled delta group, and for
  global constraints. `add_instances()` keeps independent instances.
- **Docs: the cost of a negative threshold.** The guides now say that a
  negative `offset` with `clip=True` (a fixed charge per non-zero unit) is
  recomputed for every unit a move changes, which about halves the steps per
  second of a vehicle-routing solve.
- **Docs and messages.** The guides, docstrings and comments describe each use
  case in its own terms and no longer compare it with the examples it was
  derived from. Two `optimaru.project_job_scheduling` error messages changed
  wording with them (a non-renewable global resource, a standard job with no
  execution mode). No behaviour changes.
- **Repository layout.** The source tree now has the shape of the package it
  builds. `optimaru/` is the one Python package. The engine headers moved from
  `include/optimaru/` to `optimaru/cpp/optimaru/` (the include path stays
  `#include "optimaru/..."`), and each use case moved from
  `use-cases/<slug>/{cpp,python}` into its subpackage `optimaru/<use case>/`,
  with its C++ in `optimaru/<use case>/cpp/`. Tests moved to `tests/` (engine,
  shared constraint base, one directory per use case), and the guides to
  `docs/`. The standalone per-use-case packages (`vrp_routing`,
  `st_timetabling`, ...) and their 22 copies of the constraint-spec base are
  gone: everything imports `optimaru.<use case>`. `setup.py` finds the use cases
  and their sources itself. The installed package and its
  API do not change.
- **License.** A new `THIRD-PARTY-LICENSES` file ships in the wheels and the
  sdist. It carries the notices and license texts of the C++ runtime libraries
  that the wheel repair tools bundle into the musllinux wheels (libstdc++ and
  libgcc, GPL-3.0-or-later with the GCC Runtime Library Exception) and the
  Windows wheels (the Microsoft Visual C++ runtime). The 0.4.0 wheels bundle the
  same libraries without it.
- **Builds.** One `uv sync` at the root builds every extension in place, and
  one CMake project (`cmake -B build`) builds the engine and every use case's
  C++ tests. `OPTIMARU_USE_CASES` narrows either build to some use cases.
- **Docs.** `docs/extending.md` is a step-by-step guide to adding a move or a
  cost, and a documentation site (MkDocs, with an API reference generated from
  the sources) is built from `docs/` and published on GitHub Pages. The engine
  layout in the README lists every directory. `CONTRIBUTING.md` and a section
  near the top of the README give the short version of adding a constraint or
  a move and link to the guide.
- **Bindings.** The part of `SolverConfig` that every use case shares (the
  limits, the seed, `parallel_solver_count`, `late_acceptance_size`,
  `full_assert`, `ruin_recreate_rewards`) is bound by one helper,
  `optimaru::python::bind_solver_config`, instead of 22 copies. Names, defaults
  and error messages are unchanged.
- **Engine internals.** The step loop (`search/local_search.hpp`) and the
  decomposed objective (`score/decomposed_objective.hpp`) are shorter: the
  move mix, the kick's polish loop and the adaptive selectors share one
  sampling and scoring path, the score levels fold in one place, and the
  built-in per-constraint profiler (the undocumented `ES_PROFILE` environment
  variable) is gone; use a sampling profiler instead. A seeded, step-limited
  solve gives the same result as before. Two source-incompatible changes for
  C++ code that includes the engine headers: `ListMove::Kind` enumerators are
  PascalCase like every other engine enum (`Kind::Segment`, `Kind::TailSwap`,
  ...), and `GreedyRepair::candidate_count` is replaced by
  `ruin_candidate_count`.
- **Initial solutions.** `with_initial_solution` in
  `optimaru.flight_crew_scheduling` and `optimaru.project_job_scheduling`
  raises `ValueError` for a problem that repeats an id, like the other use
  cases.

## 0.4.0

First release.

- **Python package.** `pip install optimaru` installs one package with every
  use case as a subpackage (`optimaru.vehicle_routing`,
  `optimaru.school_timetabling`, ...), as prebuilt wheels for Linux, macOS and
  Windows on Python 3.11 and later. No C++ toolchain is needed.
- **Engine.** A header-only C++17 metaheuristic engine under
  `include/optimaru/`, with no dependencies and no domain code:
  - three solution shapes (assignment, sequence and chain) and derived state
    kept up to date around every write;
  - incremental scoring with `Score` (int64 hard, double soft), written as an
    `Objective` or summed from named components (`WeightedObjective`,
    `DecomposedObjective`);
  - construction heuristics (first fit, first fit decreasing, cheapest
    insertion), acceptors (hill climbing, late acceptance, entity tabu,
    simulated annealing, great deluge, step counting hill climbing,
    record-to-record travel) and assignment and sequence moves mixed by weight;
  - nearby selection and adaptive ruin and recreate (random, related, worst,
    SISR and route ruin with regret-k recreate);
  - stop conditions on wall time, stall time, steps, stall steps and a target
    score; multi-start solves and background solves (`solve_in_background()`)
    that report status, stop on request and rethrow a failure from `wait()`;
  - score explanation (`explain_score()`, `rescore()`) and a full-check mode
    (`ScoreChecks::Full`) that compares every incremental score with a full
    recompute.
- **22 use cases**, each a C++ domain with a pybind11 Python package, tests and
  a README: assembly-line balancing, bed allocation, bin packing, conference
  scheduling, employee scheduling, facility location, flight crew scheduling,
  food packaging, knapsack, maintenance scheduling, max-cut, meeting
  scheduling, order picking, pickup and delivery, project-job scheduling,
  quadratic assignment, school timetabling, semiconductor scheduling, sports
  league scheduling, task assigning, tournament scheduling and vehicle routing.
  Each package builds and tests on its own from `use-cases/<slug>/python`.
- **Constraint contract**, the same in every use case:
  - every constraint is one named component (`<Subject>Cost`, Python name
    `<subject>_cost`) whose raw cost is 0 when nothing is violated;
  - `constraint_info()` lists the constraints with their default weight and
    level; `constraint_costs()` returns the raw per-unit costs of a solution,
    and the `constraints.score()` helper re-prices them under any selection
    without solving again;
  - each pick takes five knobs: `weight`, `level` (`"hard"`, `"medium"` where
    the use case has a medium level, or `"soft"`), `exponent`, and `offset` /
    `clip`, which shift the shaping per unit as a threshold
    (`max(0, x - offset) ** exponent`) or as the literal
    `(x - offset) ** exponent`;
  - a constraint may be picked several times, each instance with its own knobs
    and parameters; the instances add up;
  - guards keep scores meaningful: rewards never go hard, the literal offset
    formula and raw costs that can be negative stay off the levels that count
    toward the hard score, structural constraints keep their level, and the
    hard total saturates instead of wrapping.
- **Examples.** One Jupyter notebook per use case in `examples/`, with
  visualizations.
- **Benchmarks.** Every use case but vehicle routing is compared with OR-Tools
  CP-SAT on the same instances and rescored by the use case's own evaluator;
  vehicle routing is compared with the OR-Tools Routing solver on CVRPLIB Set X.
  The CP-SAT models come from the separate
  [external-solver](https://github.com/optimaru/external-solver) repository,
  except school timetabling's, which stays in `benchmarks/`. Pickup and
  delivery, project-job scheduling, bin packing, quadratic assignment,
  assembly-line balancing and vehicle routing also run on public instance sets
  (Li & Lim, RG30, Falkenauer, QAPLIB, SALBP, Augerat), downloaded on demand as
  SHA-256 pinned files.
- **License.** Apache License 2.0. NOTICE states that the engine is a modified
  derivative of Timefold Solver (itself derived from OptaPlanner and OptaPy),
  lists the use cases and benchmark harnesses derived from Timefold
  Quickstarts, and includes pybind11's BSD-3-Clause license, which the
  compiled extensions embed.
