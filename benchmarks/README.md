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
| [project-job-scheduling](project-job-scheduling) | OR-Tools CP-SAT | Patterson RCPSP + generated |
| [bin-packing](bin-packing) | OR-Tools CP-SAT | Hexaly BPP instances |
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

Each CP-SAT model lives in `benchmarks/<slug>/cpsat` as its own `uv` project
that depends on `use-cases/<slug>/python` by path, so a run always measures
the engine in this checkout. [`cpsat-common`](cpsat-common) holds the shared
constraint-plugin contract and the comparison runner (`run_cli`).

A CP-SAT constraint is a plugin that subclasses the use case's own constraint
spec (`<Subject>Cost`), so both solvers take the same selection: the same
names and the same `weight` / `level` / `exponent` knobs. The plugin declares no
defaults; its level, weight, `level_flippable` and `reward` come from the
domain's `constraint_info()`, so the two solvers cannot disagree on a default.
`level="soft"` demotes a hard or medium constraint to soft on both paths. CP-SAT
accepts only `exponent=1` and integral weights, and some constraints are
structural in a CP-SAT model (enforced by its variables, so `level="soft"` is
refused there; school-timetabling, pickup-delivery and semiconductor-scheduling
have such constraints). Each benchmark's README (or `cpsat/README.md`) lists
its own limits.

The assembly-line-balancing, bin-packing and quadratic-assignment tables on
public instance sets still hold numbers measured with the 0.1.x pure-Python
engines; the 0.2.0 C++ ports have not been re-measured on those archives yet
(see each README).
