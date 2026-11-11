# Vehicle routing benchmark (CVRP)

The [`vehicle-routing`](../../docs/use-cases/vehicle-routing) use case against a
widely used open-source solver on the classic capacitated VRP:

| Backend | Solver | Setup |
| --- | --- | --- |
| `engine` | optimaru, `optimaru.vehicle_routing` | default configuration, 1 worker |
| `ortools` | [Google OR-Tools](https://developers.google.com/optimization/routing) Routing solver (`pywrapcp`) | PATH_CHEAPEST_ARC + guided local search, the library's recommended VRP setup |

OR-Tools lives here only; it is not part of the use case.

## Protocol

- **Instances**: [CVRPLIB Set X](https://galgos.inf.puc-rio.br/cvrplib/index.php/en/instances/1)
  (Uchoa et al. 2017, 100 instances, 100–1000 customers) with the current
  best-known solutions (BKS), read from the official catalog or its
  [GitHub mirror](https://github.com/PyVRP/Instances), and Augerat's Set A
  (27 instances, 31–79 customers, each file carrying its optimum), downloaded
  from CVRPLIB one file at a time, each SHA-256 pinned.
- **Same inputs**: every backend gets the same fleet, the vehicle capacity,
  and the same rounded integer distance matrix (CVRPLIB's EUC_2D rule). Routes
  are round trips from the depot.
- **Fleet**: Set A fixes the number of routes, so its fleet is the `-kN` of
  the instance name. Set X leaves it open: `-kN` there is the fewest vehicles
  that can carry the demand, and the best-known solutions of 22 of the 100
  instances use more routes than that (the mirror's solution files on
  2026-11-10). On Set X every backend therefore gets
  a fleet that cannot bind — 30% above the capacity lower bound
  (total demand / capacity), plus three (`instance.free_fleet`) — and may leave
  vehicles unused. With the fleet held at `-kN` the BKS of those 22 instances
  is out of reach for any solver.
- **Same budget**: wall-clock seconds per run (`--seconds`), same seed,
  single-threaded.
- **Independent rescoring**: the harness rebuilds each plan's distance from
  the matrix, checks capacity and that no customer appears twice, and refuses
  a backend whose own reported distance disagrees.
- **Gap** = (distance − BKS) / BKS, counted only for complete runs (every
  customer served, no route over capacity). An incomplete run is reported as
  `n unassigned` (OR-Tools may drop a customer at a penalty when it cannot
  fit it in time) or `infeasible` (a route ends over capacity).

## Running

Requires [uv](https://docs.astral.sh/uv/).

```sh
cd benchmarks/vehicle-routing
uv sync                                      # builds the engine's pybind extension

# one Augerat instance, all backends
uv run python -m vrp_benchmark --instance A-n32-k5 --seconds 10

# all 27 Set A instances
uv run python -m vrp_benchmark --series a --seconds 30 --out bench/set-a.json

# Set X, one median instance per size bucket
uv run python -m vrp_benchmark --series x --limit-per-bucket 1 --seconds 60 \
  --catalog-source mirror --out bench/set-x.json --summary-markdown bench/set-x.md

# all 100 Set X instances, a subset of backends
uv run python -m vrp_benchmark --series x --seconds 30 --backend engine,ortools

# chosen instances of a series
uv run python -m vrp_benchmark --series x --only X-n101-k25 --only X-n153-k22 --seconds 30
```

`uv run pytest` checks the parser, the rescoring and that every backend finds
the optimum of a tiny instance.

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
├── instance.py         CVRPLIB parser, Augerat Set A download, rescore()
├── cvrplib.py          Set X catalog + BKS download and cache
└── solvers/
    ├── engine.py           optimaru via optimaru.vehicle_routing
    └── ortools_routing.py  OR-Tools Routing solver
tests/
```

<!-- results:start -->
## Results

Gap to the best-known solution; every route is re-verified by the harness. Lower is better; the best value is bold. A mean gap counts the complete runs only (complete runs / runs).

### CVRPLIB Set X

| Customers | Instances | engine | ortools |
|---|---:|---:|---:|
| 100-200 | 22 | 0.74% (22/22) | 4.07% (22/22) |
| 200-400 | 36 | 1.31% (36/36) | 6.79% (36/36) |
| 400-600 | 19 | 1.38% (19/19) | 7.00% (19/19) |
| 600-800 | 12 | 2.21% (12/12) | 7.90% (12/12) |
| 800-1000 | 11 | 1.82% (11/11) | 5.47% (11/11) |
| all | 100 | 1.36% (100/100) | 6.22% (100/100) |

| Instance | Customers | Best known | engine | ortools |
|---|---:|---:|---:|---:|
| X-n101-k25 | 100 | 27,591 | **0.25%** | 4.80% |
| X-n106-k14 | 105 | 26,362 | **0.10%** | 2.39% |
| X-n110-k13 | 109 | 14,971 | **0.69%** | 2.30% |
| X-n115-k10 | 114 | 12,747 | **0.00%** | 0.09% |
| X-n120-k6 | 119 | 13,332 | **0.89%** | 3.48% |
| X-n125-k30 | 124 | 55,539 | **1.28%** | 3.19% |
| X-n129-k18 | 128 | 28,940 | **0.35%** | 4.24% |
| X-n134-k13 | 133 | 10,916 | **1.15%** | 4.24% |
| X-n139-k10 | 138 | 13,590 | **0.00%** | 3.08% |
| X-n143-k7 | 142 | 15,700 | **1.10%** | 3.38% |
| X-n148-k46 | 147 | 43,448 | **0.27%** | 4.22% |
| X-n153-k22 | 152 | 21,220 | **0.75%** | 3.92% |
| X-n157-k13 | 156 | 16,876 | **0.28%** | 2.92% |
| X-n162-k11 | 161 | 14,138 | **0.30%** | 3.16% |
| X-n167-k10 | 166 | 20,557 | **1.88%** | 8.32% |
| X-n172-k51 | 171 | 45,607 | **0.29%** | 6.13% |
| X-n176-k26 | 175 | 47,812 | **1.14%** | 4.89% |
| X-n181-k23 | 180 | 25,569 | **0.10%** | 2.45% |
| X-n186-k15 | 185 | 24,145 | **1.00%** | 7.61% |
| X-n190-k8 | 189 | 16,980 | **2.06%** | 4.89% |
| X-n195-k51 | 194 | 44,225 | **0.49%** | 6.84% |
| X-n200-k36 | 199 | 58,578 | **2.03%** | 3.08% |
| X-n204-k19 | 203 | 19,565 | **1.51%** | 7.56% |
| X-n209-k16 | 208 | 30,656 | **0.86%** | 5.91% |
| X-n214-k11 | 213 | 10,856 | **2.68%** | 10.84% |
| X-n219-k73 | 218 | 117,595 | **0.01%** | 0.69% |
| X-n223-k34 | 222 | 40,437 | **0.78%** | 7.70% |
| X-n228-k23 | 227 | 25,742 | **1.03%** | 4.16% |
| X-n233-k16 | 232 | 19,230 | **2.00%** | 6.86% |
| X-n237-k14 | 236 | 27,042 | **0.92%** | 6.03% |
| X-n242-k48 | 241 | 82,751 | **0.51%** | 4.62% |
| X-n247-k50 | 246 | 37,274 | **2.12%** | 5.24% |
| X-n251-k28 | 250 | 38,684 | **1.34%** | 4.52% |
| X-n256-k16 | 255 | 18,839 | **1.30%** | 7.40% |
| X-n261-k13 | 260 | 26,558 | **1.76%** | 7.81% |
| X-n266-k58 | 265 | 75,478 | **1.01%** | 4.51% |
| X-n270-k35 | 269 | 35,291 | **0.86%** | 4.68% |
| X-n275-k28 | 274 | 21,245 | **0.70%** | 4.64% |
| X-n280-k17 | 279 | 33,503 | **1.01%** | 11.25% |
| X-n284-k15 | 283 | 20,226 | **1.81%** | 7.92% |
| X-n289-k60 | 288 | 95,151 | **0.89%** | 5.25% |
| X-n294-k50 | 293 | 47,161 | **0.78%** | 7.85% |
| X-n298-k31 | 297 | 34,231 | **2.05%** | 11.36% |
| X-n303-k21 | 302 | 21,736 | **1.84%** | 8.22% |
| X-n308-k13 | 307 | 25,859 | **2.86%** | 15.27% |
| X-n313-k71 | 312 | 94,043 | **0.85%** | 9.31% |
| X-n317-k53 | 316 | 78,355 | **0.41%** | 1.07% |
| X-n322-k28 | 321 | 29,834 | **1.85%** | 7.37% |
| X-n327-k20 | 326 | 27,532 | **1.91%** | 7.46% |
| X-n331-k15 | 330 | 31,102 | **1.61%** | 6.80% |
| X-n336-k84 | 335 | 139,111 | **1.00%** | 4.87% |
| X-n344-k43 | 343 | 42,050 | **1.45%** | 5.93% |
| X-n351-k40 | 350 | 25,896 | **1.37%** | 11.11% |
| X-n359-k29 | 358 | 51,505 | **1.93%** | 6.85% |
| X-n367-k17 | 366 | 22,814 | **2.06%** | 9.22% |
| X-n376-k94 | 375 | 147,713 | **0.07%** | 0.73% |
| X-n384-k52 | 383 | 65,940 | **1.06%** | 5.80% |
| X-n393-k38 | 392 | 38,260 | **0.99%** | 7.61% |
| X-n401-k29 | 400 | 66,154 | **1.02%** | 3.02% |
| X-n411-k19 | 410 | 19,712 | **2.55%** | 14.42% |
| X-n420-k130 | 419 | 107,798 | **0.46%** | 5.01% |
| X-n429-k61 | 428 | 65,449 | **1.35%** | 5.81% |
| X-n439-k37 | 438 | 36,391 | **0.64%** | 5.64% |
| X-n449-k29 | 448 | 55,233 | **2.95%** | 7.97% |
| X-n459-k26 | 458 | 24,139 | **2.42%** | 14.13% |
| X-n469-k138 | 468 | 221,824 | **1.55%** | 5.29% |
| X-n480-k70 | 479 | 89,449 | **0.53%** | 5.14% |
| X-n491-k59 | 490 | 66,483 | **1.50%** | 9.44% |
| X-n502-k39 | 501 | 69,226 | **0.35%** | 1.45% |
| X-n513-k21 | 512 | 24,201 | **1.99%** | 12.33% |
| X-n524-k153 | 523 | 154,593 | **1.07%** | 1.97% |
| X-n536-k96 | 535 | 94,846 | **1.35%** | 10.89% |
| X-n548-k50 | 547 | 86,700 | **0.70%** | 3.15% |
| X-n561-k42 | 560 | 42,717 | **2.07%** | 12.61% |
| X-n573-k30 | 572 | 50,673 | **1.13%** | 5.35% |
| X-n586-k159 | 585 | 190,316 | **0.94%** | 4.73% |
| X-n599-k92 | 598 | 108,451 | **1.57%** | 4.67% |
| X-n613-k62 | 612 | 59,535 | **2.40%** | 9.32% |
| X-n627-k43 | 626 | 62,164 | **1.76%** | 5.11% |
| X-n641-k35 | 640 | 63,684 | **2.60%** | 6.13% |
| X-n655-k131 | 654 | 106,780 | **0.20%** | 1.16% |
| X-n670-k130 | 669 | 146,332 | **3.67%** | 4.68% |
| X-n685-k75 | 684 | 68,205 | **1.79%** | 16.23% |
| X-n701-k44 | 700 | 81,923 | **2.37%** | 8.94% |
| X-n716-k35 | 715 | 43,373 | **3.54%** | 7.59% |
| X-n733-k159 | 732 | 136,187 | **0.96%** | 7.20% |
| X-n749-k98 | 748 | 77,269 | **1.57%** | 7.80% |
| X-n766-k71 | 765 | 114,417 | **2.91%** | 10.82% |
| X-n783-k48 | 782 | 72,386 | **2.79%** | 9.86% |
| X-n801-k40 | 800 | 73,311 | **1.59%** | 4.15% |
| X-n819-k171 | 818 | 158,121 | **1.41%** | 4.49% |
| X-n837-k142 | 836 | 193,737 | **1.34%** | 4.27% |
| X-n856-k95 | 855 | 88,965 | **0.97%** | 2.98% |
| X-n876-k59 | 875 | 99,299 | **1.76%** | 4.50% |
| X-n895-k37 | 894 | 53,860 | **3.38%** | 8.97% |
| X-n916-k207 | 915 | 329,179 | **1.04%** | 4.30% |
| X-n936-k151 | 935 | 132,715 | **2.61%** | 8.49% |
| X-n957-k87 | 956 | 85,465 | **0.85%** | 4.02% |
| X-n979-k58 | 978 | 118,976 | **1.98%** | 4.20% |
| X-n1001-k43 | 1000 | 72,355 | **3.10%** | 9.77% |

### CVRPLIB Set A (Augerat)

Mean gap over the 27 instances: engine 0.13% (27/27), ortools 0.48% (27/27).

| Instance | Customers | Best known | engine | ortools |
|---|---:|---:|---:|---:|
| A-n32-k5 | 31 | 784 | **0.00%** | **0.00%** |
| A-n33-k5 | 32 | 661 | **0.00%** | **0.00%** |
| A-n33-k6 | 32 | 742 | **0.00%** | **0.00%** |
| A-n34-k5 | 33 | 778 | **0.00%** | **0.00%** |
| A-n36-k5 | 35 | 799 | **0.00%** | **0.00%** |
| A-n37-k5 | 36 | 669 | **0.00%** | **0.00%** |
| A-n37-k6 | 36 | 949 | **0.00%** | **0.00%** |
| A-n38-k5 | 37 | 730 | **0.00%** | **0.00%** |
| A-n39-k5 | 38 | 822 | **0.00%** | 0.36% |
| A-n39-k6 | 38 | 831 | **0.24%** | **0.24%** |
| A-n44-k6 | 43 | 937 | **0.00%** | 0.21% |
| A-n45-k6 | 44 | 944 | **0.00%** | 0.74% |
| A-n45-k7 | 44 | 1,146 | **0.00%** | **0.00%** |
| A-n46-k7 | 45 | 914 | **0.00%** | **0.00%** |
| A-n48-k7 | 47 | 1,073 | **0.00%** | **0.00%** |
| A-n53-k7 | 52 | 1,010 | **0.69%** | **0.69%** |
| A-n54-k7 | 53 | 1,167 | **0.00%** | 0.26% |
| A-n55-k9 | 54 | 1,073 | **0.00%** | 0.09% |
| A-n60-k9 | 59 | 1,354 | **0.00%** | **0.00%** |
| A-n61-k9 | 60 | 1,034 | **0.29%** | 0.68% |
| A-n62-k8 | 61 | 1,288 | **0.00%** | 2.95% |
| A-n63-k10 | 62 | 1,314 | **0.30%** | 0.53% |
| A-n63-k9 | 62 | 1,616 | **0.00%** | 1.55% |
| A-n64-k9 | 63 | 1,401 | **0.71%** | 1.36% |
| A-n65-k9 | 64 | 1,174 | **0.34%** | 0.85% |
| A-n69-k9 | 68 | 1,159 | **0.43%** | 0.95% |
| A-n80-k10 | 79 | 1,763 | **0.62%** | 1.59% |

Measured 2026-11-10 on one Apple M4 Max Mac (16 cores), 30 s per run, seed 0, single-threaded. Single runs — a snapshot, not a statistical study.
<!-- results:end -->
