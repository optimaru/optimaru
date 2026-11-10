# Assembly line balancing benchmark

The [`assembly-line-balancing`](../../docs/use-cases/assembly-line-balancing) use case (the C++
engine) against **OR-Tools CP-SAT** ([`cpsat/`](cpsat), package `assembly_line_balancing_cpsat`)
on SALBP-1 instances (the Otto et al. SALBP archive from assembly-line-balancing.de),
downloaded on demand and checksum-verified ([`bench/data.py`](bench/data.py)). Both
answers are rescored by the use case's own evaluator (`constraint_costs()`).

The CP-SAT model is built from [external-solver](https://github.com/optimaru/external-solver) plugins, one per engine
constraint ([`external_solver.assembly_line_balancing.constraints`](https://github.com/optimaru/external-solver/blob/v0.1.0/src/external_solver/assembly_line_balancing/constraints.py), subclassing the domain specs; the
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

SALBP large data set (n=100). Lower is better; the best value is bold.

| Instance | Tasks | Best known | optimaru engine | CP-SAT |
| --- | --- | --- | --- | --- |
| instance_n=100_1 | 100 | 23 | 24 | **23** |
| instance_n=100_10 | 100 | 22 | **22** | **22** |
| instance_n=100_100 | 100 | 25 | 26 | **25** |

Stations used. Measured 2026-11-04 with the C++ engine on one Apple M4 Max Mac (16 cores), 30 s per solver and instance,
seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its time includes model
construction (it proved these optima in 2–6 s). Single runs — a snapshot, not a statistical study.
<!-- results:end -->
