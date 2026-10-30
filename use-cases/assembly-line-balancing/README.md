# Assembly-line balancing

Independent simple assembly-line balancing use case. The Python package
contains the canonical model, constraints and metaheuristic solver, and
`benchmarks/assembly-line-balancing/cpsat` contains the exact reference implementation.

Constraints cover station capacity, precedence, station-count limits,
task-specific station domains, fixed and forbidden assignments, same/different
station zoning, station-count minimization and optional workload balance. The
base SALBP-1 formulation follows the official
[Hexaly assembly-line balancing template](https://www.hexaly.com/docs/last/exampletour/assemblylinebalancing.html).

## SALBP benchmark used by Hexaly

`benchmarks/assembly-line-balancing/bench/run_bench.py` downloads the official Otto et al. SALBP archive linked by
Hexaly, verifies its SHA-256 checksum, reads the best feasible upper bound from the
bundled spreadsheets, and runs the metaheuristic and CP-SAT backends on the
same `.alb` files. Results are independently re-scored before reporting the
station-count gap. The 9.8 MB archive is cached in `benchmarks/assembly-line-balancing/bench/.cache/`.

```sh
cd benchmarks/assembly-line-balancing
uv sync --project cpsat --all-groups
uv run --project cpsat python -m bench.run_bench --dataset large --seconds 60
uv run --project cpsat python -m bench.run_bench \
  --dataset very-large --exact-max-size 0 --seconds 60
# The complete 525-instance sweep is explicit to avoid an accidental 8.75-hour run.
uv run --project cpsat python -m bench.run_bench \
  --dataset large --all --seconds 60 --json > salbp-large.json
```

Hexaly's published aggregate uses the `large` and `very-large` collections,
525 instances each, with a 60-second budget. Hardware and implementation
versions still need to be recorded before treating two aggregate numbers as a
strict speed comparison.
The checked-in local snapshot is in [`bench/RESULTS.md`](../../benchmarks/assembly-line-balancing/bench/RESULTS.md).

<!-- usage-and-extending -->

## Usage

```sh
cd use-cases/assembly-line-balancing/python
uv sync --all-groups --all-extras
uv run pytest -q
```

```python
from assembly_line_balancing import solve
from assembly_line_balancing.models import AssemblyLineBalancingProblemModel, PrecedenceModel, TaskModel

problem = AssemblyLineBalancingProblemModel(
    cycle_time=5,
    tasks=[TaskModel(id="A", duration=3), TaskModel(id="B", duration=3), TaskModel(id="C", duration=2)],
    precedences=[PrecedenceModel(before="A", after="B")],
)
solve(problem, time_limit_seconds=1.0, random_seed=42)
print({task.id: task.station for task in problem.tasks}, problem.stations_used)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
a full `config` (`SolverConfig`: temperature, cooling rate, stagnation
limit, …) and `constraints`.

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Tasks are matched by id; tasks left out or at station -1 are placed by the
normal construction, and a complete seed skips it. Any result of `solve` is
accepted, feasible or not: `max_stations`, station domains and `fixed_station`
stay hard penalties. Unknown ids, duplicates, stations below -1 and non-integer
stations raise `ValueError`.

```python
from assembly_line_balancing import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
solve(warm, time_limit_seconds=1.0, random_seed=42)
print({task.id: task.station for task in warm.tasks}, warm.stations_used)
```

## Extending

This is a pure Python reference implementation (simulated annealing in
`python/assembly_line_balancing/common.py`); it does not use the C++ engine.

- **Adding a move** — extend `neighbor(state, rng)` inside `solve()` in
  `python/assembly_line_balancing/__init__.py`; it returns one random neighbour of the
  current state, so a new kind of change is another branch chosen with `rng`.
- **Changing the construction heuristic** — the initial state built at the
  top of `solve()` in the same file.
- **Adding a hard or soft cost** — add a `Constraint` subclass (`name`,
  `label`, `default_hard`) to `python/assembly_line_balancing/constraints.py` and add it to the
  default list there (`DEFAULTS`), then return its raw cost under the same `name`
  from `constraint_costs()` in `__init__.py`. Mirror it in
  [`benchmarks/assembly-line-balancing/cpsat`](../../benchmarks/assembly-line-balancing/cpsat).

## Benchmark

[`benchmarks/assembly-line-balancing`](../../benchmarks/assembly-line-balancing) compares this use case with
OR-Tools CP-SAT on public instances.
