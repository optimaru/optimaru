# Assembly line balancing benchmark

The [`assembly-line-balancing`](../../docs/use-cases/assembly-line-balancing) use case (the C++
engine) against **OR-Tools CP-SAT** ([`cpsat/`](cpsat), package `assembly_line_balancing_cpsat`)
on SALBP-1 instances (the Otto et al. SALBP archive from assembly-line-balancing.de),
downloaded on demand and checksum-verified ([`bench/data.py`](bench/data.py)). Both
answers are rescored by the use case's own evaluator (`constraint_costs()`).

The CP-SAT model is built from [external-solver](https://github.com/optimaru/external-solver) plugins, one per engine
constraint ([`external_solver.assembly_line_balancing.constraints`](https://github.com/optimaru/external-solver/blob/v0.2.3/src/external_solver/assembly_line_balancing/constraints.py), subclassing the domain specs; the
defaults come from the engine's `constraint_info()`). Every encoding is exact, so the same
assignment gives the same raw cost per constraint in both solvers
(external-solver's `tests/assembly_line_balancing/test_constraint_parity.py` pins every
assignment of small instances).
`level="soft"` / `level="hard"` moves a term to the soft / hard level in both solvers, and
`level="medium"` is refused by both (`this use case has no medium level`). Not supported on
CP-SAT: `exponent != 1` and non-integer weights (refused).

A constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own terms with
its own knobs, its level included. The six per-station constraints add one term per station,
an empty one included (`max_stations_cost` and `forbidden_task_stations_cost` exactly their
scoped stations, as the engine evaluates them), and the four globals one term for their
total, so an integral `offset` shapes each station (or the total) exactly as the engine does
in both `clip` modes; a fractional offset is refused (`offset must be an integer for CP-SAT`).
`clip=False` with an offset on a hard pick is refused with the engine's message when the
objective is built.

```sh
cd benchmarks/assembly-line-balancing/cpsat
OPTIMARU_USE_CASES=assembly_line_balancing uv sync --all-groups --all-extras   # also builds this checkout's optimaru (assembly_line_balancing only)
uv run pytest -q tests     # comparison runner and SALBP parser (the model tests live in external-solver)
uv run python -m assembly_line_balancing_cpsat.benchmark --seconds 10     # generated lines, no download
cd .. && uv run --project cpsat python -m bench.run_bench --seconds 60   # the SALBP archive
```

Recorded results: [`bench/RESULTS.md`](bench/RESULTS.md).

<!-- results:start -->
## Results

SALBP-1: instances 1, 125, 250, 375 and 500 of each data set. Stations used; lower is better and the best value is bold. The upper bound
is the one in the archive's details workbook: the optimum where the workbook's lower bound meets
it, otherwise the best its authors' solver found in 100 s.

| Data set | Instance | Tasks | Archive upper bound | optimaru engine | CP-SAT |
| --- | --- | ---: | ---: | ---: | ---: |
| small | instance_n=20_1 | 20 | 3 | **3** | **3** |
| small | instance_n=20_125 | 20 | 5 | **5** | **5** |
| small | instance_n=20_250 | 20 | 10 | **10** | **10** |
| small | instance_n=20_375 | 20 | 3 | **3** | **3** |
| small | instance_n=20_500 | 20 | 8 | **8** | **8** |
| medium | instance_n=50_1 | 50 | 8 | **8** | **8** |
| medium | instance_n=50_125 | 50 | 33 | **33** | **33** |
| medium | instance_n=50_250 | 50 | 7 | **7** | **7** |
| medium | instance_n=50_375 | 50 | 13 | **13** | **13** |
| medium | instance_n=50_500 | 50 | 34 | **34** | **34** |
| large | instance_n=100_1 | 100 | 23 | 24 | **23** |
| large | instance_n=100_125 | 100 | 14 | **14** | **14** |
| large | instance_n=100_250 | 100 | 24 | **24** | **24** |
| large | instance_n=100_375 | 100 | 57 | 59 | **57** |
| large | instance_n=100_500 | 100 | 14 | **14** | **14** |
| very-large | instance_n=1000_1 | 1000 | 135 | 137 | not run (size cap) |
| very-large | instance_n=1000_125 | 1000 | 546 | 530 | not run (size cap) |
| very-large | instance_n=1000_250 | 1000 | 140 | 153 | not run (size cap) |
| very-large | instance_n=1000_375 | 1000 | 229 | 233 | not run (size cap) |
| very-large | instance_n=1000_500 | 1000 | 591 | 555 | not run (size cap) |

CP-SAT runs only up to the exact-model size cap (`--exact-max-size`, 100 tasks). Measured 2026-11-10 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — a snapshot, not a statistical study.
<!-- results:end -->
