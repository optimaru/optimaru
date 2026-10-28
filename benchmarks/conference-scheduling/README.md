# Conference scheduling benchmark

The [`conference-scheduling`](../../use-cases/conference-scheduling) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `conference_scheduling_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/conference-scheduling/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run conference-scheduling-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.
