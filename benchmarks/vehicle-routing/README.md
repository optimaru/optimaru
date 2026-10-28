# CVRP public comparison

Run everything below from this directory (`benchmarks/vehicle-routing`);
`uv sync --extra dev` builds the engine's pybind extension and installs the
CP-SAT backend beside it.

The single-instance smoke test uses
[Hexaly's CVRP template archive](https://www.hexaly.com/examples/cvrp.zip),
which contains Augerat/CVRPLIB `EUC_2D` instances and their known optimum in
the instance comment. The downloader pins SHA-256 and the parser implements
the same nearest-integer Euclidean rule as the template.

```bash
uv run python -m vrp_benchmark.public_benchmark \
  --instance A-n32-k5 --seconds 10 --backend both
```

Observed locally on 2026-08-17 with one worker and seed 0:

| Backend | Vehicles | Distance | Gap to 784 |
|---|---:|---:|---:|
| Engine | 5 | 827 | 5.48% |
| CP-SAT | 5 | 975 | 24.36% |

This is a same-machine, same-budget run. It is not a reproduction of Hexaly's
published hardware/runtime table. The fleet size comes from `-k5`; the capacity
lower bound makes all five vehicles necessary. Time-limited search can vary
slightly with wall-clock termination even with a fixed random seed.

## PyVRP on the same benchmark, on every version

`--backend pyvrp` runs [PyVRP](https://github.com/PyVRP/PyVRP) (hybrid
genetic search) beside the engine on the very same inputs: the Set X
instances and best-known solutions from the
[PyVRP/Instances](https://github.com/PyVRP/Instances) mirror that PyVRP's own
benchmarks read (`--catalog-source pyvrp-mirror`), the fleet size from the
instance name, the same rounded integer distance matrix handed to PyVRP as its
`ProblemData`, the same wall-clock budget and seed, and the same independent
`rescore()` of the returned routes. The `bench` extra installs it:

```bash
uv sync --extra bench
uv run python -m vrp_benchmark.public_benchmark \
  --series x --limit-per-bucket 1 --seconds 60 \
  --backend engine,pyvrp --catalog-source pyvrp-mirror \
  --out bench/set-x.json --summary-markdown bench/set-x.md
```

The `vrp-public-bench` workflow runs exactly that on every push to `main` and
every `v*` tag (and on demand with other budgets, samples and backends), posts
the Markdown table as the job summary and keeps the JSON as an artifact. It is
a report, not a quality gate — gaps move with the runner — except that the
engine leaving a customer unassigned fails the job (`--fail-on-unassigned`).

Local 60-second runs (Linux x86-64, four runs in parallel on four cores,
seeds 0/1), PyVRP 0.14.0. The engine request gives every visit a 60-second
service duration and a deadline no route can reach, while PyVRP gets zero
durations; neither time dimension can bind, so both minimise the same rounded
distance. The middle column is the engine with the ejection chain alone
(2026-10-05); the right-hand engine column adds the O(1) move previews and the
iterated local search (2026-10-06, see below):

| Instance | Engine (ejection chain) | Engine (+ previews, ILS) | PyVRP |
|---|---:|---:|---:|
| X-n153-k22 | 1.61% / 1.50% | 1.05% / 1.41% | 0.49% / 0.46% |
| X-n289-k60 | 6.41% / 8.67% | 2.46% / 2.36% | 0.43% / 0.70% |
| X-n491-k59 | 3.46% / 3.85% | 1.45% / 1.10% | 0.54% / 0.51% |
| X-n701-k44 | 4.49% / 3.94% | 2.41% / 2.02% | 1.18% / 1.09% |
| X-n895-k37 | 16.99% / 17.91% | 4.45% / 5.66% | 1.44% / 1.42% |

The engine runs are deterministic for a given seed, so a re-run of the same
build reproduces these distances exactly; what moves between builds is the
search trajectory, and the X-n895 pair spans roughly a point between seeds.

Two changes made the difference. Every strategy used to evaluate a candidate
by applying it, rescoring and undoing it, which costs a route walk per
candidate; the vehicle-routing calculator now answers O(1) *previews* (segment
move, swap, reverse, tail swap, CROSS, insert) from per-route prefix sums, and
the local search, descent, ejection chain and repair all ask for a preview
first. The same search trajectory runs 6-9x faster (X-n895: 255k to 2.3M
steps in 60s; 9.67% / 11.66%). On top of that an *iterated local search*
strategy (`iteratedLocalSearchWeight`) ruins 2-8 visits, repairs them and
descends with relocate / swap / 2-opt / 2-opt* / CROSS around each touched
visit's 25 nearest neighbours until the routes are locally optimal, and the
acceptor judges the local optimum. Its settings were chosen on X-n289 and
X-n895 at 10 seconds (mean gap over both seeds: no ILS 12.5%, ruin 5-15 with
Or-opt 7.3%, Or-opt off 6.0%, nearby 25 5.4%, ruin 2-8 4.9%).

## CVRPLIB Set X versus Hexaly's published result

The series runner reads all 100 Set X instances and their current BKS directly
from the [official CVRPLIB catalog](https://galgos.inf.puc-rio.br/cvrplib/index.php/en/instances/1).
It uses the same five customer-count buckets as
[Hexaly's CVRP benchmark](https://www.hexaly.com/benchmarks/hexaly-vs-gurobi-capacitated-vehicle-routing-problem-cvrp).

Hexaly's published experiment uses 60 seconds per instance, default parameters,
and an AMD Ryzen 7 7700 with eight cores. Its July 2026 aggregate table is:

| Customers | Instances | Hexaly 15.0 | Gurobi 11.0 | OR-Tools 9.14 |
|---|---:|---:|---:|---:|
| 100–200 | 22 | 0.1% | 11.5% | 3.6% |
| 200–400 | 36 | 0.5% | 17.5% | 6.4% |
| 400–600 | 19 | 0.6% | 41.0% | 6.9% |
| 600–800 | 12 | 0.8% | 586.7% | 7.8% |
| 800–1000 | 11 | 0.9% | 793.3% | 5.4% |

Run the full matching Set X protocol for the local engine:

```bash
uv run python -m vrp_benchmark.public_benchmark \
  --series x --seconds 60 --workers 8 --backend engine
```

Use `--limit-per-bucket 1` for an evenly sampled five-instance smoke run.
The CP-SAT circuit model grows quadratically per vehicle, so series runs skip it
above 200 customers by default; change `--cpsat-max-customers` explicitly to
raise that guard.

The engine backend runs on the monolithic fast path
(`solve_engine_request(request)`), the repo's bench convention: the four
benchmark constraints are exactly the monolithic score semantics
(monolith-vs-plugin parity is a standing `vrp_plugin_test` gate), and every
returned solution is still re-verified by the harness's independent
`rescore()`.

The plugin assembly path (`solve_with_constraints`, the path every request
with an optional route policy takes) runs the same iterated local search as
the fast path, since the decomposed calculator serves move previews. The four
default components answer a preview in O(pieces) from shared per-route prefix
sums (`constraints/route_sums.hpp`), and a component at weight 0 (an opt-in
objective left off) is not asked during a preview at all; before that, every
component re-walked the written-out candidate route per preview, the
weight-0 enclosing-circle objective included, and the plugin path managed a
few hundred steps in 10 seconds on X-n895. Gap to BKS on the plugin path, 1
worker, seeds 0 / 1, 4 runs in parallel on a 4-core Linux box (`-3medium` =
three customers unassigned):

| instance | 10 s, before previews | 10 s, previews by re-walk | 60 s, previews by re-walk | 10 s, piecewise previews | 60 s, piecewise previews |
|---|---:|---:|---:|---:|---:|
| X-n289-k60 | 22.06 / 20.56% | 6.51 / 7.77% | 3.69 / 4.05% | 4.95 / 5.89% | 3.44 / 3.18% |
| X-n895-k37 | 203.88 / 225.48% | 19.87 / 23.14% (3 / 2 unassigned) | 14.24 / 14.38% | 14.35 / 14.94% | 7.81 / 7.98% |

The monolithic calculator serves previews on time-windowed plans too (one
walk of the candidate route when visits wait for their windows), which the
Set X runs never exercise. On the repository's demo generator (morning /
afternoon windows, 07:30 departures) at 10 s, best score before → with
previews, seeds 0 / 1:

| plan | before | with previews |
|---|---|---|
| 400 visits, 20 vehicles | -560medium / -13514soft, -560 / -13125 | -560 / -13282, -560 / -12926 |
| 800 visits, 40 vehicles | -1030 / -30296, -1040 / -34857 | -1030 / -21720, -1030 / -20900 |

Local 60-second smoke result on 2026-08-18 (one median instance per bucket,
engine one worker, mean of seeds 0-1, Apple Silicon macOS, single machine,
after the fast-path harness alignment above and the solver's late-acceptance
auto-rule retune — the 2026-08-17 numbers, 16-51% with the same protocol,
predate both):

| Customers | Instance | Engine gap to BKS | Hexaly full-bucket average |
|---|---|---:|---:|
| 100–200 | X-n153-k22 | 3.61% | 0.1% |
| 200–400 | X-n289-k60 | 15.80% | 0.5% |
| 400–600 | X-n491-k59 | 3.71% | 0.6% |
| 600–800 | X-n701-k44 | 4.52% | 0.8% |
| 800–1000 | X-n895-k37 | incomplete (1 unassigned on both seeds; total distance 71664 / 62547 vs BKS 53860) | 0.9% |

With the ejection chain enabled by default (`ejectionChainWeight = 0.05`),
the ruin & recreate repair kept out of the unassigned pool and the
stagnation kick off (`ruinRecreate = false`), the same one-instance-per-bucket
protocol on 2026-10-05 (Linux x86-64, four runs in parallel on four cores,
seeds 0-1, instances and BKS from the PyVRP/Instances mirror of Set X because
the CVRPLIB host was unreachable) measured:

| Customers | Instance | Before (seed 0 / 1) | After (seed 0 / 1) |
|---|---|---:|---:|
| 100–200 | X-n153-k22 | 3.11% / 2.60% | 1.61% / 1.50% |
| 200–400 | X-n289-k60 | 15.83% / 14.36% | 6.41% / 8.67% |
| 400–600 | X-n491-k59 | 5.09% / 4.69% | 3.46% / 3.85% |
| 600–800 | X-n701-k44 | 5.08% / 4.40% | 4.49% / 3.94% |
| 800–1000 | X-n895-k37 | incomplete (2 / 1 unassigned) | 16.99% / 17.91% |

Three things changed, each measured on its own. X-n895-k37 loads its 37
vehicles to 99.8% with demands of 50–100, so the last customer only fits
after a chain of slack-shifting swaps that are each uphill; the ejection
chain applies them as one move, and every customer is now assigned. The
stagnation kick turned out to be what held X-n289-k60 (99.7% loaded) at
9-10%: a kicked solution parks the leftover in the pool, the search never
re-assigns it, and the best stops moving after 5-17 seconds. With the kick
off the search improves until the clock runs out on every instance. The
X-n895 gap that remained after this step was mostly throughput (the
construction heuristic starts it 200% above BKS); the move previews and the
iterated local search described under the PyVRP comparison above brought it
to 4.5-5.7%.

The local CP-SAT backend left five customers unassigned on X-n153-k22 after
60 seconds, so it has no valid BKS gap for that run. Hexaly's `OR-Tools 9.14`
column uses the OR-Tools Routing Model API; it is not this CP-SAT backend.

The smoke rows are intentionally not presented as an aggregate reproduction:
they cover one of the official instances per bucket and ran on different
hardware. A local gap enters the average only when every customer is assigned;
incomplete time-limited solutions are reported separately.
