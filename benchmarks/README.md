# Benchmarks

Reproducible comparisons of the engine's use cases against other open-source
solvers. The competing solvers live only here, never in the `optimaru` package.
Instance data is generated deterministically or downloaded on demand
(checksum-verified, kept out of Git), and every returned solution is rescored
by the use case's own evaluator; a solver's self-reported objective is never
taken as the result.

| Benchmark | Compared against | Instances |
| --- | --- | --- |
| [vehicle-routing](vehicle-routing) | OR-Tools Routing | CVRPLIB Set X (100), Augerat Set A (27) |
| [school-timetabling](school-timetabling) | OR-Tools CP-SAT | generated schools, 3 to 60 classes (20) |
| [pickup-delivery](pickup-delivery) | OR-Tools CP-SAT | Li & Lim PDPTW, 100 customers (56) + generated, 10 to 400 requests (21) |
| [project-job-scheduling](project-job-scheduling) | OR-Tools CP-SAT | RG30 RCPSP Pat1–Pat5 (Patterson format) and generated instances of that shape (20) + a generated ladder, 5 sizes (20) |
| [bin-packing](bin-packing) | OR-Tools CP-SAT | Falkenauer u/t (OR-Library), three per class |
| [quadratic-assignment](quadratic-assignment) | OR-Tools CP-SAT | QAPLIB, 30–50 facilities (33, engine only) + generated, 6 to 25 facilities (20) |
| [assembly-line-balancing](assembly-line-balancing) | OR-Tools CP-SAT | SALBP-1 (Otto et al.), five per data set |
| [bed-allocation](bed-allocation) | OR-Tools CP-SAT | generated, 5 sizes (20) |
| [conference-scheduling](conference-scheduling) | OR-Tools CP-SAT | the demo conference + generated, 2 to 5 days (20) |
| [employee-scheduling](employee-scheduling) | OR-Tools CP-SAT | seeded demo (generated), 5 sizes (20) |
| [facility-location](facility-location) | OR-Tools CP-SAT | generated, 5 sizes (20) |
| [flight-crew-scheduling](flight-crew-scheduling) | OR-Tools CP-SAT | generated, 5 sizes (20) |
| [food-packaging](food-packaging) | OR-Tools CP-SAT | the demo plant at 20 sizes |
| [knapsack](knapsack) | OR-Tools CP-SAT | generated, 50 to 5000 items (20) |
| [maintenance-scheduling](maintenance-scheduling) | OR-Tools CP-SAT | the two demo instances + generated, 4 sizes (20) |
| [max-cut](max-cut) | OR-Tools CP-SAT | generated, 30 to 1600 vertices (20) |
| [meeting-scheduling](meeting-scheduling) | OR-Tools CP-SAT | the demo instance + generated, 4 sizes (20) |
| [order-picking](order-picking) | OR-Tools CP-SAT | generated, 5 sizes (20) |
| [semiconductor-scheduling](semiconductor-scheduling) | OR-Tools CP-SAT | generated, 6 to 48 lots (20) |
| [sports-league-scheduling](sports-league-scheduling) | OR-Tools CP-SAT | the demo league + generated, 6 to 18 teams (20) |
| [task-assigning](task-assigning) | OR-Tools CP-SAT | the demo instance + generated, 4 sizes (20) |
| [tournament-scheduling](tournament-scheduling) | OR-Tools CP-SAT | generated, 6 sizes (20) |

The CP-SAT models come from
[external-solver](https://github.com/optimaru/external-solver), installed from
its GitHub release tag (`external_solver.<subpackage>`); the models and their
own tests live there. Two benchmarks are set up differently: school-timetabling
keeps its CP-SAT model (`teacher_scheduling`) and its parity test in
[`school-timetabling/cpsat`](school-timetabling/cpsat), the project that also holds its
comparison (`school_timetabling_cpsat`, `school-timetabling-compare`), and vehicle-routing is
the single `uv` project [`vehicle-routing`](vehicle-routing), which runs the
OR-Tools Routing solver. Every other comparison is a `uv` project in
`benchmarks/<slug>/cpsat` that installs `optimaru` from this checkout (an
editable path dependency, `optimaru = { path = "../../.." }`), so a run always
measures the engine in this checkout. external-solver's pin on the published
release is overridden (`override-dependencies`), so external-solver builds and
scores with the same `optimaru.<subpackage>` modules. Importing the project's
package calls `cpsat_plugins.checkout.use_checkout_engine`, which fails fast if
`optimaru.<subpackage>` comes from anywhere else. `OPTIMARU_USE_CASES=<subpackage>`
before `uv sync` builds only that extension. [`cpsat-common`](cpsat-common) holds
that check, the comparison runner (`run_cli`) and the results file of the
public-instance runners (`write_results`). To pick up a new external-solver release,
bump the `tag` in each project's `[tool.uv.sources]` and run `uv lock`.

The five use cases that also run on public instance sets (pickup & delivery,
project-job scheduling, bin packing, quadratic assignment, assembly-line
balancing) have the runner in the same package, `<use case>_cpsat/public_benchmark.py`,
run as `uv run <slug>-public-bench` from `benchmarks/<slug>/cpsat`. It downloads its
archive on demand, checks it against pinned SHA-256 checksums and caches it in the
package's `.cache/` (ignored by git); `--json` prints the results for the publisher.
Its protocol and results are in `cpsat/PUBLIC_BENCHMARKS.md` (bin packing's in its
README).

Four use cases (bed allocation, conference scheduling, meeting scheduling, school
timetabling) also have an engine-only C++ bench for tuning runs,
`benchmarks/<slug>/cpp/<ns>_quality_bench.cpp`. The use case's `CMakeLists.txt` builds it
(`cmake --build build --target <ns>_quality_bench`, binary in `build/<use case>/`); its
arguments are in the comment at the top of the file. A C++ bench generates no data: every
instance is built in Python and written to a plain token file the bench reads, so a tuning
run solves the problems the comparison solves and an instance is the same on every
platform. `benchmarks/<slug>/dump_instances.py` writes the comparison's instances for bed
allocation, conference scheduling and meeting scheduling (the file format is in its
docstring; conference and meeting scheduling's also prints the score of a fixed assignment,
which `<ns>_quality_bench crosscheck <file>` reproduces, and CI compares the two on every
instance with the script's `--check`, on a pull request that runs that benchmark job); school timetabling's track-1 generator,
`school_timetabling_cpsat/gen_quickstart.py`, writes its own.

A CP-SAT constraint is a plugin that subclasses the use case's own constraint
spec (`<Subject>Cost`), so both solvers take the same selection: the same
names and the same five knobs, `weight` / `level` / `exponent` / `offset` /
`clip`. The plugin declares no defaults; its level, weight, `level_flippable`
and `reward` come from the domain's `constraint_info()`, so the two solvers
cannot disagree on a default. `level="soft"` demotes a hard or medium
constraint to soft on both paths. A constraint picked several times adds one
set of terms per pick, each with its own knobs, and the per-pick costs are
keyed `name`, `name#2`, ... as in the engine. CP-SAT accepts only `exponent=1`,
integral weights and integral offsets, and some constraints are structural in a
CP-SAT model (enforced by its variables, so `level="soft"` is refused there;
school-timetabling, pickup-delivery and semiconductor-scheduling have such
constraints). As in the engine, `clip=False` with a non-zero offset is refused
on hard, and on medium where it counts toward the hard score (task assigning),
with the engine's message, when the objective is built: hard 0 must mean no
violation. Each benchmark's README (or `cpsat/README.md`) lists its own
limits, including the plugins that refuse an offset.

## Instances of the generated comparisons

Every comparison runs a ladder of 20 instances of growing size. The smallest
are ones both solvers finish; on the largest the exact model often returns
nothing within the budget, which the results show as "no solution in time".
Where a use case ships a demo instance it is one rung, and the others come
from a seeded generator in its `benchmark.py`. Most ladders have a few sizes
with several instances each, drawn with different seeds of the generator: the
first instance of a size has the size's name (`medium`) and the others are
numbered from 2 (`medium-2`, `medium-3`); `cpsat_plugins.benchmark.seeded`
builds a ladder from such a list. Food packaging's generator has no seed, so
its ladder is 20 plants of different sizes. `--instance NAME` (repeatable) picks rungs,
and `--solver engine` or `--solver cp-sat` runs one solver only, which lets
the single-threaded engine runs and the CP-SAT runs be measured side by side.
The publisher joins the JSON of the two runs (`compare/<slug>.engine.json` and
`compare/<slug>.cp-sat.json`) into the comparison after checking that they are
of the same instances, budget and seed; it refuses a comparison that lacks a
run of either solver.

## How a CP-SAT run is timed

A CP-SAT run gets the budget as its time limit, which CP-SAT counts from the
start of the search, so building the model comes on top. The comparison runner
stops the run as a whole, model construction included, 60 s past the budget
(`--cpsat-grace`; 90 s of wall clock at a 30 s budget). A run stopped there,
and a run whose search ends without a solution, is reported as "no solution
in time". The public-instance runners have no such stop.

Where the score levels of an instance do not fit one 64-bit objective, the
reference model minimises them one after another, hard first, and gives each
level an equal share of the time that is left (`ObjectiveBuilder.solve` in
external-solver). The first level has no solution to fall back on: when its
share ends without one it keeps the time that is left and stops at the first
solution it finds, so such a run ends without a solution only when the whole
budget passed without one. A later level that finds nothing in its share
returns the solution of the level before it. Food packaging and tournament
scheduling are solved this way on every instance of their ladders, conference
scheduling on every instance but the smallest, task assigning on every instance
but the five of the smallest size, and meeting scheduling on every instance but
two of the smallest (whether the levels fit one objective depends on an
instance's bounds, not on its size alone). Of the 37 rows without a CP-SAT
solution in those five tables, four are runs whose first level found nothing
in the whole budget: four plants of food packaging, from 5 lines / 75 jobs to
6 lines / 120 jobs. The other 33 are runs the runner stopped 60 s past the
budget, with the model still being built or the search still running (on the
16-meeting instances of meeting scheduling, building the model alone takes
about a minute). All shares come from one clock: it
starts with the first level and keeps running between levels, while each
level's solution is written into the model as the next level's hint. A row
that this split shapes stays in its table, with a note beside it (five plants
of food packaging), and is not left out.
(Before external-solver `v0.2.2` a first level without a solution ended the
run at its share; two rungs were left out of the ladders for that reason, and
both are back.)

The `<slug>-compare` commands in the READMEs use `--seconds 10` for a quick
check, and the public-instance runners show their own budgets. Every results
block, `RESULTS.md` and `PUBLIC_BENCHMARKS.md` state the budget it was measured with (30 s per solver
and instance for most); pass that `--seconds` to reproduce it.

## Publishing results

[`scripts/publish_benchmarks.py`](../scripts/publish_benchmarks.py) writes the
results blocks of these READMEs (between `<!-- results:start -->` and
`<!-- results:end -->`) and the homepage's benchmark data from the JSON the
harnesses print; its docstring lists the file each harness's output goes in.
A refresh may cover some data sets only: the others keep their text and their
date. Two guides quote their table by hand and are updated with it (the
"Latest run" line of `docs/use-cases/knapsack/README.md` and of
`docs/use-cases/max-cut/README.md`).

The published numbers are single runs on one machine. The harnesses were run
side by side, each on its own performance cores: one CP-SAT job at a time
(8 workers) beside two single-threaded jobs, at most ten solver threads on
twelve performance cores, with builds and other work kept to the efficiency
cores (`taskpolicy -b` on macOS). In the generated comparisons that means the
engine's runs and CP-SAT's were made at the same time, not one after the other.

How far a number repeats differs by use case. The 38 instances of the earlier
generated ladders were measured on two days: the engine's score was the same
both times on all of them, and CP-SAT's differed on 8 (eight workers under a
time limit). The `v0.2.2` run on 2026-11-11 measured 25 instances again (the
five comparisons that use the level-by-level solve, on a later build of the
engine): the engine's score was the same on 24 of them (the 24-meeting
instance of meeting scheduling moved by half a percent, `-3049soft` to
`-3035soft`, on engine code that had changed in between), and CP-SAT's result
on 18. The engine's own result does move elsewhere: by a few percent on the
larger semiconductor instances (see that README), and on the 400-request
synthetic pickup-delivery instance it did with the stop-granular search those
days measured (54 vehicles one day, 55 the other; the request-level search
that replaced it returned the same plan in two runs on 2026-11-11). Bed
allocation, facility location (since its table of 2026-11-12), food packaging,
knapsack, maintenance scheduling, max-cut, quadratic assignment and sports
league scheduling search in stages of growing late acceptance history, and
which stage a solve reaches under a time limit
depends on the clock: of the 35 instances of five of them, measured twice on
2026-11-11 with an earlier build, the engine's score was the same on 32
(the 10-line plant of food packaging moved by 0.01%, the 400-vertex max-cut by
0.1%, the 18-team league by 5%), and all 33 QAPLIB results were the same.
QAPLIB's table is of one run on 2026-11-12. One seed says little about a
change of the search: QAPLIB's `tai30b` is 1.4% above its best known value in
that run where the run before reached it.
The 20-instance tables of the generated comparisons are of one run on
2026-11-12, three engine lanes beside one CP-SAT lane. On the 102 instances
that the tables had before, the engine's score was the same as in the tables it
replaced on 91; the 11 that moved are bed allocation `xlarge`, conference
`days5`, facility location `medium`, food packaging `lines8`, maintenance
`medium` and `medium-2`, max-cut `g120` and the semiconductor instances of 18
to 48 lots, all by under 1% except maintenance `medium` (8% better) and the
semiconductor ones (up to 2.4%).
Read a small margin in any table as noise.

