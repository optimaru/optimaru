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
| [bed-allocation](bed-allocation) | OR-Tools CP-SAT | generated |
| [conference-scheduling](conference-scheduling) | OR-Tools CP-SAT | demo instance |
| [employee-scheduling](employee-scheduling) | OR-Tools CP-SAT | seeded demo (generated) |
| [facility-location](facility-location) | OR-Tools CP-SAT | generated |
| [flight-crew-scheduling](flight-crew-scheduling) | OR-Tools CP-SAT | generated |
| [food-packaging](food-packaging) | OR-Tools CP-SAT | demo instance |
| [knapsack](knapsack) | OR-Tools CP-SAT | generated |
| [maintenance-scheduling](maintenance-scheduling) | OR-Tools CP-SAT | demo instances (small, large) |
| [max-cut](max-cut) | OR-Tools CP-SAT | generated |
| [meeting-scheduling](meeting-scheduling) | OR-Tools CP-SAT | demo instance |
| [order-picking](order-picking) | OR-Tools CP-SAT | generated |
| [semiconductor-scheduling](semiconductor-scheduling) | OR-Tools CP-SAT | generated |
| [sports-league-scheduling](sports-league-scheduling) | OR-Tools CP-SAT | demo instance |
| [task-assigning](task-assigning) | OR-Tools CP-SAT | demo instance |
| [tournament-scheduling](tournament-scheduling) | OR-Tools CP-SAT | generated |

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
that check and the comparison runner (`run_cli`). To pick up a new external-solver release,
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
date.

The published numbers are single runs on one machine. The harnesses were run
side by side, each on its own performance cores: one CP-SAT job at a time
(8 workers) beside two single-threaded jobs, at most ten solver threads on
twelve performance cores, with builds and other work kept to the efficiency
cores (`taskpolicy -b` on macOS).

