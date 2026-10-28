# Benchmarks

Reproducible comparisons of the engine's use cases against other open-source
solvers. The competing solvers live only here, never in `use-cases/`.
Instance data is generated deterministically or downloaded on demand
(checksum-verified, kept out of Git), and every returned solution is rescored
by the use case's own evaluator; a solver's self-reported objective is never
taken as the result.

| Benchmark | Compared against | Instances |
| --- | --- | --- |
| [vehicle-routing](vehicle-routing) | OR-Tools Routing, Timefold Solver | CVRPLIB Set X, Augerat |
| [school-timetabling](school-timetabling) | OR-Tools CP-SAT, Timefold Solver | generated S/M/L schools |
| [pickup-delivery](pickup-delivery) | OR-Tools CP-SAT | Li & Lim PDPTW |
| [project-job-scheduling](project-job-scheduling) | OR-Tools CP-SAT, Timefold Solver | Patterson RCPSP + generated |
| [bin-packing](bin-packing) | OR-Tools CP-SAT | Hexaly BPP instances |
| [quadratic-assignment](quadratic-assignment) | OR-Tools CP-SAT | QAPLIB |
| [assembly-line-balancing](assembly-line-balancing) | OR-Tools CP-SAT | SALBP-1 |
| [bed-allocation](bed-allocation) | OR-Tools CP-SAT, Timefold Solver | generated |
| [conference-scheduling](conference-scheduling) | OR-Tools CP-SAT, Timefold Solver | quickstart demo |
| [facility-location](facility-location) | OR-Tools CP-SAT, Timefold Solver | generated |
| [flight-crew-scheduling](flight-crew-scheduling) | OR-Tools CP-SAT, Timefold Solver | generated |
| [food-packaging](food-packaging) | OR-Tools CP-SAT, Timefold Solver | quickstart demo |
| [knapsack](knapsack) | OR-Tools CP-SAT | generated |
| [maintenance-scheduling](maintenance-scheduling) | OR-Tools CP-SAT, Timefold Solver | quickstart demo (small, large) |
| [max-cut](max-cut) | OR-Tools CP-SAT | generated |
| [meeting-scheduling](meeting-scheduling) | OR-Tools CP-SAT, Timefold Solver | quickstart demo |
| [order-picking](order-picking) | OR-Tools CP-SAT, Timefold Solver | generated |
| [semiconductor-scheduling](semiconductor-scheduling) | OR-Tools CP-SAT | generated |
| [sports-league-scheduling](sports-league-scheduling) | OR-Tools CP-SAT, Timefold Solver | quickstart demo |
| [task-assigning](task-assigning) | OR-Tools CP-SAT, Timefold Solver | quickstart demo |
| [tournament-scheduling](tournament-scheduling) | OR-Tools CP-SAT, Timefold Solver | generated |

Each CP-SAT model lives in `benchmarks/<slug>/cpsat` as its own `uv` project
that depends on `use-cases/<slug>/python` by path, so a run always measures
the engine in this checkout. [`cpsat-common`](cpsat-common) holds the shared
constraint-plugin contract and the comparison runner (`run_cli`).

**Timefold.** Every Timefold comparison uses Timefold Solver **2.6.0** (open
source, default configuration) and the models of
[timefold-quickstarts v2.6.0](timefold-quickstarts): the quickstart's own
domain and `ConstraintProvider`, so Timefold solves the problem exactly as its
authors defined it. Each solution of every solver is also scored by that
`ConstraintProvider`, and for every use case the engine's own evaluator agrees
with it exactly — the solvers optimise the same objective. Build the models
once with `mvn -f benchmarks/timefold-quickstarts/pom.xml package` (JDK 21+);
without the jar the runners simply leave the Timefold column out
(`--no-timefold` does so explicitly).
