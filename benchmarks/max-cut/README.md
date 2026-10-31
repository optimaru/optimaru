# Max cut benchmark

The [`max-cut`](../../use-cases/max-cut) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `max_cut_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/max-cut/cpsat
uv sync --all-groups --all-extras
uv run pytest -q                       # CP-SAT model + parity tests
uv run max-cut-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.

<!-- results:start -->
## Results

Scores come from the use case's own evaluator. Higher is better; ties are all bold.

| Instance | Size | optimaru engine | OR-Tools CP-SAT |
| --- | --- | --- | --- |
| g30 | 30 vertices / 125 edges | 0hard/477soft | **0hard/479soft** |
| g80 | 80 vertices / 313 edges | 0hard/1321soft | **0hard/1327soft** |
| g200 | 200 vertices / 1067 edges | 0hard/4309soft | **0hard/4353soft** |

Measured 2026-10-29 on one Apple Silicon Mac (16 cores), 30 s per solver and instance, seed 1. The engine run single-threaded; CP-SAT uses 8 workers and its time includes model construction. Single runs — treat small differences as noise.
<!-- results:end -->
