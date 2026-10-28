# Bed allocation benchmark

The [`bed-allocation`](../../use-cases/bed-allocation) use case against **OR-Tools CP-SAT**
([`cpsat/`](cpsat), package `bed_allocation_cpsat`). The CP-SAT model implements the
same constraints under the same names and units, so both answers are
rescored by the use case's own evaluator; parity tests in `cpsat/tests`
keep the two cost definitions identical.

```sh
cd benchmarks/bed-allocation/cpsat
uv sync --all-groups --all-extras      # builds the engine extension too
uv run pytest -q                       # CP-SAT model + parity tests
uv run bed-allocation-compare --seconds 10 --workers 8   # engine vs CP-SAT table (--markdown, --json)
```

CP-SAT formulation notes: [`cpsat/README.md`](cpsat/README.md).

`dump_instances.py` writes the comparison instances for the engine-only
C++ bench (`use-cases/bed-allocation/cpp/bench/ba_quality_bench.cpp`).

The engine runs single-threaded; CP-SAT gets `--workers` search workers and
its time includes model construction.
