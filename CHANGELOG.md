# Changelog

## Unreleased

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
    repeat, `--all` runs the archive, `--json` prints the results, and the engine
    runs single-threaded whatever `--workers` says, as the READMEs stated.
  - *Project scheduling.* The RG30 archive is checked by the SHA-256 of its five
    instance files: the archive itself is rebuilt upstream, and the pinned archive
    checksum no longer matched. `--all`, a repeatable `--instance` and `--json`
    are new, and the engine runs single-threaded there too.
  - *Quadratic assignment.* `--all` stopped at `kra30a`: the harness re-scores
    each QAPLIB reference permutation and read every solution file as the
    facility of each location, while four files list the location of each
    facility. Both readings are tried, and the reference is what the permutation
    scores (`kra32`'s file states 88900, its permutation scores 88700).
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
