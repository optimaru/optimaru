# Benchmarks

Reproducible comparisons of the engine's use cases against other open-source
solvers. The competing solvers live only here, never in `use-cases/`.
Instance data is generated deterministically or downloaded on demand
(checksum-verified, kept out of Git), and every returned solution is rescored
by the use case's own evaluator; a solver's self-reported objective is never
taken as the result.

| Benchmark | Compared against | Instances |
| --- | --- | --- |
| [vehicle-routing](vehicle-routing) | OR-Tools Routing | CVRPLIB Set X, Augerat |
| [school-timetabling](school-timetabling) | OR-Tools CP-SAT | generated S/M/L schools |
| [pickup-delivery](pickup-delivery) | OR-Tools CP-SAT | Li & Lim PDPTW |
| [project-job-scheduling](project-job-scheduling) | OR-Tools CP-SAT | RG30 RCPSP (Patterson format) + generated |
| [bin-packing](bin-packing) | OR-Tools CP-SAT | Falkenauer u/t (OR-Library) |
| [quadratic-assignment](quadratic-assignment) | OR-Tools CP-SAT | QAPLIB |
| [assembly-line-balancing](assembly-line-balancing) | OR-Tools CP-SAT | SALBP-1 |
| [bed-allocation](bed-allocation) | OR-Tools CP-SAT | generated |
| [conference-scheduling](conference-scheduling) | OR-Tools CP-SAT | quickstart demo |
| [employee-scheduling](employee-scheduling) | OR-Tools CP-SAT | seeded demo (generated) |
| [facility-location](facility-location) | OR-Tools CP-SAT | generated |
| [flight-crew-scheduling](flight-crew-scheduling) | OR-Tools CP-SAT | generated |
| [food-packaging](food-packaging) | OR-Tools CP-SAT | quickstart demo |
| [knapsack](knapsack) | OR-Tools CP-SAT | generated |
| [maintenance-scheduling](maintenance-scheduling) | OR-Tools CP-SAT | quickstart demo (small, large) |
| [max-cut](max-cut) | OR-Tools CP-SAT | generated |
| [meeting-scheduling](meeting-scheduling) | OR-Tools CP-SAT | quickstart demo |
| [order-picking](order-picking) | OR-Tools CP-SAT | generated |
| [semiconductor-scheduling](semiconductor-scheduling) | OR-Tools CP-SAT | generated |
| [sports-league-scheduling](sports-league-scheduling) | OR-Tools CP-SAT | quickstart demo |
| [task-assigning](task-assigning) | OR-Tools CP-SAT | quickstart demo |
| [tournament-scheduling](tournament-scheduling) | OR-Tools CP-SAT | generated |

The CP-SAT models come from
[external-solver](https://github.com/optimaru/external-solver), installed from
its GitHub release tag (`external_solver.<subpackage>`); the models and their
own tests live there. Each comparison is a `uv` project in
`benchmarks/<slug>/cpsat` that depends on `use-cases/<slug>/python` by path, so
a run always measures the engine in this checkout. Importing the project's
package calls `cpsat_plugins.checkout.use_checkout_engine`, which registers that
checkout package as `optimaru.<subpackage>`, so external-solver builds and scores
with the same engine module; the published `optimaru` wheel is not installed
(`override-dependencies`). [`cpsat-common`](cpsat-common) holds that helper and
the comparison runner (`run_cli`). To pick up a new external-solver release,
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

