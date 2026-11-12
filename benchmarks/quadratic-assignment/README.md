# Quadratic assignment benchmark

The [`quadratic-assignment`](../../docs/use-cases/quadratic-assignment) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `quadratic_assignment_cpsat`). The CP-SAT model implements the
same constraints under the same names and units — one external-solver `ConstraintPlugin` per
constraint ([`external_solver.quadratic_assignment.constraints`](https://github.com/optimaru/external-solver/blob/v0.3.0/src/external_solver/quadratic_assignment/constraints.py)), each a subclass of the domain spec with its
defaults read from `quadratic_assignment.constraint_info()` — so both answers are rescored by the use
case's own evaluator. External-solver's [`tests/quadratic_assignment/test_constraint_parity.py`](https://github.com/optimaru/external-solver/blob/v0.3.0/tests/quadratic_assignment/test_constraint_parity.py) pins every permutation of small
feature-rich instances (and 100 random ones of a 6-facility instance) and checks that each constraint's
CP-SAT term equals the engine's `constraint_costs()` total (0 exactly when the engine reports no
violation), and that the CP-SAT optimum equals the brute-force optimum of the engine's score, per
constraint and for the default set. Exponents other than 1 and fractional weights are not supported on
CP-SAT (they raise `ValueError`).

The CP-SAT model: one bool `x[f, l]` per facility and location with exactly one location per facility
and one facility per location; `pinned_assignments_cost` = sum over fixed locations of `1 - x`,
`forbidden_assignments_cost` = sum of `x` over the **distinct** forbidden pairs (a repeated
row counts once, as in the engine), `location_capacity_cost` = per capacitated
location an excess variable `>= demand - capacity`, `assignment_cost` = `sum c[f][l] * x[f, l]` and
`flow_distance_cost` = the diagonal `sum flows[a][a] * distances[l][l] * x[a, l]` plus, per facility pair
`a < b` and pair of distinct locations, a bool `y[a, b, la, lb]` with the coefficient
`flows[a][b] * distances[la][lb] + flows[b][a] * distances[lb][la]`. `y` is tied to `x` by linear
equalities (`sum over lb of y = x[a, la]`, `sum over la of y = x[b, lb]`), which on a permutation
force `y = x[a, la] AND x[b, lb]` exactly (O(n^4) pair bools and O(n^3) equalities, built only when
the constraint is selected; one multiplication constraint per ordered pair presolves much more
slowly). Flows, distances and direct costs may be negative: the objective bounds cover each
term's whole value range, so the hard level still dominates the soft one.

A constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own terms with
its own knobs, its level included (`level="soft"` / `level="hard"` move a term in both
solvers; `level="medium"` is refused by both: `this use case has no medium level`). The
three side constraints add one term per location the engine evaluates (the fixed
locations, the locations named by a forbidden row, the capacitated locations) and the two
objectives one term for their total, so an integral `offset` shapes each
location (or the total) exactly as the engine does in both `clip` modes; a fractional
offset is refused (`offset must be an integer for CP-SAT`). External-solver's shared `ObjectiveBuilder`
shapes a term over a non-negative range, so an offset on `assignment_cost` /
`flow_distance_cost` is refused when the data can make that total negative (a negative
direct cost, flow or distance: `offset needs non-negative ... for CP-SAT`); with offset 0
signed data scores as usual. `clip=False` with an offset on a hard pick is refused with
the engine's message when the objective is built, before that check. On such data the
objective cannot be hard in either solver (the engine's sign rule: a negative hard total
would cancel a broken pin), refused in the engine's words before any term is built:
`constraint 'assignment_cost' can have a negative raw cost on this problem (a bonus) and
cannot be hard`.

```sh
cd benchmarks/quadratic-assignment/cpsat
OPTIMARU_USE_CASES=quadratic_assignment uv sync   # also builds this checkout's optimaru (quadratic_assignment only)
uv run pytest -q                                       # benchmark harness tests
uv run quadratic-assignment-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The comparison instances stop at 25 facilities (6, 8, 10, 12, 14, 16, 20 and 25) because of the O(n^4)
pair variables; the QAPLIB instances below start at 30 and are above the exact-model cap.
With the 1 s CI smoke budget CP-SAT returns an 8-facility answer (not always the optimum) and is still
presolving the 10-facility model (reported as UNKNOWN); with 2 workers it proves the 8-facility optimum
in about 1.5 s. At 30 s the two solvers tie up to 14 facilities, and from 16 up the engine has the
lower cost (table below).
The engine runs single-threaded. CP-SAT gets `--workers` search workers;
its budget starts once its model is built, and the time the runner reports
for it includes building the model.

## QAPLIB harness

`quadratic-assignment-public-bench` ([`public_benchmark.py`](cpsat/quadratic_assignment_cpsat/public_benchmark.py))
downloads the QAPLIB instance and solution archives on demand, checksum-verified
([`public_benchmark_data.py`](cpsat/quadratic_assignment_cpsat/public_benchmark_data.py)), and rescores every answer with
`quadratic_assignment.constraint_costs` (`flow_distance_cost`).

```sh
cd benchmarks/quadratic-assignment/cpsat
OPTIMARU_USE_CASES=quadratic_assignment uv sync   # also builds this checkout's optimaru (quadratic_assignment only)
uv run pytest -q
uv run quadratic-assignment-public-bench --seconds 60
```

Public-instance protocol and results: [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).

<!-- results:start -->
## Results

Every cell is the score of that solver's final solution under the use case's own rules, re-scored by its evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| plant6 | 6 facilities | **0hard/-368soft** | **0hard/-368soft** |
| random8 | 8 facilities | **0hard/-1130soft** | **0hard/-1130soft** |
| random8-2 | 8 facilities | **0hard/-850soft** | **0hard/-850soft** |
| random8-3 | 8 facilities | **0hard/-1054soft** | **0hard/-1054soft** |
| random10 | 10 facilities | **0hard/-1766soft** | **0hard/-1766soft** |
| random10-2 | 10 facilities | **0hard/-1048soft** | **0hard/-1048soft** |
| random10-3 | 10 facilities | **0hard/-1770soft** | **0hard/-1770soft** |
| random12 | 12 facilities | **0hard/-2292soft** | **0hard/-2292soft** |
| random12-2 | 12 facilities | **0hard/-2048soft** | **0hard/-2048soft** |
| random12-3 | 12 facilities | **0hard/-2520soft** | **0hard/-2520soft** |
| random14 | 14 facilities | **0hard/-3666soft** | **0hard/-3666soft** |
| random14-2 | 14 facilities | **0hard/-3522soft** | 0hard/-3524soft |
| random14-3 | 14 facilities | **0hard/-3332soft** | 0hard/-3402soft |
| random16 | 16 facilities | **0hard/-4020soft** | **0hard/-4020soft** |
| random16-2 | 16 facilities | **0hard/-4836soft** | 0hard/-4878soft |
| random16-3 | 16 facilities | **0hard/-3912soft** | 0hard/-3964soft |
| random20 | 20 facilities | **0hard/-7632soft** | 0hard/-7836soft |
| random20-2 | 20 facilities | **0hard/-7404soft** | 0hard/-7494soft |
| random25 | 25 facilities | **0hard/-10644soft** | 0hard/-10988soft |
| random25-2 | 25 facilities | **0hard/-12290soft** | 0hard/-12670soft |

Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses 8 workers and its budget starts once its model is built. Single runs — treat small differences as noise. How the runs were made: [benchmarks/README.md](../README.md#publishing-results).
<!-- results:end -->

<!-- qaplib:start -->
## QAPLIB

The 33 QAPLIB instances with 30 to 50 facilities. Lower is better; the gap is to the
QAPLIB best-known value. CP-SAT runs only up to the exact-model size cap (`--exact-max-size`);
these instances are above it.

The archive has no solution file for esc32a, esc32b, esc32c, esc32d, esc32h, so they have no reference and no gap.

| Instance | Size | QAPLIB best known | optimaru engine | Gap |
| --- | ---: | ---: | ---: | ---: |
| esc32a | 32 | — | 130 | — |
| esc32b | 32 | — | 168 | — |
| esc32c | 32 | — | 642 | — |
| esc32d | 32 | — | 200 | — |
| esc32e | 32 | 2 | 2 | 0.00% |
| esc32g | 32 | 6 | 6 | 0.00% |
| esc32h | 32 | — | 438 | — |
| kra30a | 30 | 88,900 | 88,900 | 0.00% |
| kra30b | 30 | 91,420 | 91,420 | 0.00% |
| kra32 | 32 | 88,700 | 88,700 | 0.00% |
| lipa30a | 30 | 13,178 | 13,178 | 0.00% |
| lipa30b | 30 | 151,426 | 151,426 | 0.00% |
| lipa40a | 40 | 31,538 | 31,538 | 0.00% |
| lipa40b | 40 | 476,581 | 476,581 | 0.00% |
| lipa50a | 50 | 62,093 | 62,093 | 0.00% |
| lipa50b | 50 | 1,210,244 | 1,210,244 | 0.00% |
| nug30 | 30 | 6,124 | 6,128 | 0.07% |
| sko42 | 42 | 15,812 | 15,812 | 0.00% |
| sko49 | 49 | 23,386 | 23,394 | 0.03% |
| ste36a | 36 | 9,526 | 9,550 | 0.25% |
| ste36b | 36 | 15,852 | 15,852 | 0.00% |
| ste36c | 36 | 8,239,110 | 8,239,110 | 0.00% |
| tai30a | 30 | 1,818,146 | 1,836,384 | 1.00% |
| tai30b | 30 | 637,117,113 | 646,036,371 | 1.40% |
| tai35a | 35 | 2,422,002 | 2,453,950 | 1.32% |
| tai35b | 35 | 283,315,445 | 283,315,445 | 0.00% |
| tai40a | 40 | 3,139,370 | 3,190,584 | 1.63% |
| tai40b | 40 | 637,250,948 | 637,250,948 | 0.00% |
| tai50a | 50 | 4,938,796 | 5,003,296 | 1.31% |
| tai50b | 50 | 458,821,517 | 458,993,732 | 0.04% |
| tho30 | 30 | 149,936 | 149,936 | 0.00% |
| tho40 | 40 | 240,516 | 240,632 | 0.05% |
| wil50 | 50 | 48,816 | 48,842 | 0.05% |

Mean gap 0.26%, median 0.00%, worst 1.63%. Measured 2026-11-12 on one Apple M4 Max Mac (16 cores), 30 s per solver and instance, seed 1. Single runs — a snapshot, not a statistical study.
<!-- qaplib:end -->
