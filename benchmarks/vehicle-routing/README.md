# Vehicle routing benchmark (CVRP)

The [`vehicle-routing`](../../use-cases/vehicle-routing) use case against two
widely used open-source solvers on the classic capacitated VRP:

| Backend | Solver | Setup |
| --- | --- | --- |
| `engine` | optimaru, `vrp_routing` | default configuration, 1 worker |
| `ortools` | [Google OR-Tools](https://developers.google.com/optimization/routing) Routing solver (`pywrapcp`) | PATH_CHEAPEST_ARC + guided local search, the library's recommended VRP setup |
| `timefold` | [Timefold Solver](https://github.com/TimefoldAI/timefold-solver) open-source edition, 2.7.1 | default configuration (construction heuristic + local search), Constraint Streams, list variable |

OR-Tools and Timefold live here only; they are not part of the use case.

## Protocol

- **Instances**: [CVRPLIB Set X](https://galgos.inf.puc-rio.br/cvrplib/index.php/en/instances/1)
  (Uchoa et al. 2017, 100–1000 customers) with the current best-known
  solutions (BKS), read from the official catalog or its
  [GitHub mirror](https://github.com/PyVRP/Instances). Single Augerat
  instances (`--instance A-n32-k5`) come from a SHA-256-pinned archive.
- **Same inputs**: every backend gets the fleet size from the instance name
  (`-kN`), the vehicle capacity, and the same rounded integer distance
  matrix (CVRPLIB's EUC_2D rule). Routes are round trips from the depot.
- **Same budget**: wall-clock seconds per run (`--seconds`), same seed,
  single-threaded. Timefold's time includes JVM start-up (~0.3 s).
- **Independent rescoring**: the harness rebuilds each plan's distance from
  the matrix, checks capacity and that no customer appears twice, and refuses
  a backend whose own reported distance disagrees.
- **Gap** = (distance − BKS) / BKS, counted only for complete runs (every
  customer served, no route over capacity). An incomplete run is reported as
  `n unassigned` (OR-Tools may drop a customer at a penalty when it cannot
  fit it in time) or `infeasible` (Timefold assigns every customer and may
  end over capacity).

## Running

Requires [uv](https://docs.astral.sh/uv/) and, for Timefold, JDK 21+ and Maven.

```sh
cd benchmarks/vehicle-routing
uv sync --extra dev                          # builds the engine's pybind extension
mvn -f timefold/pom.xml -q package           # builds timefold/target/timefold-vrp-bench.jar

# one Augerat instance, all backends
uv run python -m vrp_benchmark --instance A-n32-k5 --seconds 10

# Set X, one median instance per size bucket
uv run python -m vrp_benchmark --series x --limit-per-bucket 1 --seconds 60 \
  --catalog-source mirror --out bench/set-x.json --summary-markdown bench/set-x.md

# all 100 Set X instances, a subset of backends
uv run python -m vrp_benchmark --series x --seconds 60 --backend engine,ortools
```

`uv run pytest` checks the parser, the rescoring and that every backend finds
the optimum of a tiny instance (Timefold's case is skipped without a JDK and
the jar).

The [`vrp-public-bench`](../../.github/workflows/vrp-public-bench.yml)
workflow runs the one-per-bucket sample at 60 s on every push to `main` that
touches the engine, the use case or this harness, and on every `v*` tag. It
posts the Markdown table as the job summary and keeps the JSON as an
artifact. It is a report, not a quality gate — gaps move with the runner —
except that an incomplete engine plan fails the job (`--fail-on-incomplete`).

## Layout

```
vrp_benchmark/
├── run.py              CLI, series runner, summaries (python -m vrp_benchmark)
├── instance.py         CVRPLIB parser, Augerat archive, rescore()
├── cvrplib.py          Set X catalog + BKS download and cache
└── solvers/
    ├── engine.py           optimaru via vrp_routing
    ├── ortools_routing.py  OR-Tools Routing solver
    └── timefold.py         runs the Java program below as a subprocess
timefold/               Maven project: Timefold CVRP model and a main that reads
                        the instance file and prints the routes
tests/
```

## Sample result

One median instance per bucket, 10 s per run, seed 0, Apple Silicon macOS
(2026-10-29). A short local smoke run, not a published comparison:

| Instance | Customers | engine | ortools | timefold |
|---|---:|---:|---:|---:|
| X-n153-k22 | 152 | 1.46% | 1 unassigned | 4.86% |
| X-n289-k60 | 288 | 3.02% | 2 unassigned | 25.70% |
| X-n491-k59 | 490 | 2.27% | 9.39% | 73.81% |
| X-n701-k44 | 700 | 2.26% | 8.94% | 78.82% |
| X-n895-k37 | 894 | 6.08% | 5 unassigned | 289.34% |

Nearby selection, which Timefold recommends for routing at this scale, is
not part of its open-source edition, so the `timefold` column runs without
it.
