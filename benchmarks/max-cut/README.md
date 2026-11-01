# Max cut benchmark

The [`max-cut`](../../use-cases/max-cut) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `max_cut_cpsat`). The CP-SAT model implements the
same constraints under the same names and units — one `cpsat_plugins`
`ConstraintPlugin` per constraint (`max_cut_cpsat/constraints.py`), each a
subclass of the domain spec with its defaults read from
`max_cut.constraint_info()` — so both answers are rescored by the use case's
own evaluator. The model has one bool per vertex (its side) and one per edge
(crossed); every term is an exact function of the sides (equalities, not
one-sided bounds), and the first vertex is put on side 0 when nothing is fixed
(every constraint is symmetric under swapping the sides; `symmetry_breaking=False`
turns it off). `cpsat/tests/test_constraint_parity.py` pins every partition of
small instances (and 60 seeded random instances) and checks that each
constraint's CP-SAT term equals the engine's `constraint_costs()` total (the sum
of its per-side costs; 0 exactly when the engine reports no violation, `-cut` for
the `cut_weight_cost` reward), and that the CP-SAT optimum equals the brute-force
optimum of the engine's score, per constraint and for the default set.
Exponents other than 1 and fractional weights are not supported on CP-SAT (they
raise `ValueError`); `level="soft"` / `level="hard"` moves a term to the soft / hard
level in both solvers, `level="medium"` is refused by both (`this use case has no medium
level`), and `cut_weight_cost` with `level="hard"` is refused as in the engine.

**0.3.0:** a constraint may be picked several times (as specs or as
`{"name", "weight", "level", "offset", "clip"}` mappings); each pick builds its own terms with
its own knobs, its level included. The four unit constraints add one term per side (side 0
and side 1, a side of raw 0 included) and the imbalance and the cut one term for their
total, so an integral `offset` shapes each side (or the total) exactly as the engine does
in both `clip` modes; a fractional offset is refused (`offset must be an integer for
CP-SAT`).

```sh
cd benchmarks/max-cut/cpsat
uv sync --all-groups --all-extras
uv run pytest -q                       # CP-SAT model + engine parity tests
uv run max-cut-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.

<!-- results:start -->
## Results

Scores come from the use case's own evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| g30 | 30 vertices / 125 edges | **0hard/479soft** | **0hard/479soft** |
| g80 | 80 vertices / 313 edges | **0hard/1320soft** | **0hard/1320soft** |
| g200 | 200 vertices / 1067 edges | **0hard/4336soft** | 0hard/4268soft |

Measured 2026-10-31 with the 0.2.0 C++ engine on a shared 4-core Linux container,
30 s per solver and instance, seed 1. The engine runs single-threaded; CP-SAT uses
2 workers and its time includes model construction. Single runs — treat small
differences as noise. (The 0.1.5 pure-Python engine, 30 s on a 16-core Mac with
CP-SAT on 8 workers, reached 477 / 1321 / 4309 against CP-SAT's 479 / 1327 /
4353.)
<!-- results:end -->
