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
| [school-timetabling](school-timetabling) | OR-Tools CP-SAT | generated S/M/L schools |
| [pickup-delivery](pickup-delivery) | OR-Tools CP-SAT | Li & Lim PDPTW, 100 customers (56) + generated |
| [project-job-scheduling](project-job-scheduling) | OR-Tools CP-SAT | RG30 RCPSP Pat1–Pat5 (Patterson format) + generated |
| [bin-packing](bin-packing) | OR-Tools CP-SAT | Falkenauer u/t (OR-Library), three per class |
| [quadratic-assignment](quadratic-assignment) | OR-Tools CP-SAT | QAPLIB, 30–50 facilities (33, engine only) + generated |
| [assembly-line-balancing](assembly-line-balancing) | OR-Tools CP-SAT | SALBP-1 (Otto et al.), five per data set |
| [bed-allocation](bed-allocation) | OR-Tools CP-SAT | generated, 6 sizes |
| [conference-scheduling](conference-scheduling) | OR-Tools CP-SAT | the demo conference + generated, 2 to 5 days |
| [employee-scheduling](employee-scheduling) | OR-Tools CP-SAT | seeded demo (generated), 6 sizes |
| [facility-location](facility-location) | OR-Tools CP-SAT | generated, 6 sizes |
| [flight-crew-scheduling](flight-crew-scheduling) | OR-Tools CP-SAT | generated, 6 sizes |
| [food-packaging](food-packaging) | OR-Tools CP-SAT | the demo plant at 5 sizes |
| [knapsack](knapsack) | OR-Tools CP-SAT | generated, 50 to 5000 items |
| [maintenance-scheduling](maintenance-scheduling) | OR-Tools CP-SAT | the two demo instances + generated |
| [max-cut](max-cut) | OR-Tools CP-SAT | generated, 30 to 800 vertices |
| [meeting-scheduling](meeting-scheduling) | OR-Tools CP-SAT | the demo instance + generated |
| [order-picking](order-picking) | OR-Tools CP-SAT | generated, 5 sizes |
| [semiconductor-scheduling](semiconductor-scheduling) | OR-Tools CP-SAT | generated, 6 to 48 lots |
| [sports-league-scheduling](sports-league-scheduling) | OR-Tools CP-SAT | the demo league + generated, 6 to 18 teams |
| [task-assigning](task-assigning) | OR-Tools CP-SAT | the demo instance + generated |
| [tournament-scheduling](tournament-scheduling) | OR-Tools CP-SAT | generated, 6 sizes |

The CP-SAT models come from
[external-solver](https://github.com/optimaru/external-solver), installed from
its GitHub release tag (`external_solver.<subpackage>`); the models and their
own tests live there. Two benchmarks are set up differently: school-timetabling
keeps its CP-SAT model (`teacher_scheduling`) and its parity test in
[`school-timetabling/cpsat`](school-timetabling/cpsat), and vehicle-routing is
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

Every comparison runs a ladder of instances of growing size. The smallest are
ones both solvers finish; on the largest the exact model often returns nothing
within the budget, which the results show as "no solution in time". Where a
use case ships a demo instance it is one rung, and the others come from a
seeded generator in its `benchmark.py`. `--instance NAME` (repeatable) picks rungs,
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
external-solver); a first level that finds no solution in its share ends the
run without one. Food packaging and tournament scheduling are solved this way
on every instance of their ladders, conference scheduling, meeting scheduling
and task assigning on every instance but the smallest. A row where that early
end is the whole result is not published: a rung is left out of the ladder
when the first level finds a solution once it has the whole budget (task
assigning above 120 tasks, the 4-line plant of food packaging; see their
`benchmark.py`), and stays when it does not (the demo plant of food packaging,
the 16-meeting instance of meeting scheduling).

The `<slug>-compare` commands in the READMEs use `--seconds 10` for a quick
check, and the public-instance runners show their own budgets. Every results
block and `RESULTS.md` states the budget it was measured with (30 s per solver
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
time limit). The engine's own result does move elsewhere: by a few percent on
the larger semiconductor instances (see that README), and on the 400-request
synthetic pickup-delivery instance it did with the stop-granular search those
days measured (54 vehicles one day, 55 the other; the request-level search that
replaced it returned the same plan in two runs on 2026-11-11). Read a small
margin in any table as noise.

