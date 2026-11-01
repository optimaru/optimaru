# Quadratic assignment benchmark

The [`quadratic-assignment`](../../use-cases/quadratic-assignment) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `quadratic_assignment_cpsat`). The CP-SAT model implements the
same constraints under the same names and units — one `cpsat_plugins` `ConstraintPlugin` per
constraint (`quadratic_assignment_cpsat/constraints.py`), each a subclass of the domain spec with its
defaults read from `quadratic_assignment.constraint_info()` — so both answers are rescored by the use
case's own evaluator. `cpsat/tests/test_constraint_parity.py` pins every permutation of small
feature-rich instances (and 100 random ones of a 6-facility instance) and checks that each constraint's
CP-SAT term equals the engine's `constraint_costs()` total (0 exactly when the engine reports no
violation), and that the CP-SAT optimum equals the brute-force optimum of the engine's score, per
constraint and for the default set. Exponents other than 1 and fractional weights are not supported on
CP-SAT (they raise `ValueError`).

The CP-SAT model: one bool `x[f, l]` per facility and location with exactly one location per facility
and one facility per location; `pinned_assignments_cost` = sum over fixed locations of `1 - x`,
`forbidden_assignments_cost` = sum of `x` over the **distinct** forbidden pairs (0.2.0 fix: a repeated
row used to count twice here and once in the engine), `location_capacity_cost` = per capacitated
location an excess variable `>= demand - capacity`, `assignment_cost` = `sum c[f][l] * x[f, l]` and
`flow_distance_cost` = the diagonal `sum flows[a][a] * distances[l][l] * x[a, l]` plus, per facility pair
`a < b` and pair of distinct locations, a bool `y[a, b, la, lb]` with the coefficient
`flows[a][b] * distances[la][lb] + flows[b][a] * distances[lb][la]`. `y` is tied to `x` by linear
equalities (`sum over lb of y = x[a, la]`, `sum over la of y = x[b, lb]`), which on a permutation
force `y = x[a, la] AND x[b, lb]` exactly (O(n^4) pair bools and O(n^3) equalities, built only when
the constraint is selected; 0.1.x used one multiplication constraint per ordered pair, which presolves
much more slowly). Flows, distances and direct costs may be negative: the objective bounds cover each
term's whole value range, so the hard level still dominates the soft one.

**0.3.0:** a constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own terms with
its own knobs, its level included (`level="soft"` / `level="hard"` move a term in both
solvers; `level="medium"` is refused by both: `this use case has no medium level`). The
three side constraints add one term per location the engine evaluates (the fixed
locations, the locations named by a forbidden row, the capacitated locations) and the two
objectives one term for their total, so an integral `offset` shapes each
location (or the total) exactly as the engine does in both `clip` modes; a fractional
offset is refused (`offset must be an integer for CP-SAT`). The shared `ObjectiveBuilder`
shapes a term over a non-negative range, so an offset on `assignment_cost` /
`flow_distance_cost` is refused when the data can make that total negative (a negative
direct cost, flow or distance: `offset needs non-negative ... for CP-SAT`); offset 0 keeps
scoring signed data as before. `clip=False` with an offset on a hard pick is refused with
the engine's message when the objective is built, before that check. On such data the
objective cannot be hard in either solver (the engine's sign rule: a negative hard total
would cancel a broken pin), refused in the engine's words before any term is built:
`constraint 'assignment_cost' can have a negative raw cost on this problem (a bonus) and
cannot be hard`.

```sh
cd benchmarks/quadratic-assignment/cpsat
uv sync --all-groups --all-extras
uv run pytest -q                                       # CP-SAT model + engine parity tests
uv run quadratic-assignment-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The comparison instances stay small (6, 8 and 10 facilities) because of the O(n^4) pair variables.
With the 1 s CI smoke budget CP-SAT returns an 8-facility answer (not always the optimum) and is still
presolving the 10-facility model (reported as UNKNOWN); with 2 workers it proves the 8-facility optimum
in about 1.5 s, and at 10 s it matches the engine on all three instances (table below).
The engine runs single-threaded; CP-SAT gets `--workers` search workers and its time includes model
construction.

## QAPLIB (Hexaly archive)

`bench/run_bench.py` downloads QAPLIB (the Hexaly archive) with the QAPLIB best-known solutions on
demand, checksum-verified ([`bench/data.py`](bench/data.py)), and rescores every answer with
`quadratic_assignment.constraint_costs` (`flow_distance_cost`).

```sh
cd benchmarks/quadratic-assignment
uv sync --project cpsat --all-groups --all-extras
uv run --project cpsat pytest -q cpsat/tests
uv run --project cpsat python -m bench.run_bench --seconds 60
```

Recorded results: [`bench/RESULTS.md`](bench/RESULTS.md) — **0.1.x numbers** (the pure-Python
engine); not re-measured for the 0.2.0 C++ engine, because the QAPLIB archive download was not reachable
when 0.2.0 was prepared.

<!-- results:start -->
## Results

Small instances, engine vs CP-SAT. Scores come from the use case's own evaluator. Higher is better.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| plant6 | 6 facilities | **0hard/-368soft** | **0hard/-368soft** |
| random8 | 8 facilities | **0hard/-1130soft** | **0hard/-1130soft** |
| random10 | 10 facilities | **0hard/-1766soft** | **0hard/-1766soft** |

Measured 2026-10-31 with the 0.2.0 C++ engine on a shared 4-core Linux container, 10 s per solver and
instance, seed 1. The engine runs single-threaded; CP-SAT uses 2 workers and its time includes model
construction. Single runs — a snapshot, not a statistical study.

QAPLIB (Hexaly example instances). **0.1.x numbers**: measured with the 0.1.x pure-Python engine, 30 s
per solver and instance on one Apple Silicon Mac, 2026-10-29, and not re-measured for the 0.2.0 C++
engine (the archive download was not reachable). Lower is better. CP-SAT runs only up to the exact-model
size cap (`--exact-max-size`); these n=30 instances are above it.

| Instance | Size | QAPLIB best known | engine (0.1.x) | CP-SAT |
| --- | --- | --- | --- | --- |
| nug30 | 30 | 6124 | 6,156 | not run (size cap) |
| lipa30a | 30 | 13178 | 13,395 | not run (size cap) |
| tai30a | 30 | 1818146 | 1,864,536 | not run (size cap) |
<!-- results:end -->
