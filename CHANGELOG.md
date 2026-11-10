# Changelog

## Unreleased

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
  layout in the README lists every header.

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
