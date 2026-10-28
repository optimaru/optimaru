# Timefold quickstart models (benchmark only)

The domain models and `ConstraintProvider`s of twelve
[Timefold quickstarts](https://github.com/TimefoldAI/timefold-quickstarts),
used as the Timefold column and as an independent referee in the
engine-vs-CP-SAT-vs-Timefold benchmarks. Nothing in `use-cases/` depends on
this directory.

- **Source**: timefold-quickstarts tag `v2.6.0`
  (commit `4693fdb5ef55323dc1f61f3e3a3074d4427fdfbf`), the `domain` and
  `solver` packages of each use case, under `src/main/java/org/acme/`.
- **Solver**: Timefold Solver 2.6.0 (open-source edition), default
  configuration — construction heuristic + local search; only the
  termination and the random seed are set. Every Timefold comparison in this
  repository (vehicle routing and school timetabling included) uses 2.6.0.
- **License**: Apache-2.0, see
  [`LICENSE-timefold-quickstarts.txt`](LICENSE-timefold-quickstarts.txt).
  Timefold Quickstarts derives from OptaPlanner Quickstarts (Red Hat, Inc.
  and contributors, Apache-2.0).

## Changes to the upstream sources

Only conference scheduling is touched, because its v2.6.0 sources reference
Timefold Platform model types that are not open source:

- `ConstraintInfo`, `ConstraintGroupInfo` and `ModelConstraintJustification`
  are imported from `optimaru.bench.timefold.platform` — minimal stand-ins
  that carry constraint names, descriptions and justification text. They take
  no part in scoring.
- `ConferenceSchedule` no longer implements the platform interfaces
  (`SolverModel`, `InputMetricsAware`, `OutputMetricsAware`); the two
  metrics getters that only fed the platform UI are removed.

All constraint definitions, weights and domain logic are upstream's.

## Runner

`optimaru.bench.timefold.Runner` reads a quickstart solution as JSON,
solves it (`solve`) or scores a given assignment (`score`), and writes the
score and every planning variable as ids:

```sh
mvn -q package                     # JDK 21+, builds target/timefold-quickstarts-bench.jar
java -jar target/timefold-quickstarts-bench.jar solve task-assigning in.json out.json 30000 1
java -jar target/timefold-quickstarts-bench.jar score task-assigning in.json out.json
```

Each benchmark's `to_timefold` / `from_timefold` (in
`benchmarks/<slug>/cpsat/<package>/benchmark.py`) converts between the use
case's own objects and this JSON. The runner tolerates references written as
`{"id": ...}` (it relinks them to one instance by `@PlanningId`), takes list
variables a quickstart keeps out of its JSON under `"__lists"`, and, for
order picking, rebuilds the orders from the item ids
(`"<order id>/<index>"`), since `OrderItem.order` is not serialized.

## Parity

Before any result is reported, the engine's solution is also scored by the
quickstart's own `ConstraintProvider`. For all twelve use cases that score
matches the engine's own evaluator exactly, so the three solvers optimise the
same problem.
