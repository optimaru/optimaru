# Contributing to optimaru

Most contributions add a **move** or a **constraint** to one of the 22 use
cases. Neither needs a change to the engine: both plug in from the use case.
This page is the short version; [`docs/extending.md`](docs/extending.md) is the
step-by-step guide with real code to copy from.

## Set up

```sh
git clone https://github.com/optimaru/optimaru && cd optimaru
export OPTIMARU_USE_CASES=<uc>           # build and test one use case
uv sync                                  # builds optimaru.<uc>._core in place
uv run pytest -q tests/<uc>

cmake -B build -DOPTIMARU_USE_CASES=<uc> && cmake --build build -j
ctest --test-dir build --output-on-failure   # the engine tests and <uc>'s C++ tests
```

`<uc>` is the subpackage name, such as `vehicle_routing` or `bin_packing`. Leave
`OPTIMARU_USE_CASES` unset to build all 22, and keep it exported while you test:
uv rebuilds when it changes, and the two contract tests
(`tests/test_solve_contract.py`, `tests/test_solver_config_contract.py`) skip
only the use cases it leaves out. You need a C++17 compiler, CMake and uv.

## Where things are

| You want to change | Look in |
| --- | --- |
| The search itself (step loop, acceptors, built-in moves, ruin and recreate) | `optimaru/cpp/optimaru/` (header-only engine, no domain code) |
| One use case's model, constraints, moves | `optimaru/<uc>/cpp/include/<ns>/`, `optimaru/<uc>/cpp/src/` |
| What Python sees | `optimaru/<uc>/cpp/bindings.cpp`, `_core.pyi`, `constraints.py`, `models.py` |
| Tests | `tests/<uc>/` (pytest), `tests/<uc>/cpp/` (C++), `tests/engine/` |
| Guides | `docs/use-cases/<slug>/`, `docs/extending.md` |

The README has the full [repository layout](README.md#repository-layout) and
[engine layout](README.md#engine-layout).

## Add a constraint

A constraint is one named cost component. Callers pick it, reweight it and move
it between hard, medium and soft per solve.

1. **Write the component**: `optimaru/<uc>/cpp/include/<ns>/constraints/<subject>_cost.hpp`,
   a struct `<Subject>Cost` with `kDefaultWeight`, `kDefaultLevel` and a
   `cost(solution, unit, members)` that returns the raw cost of one unit
   (a route, an employee, a bin). `0` means no violation.
2. **Add it to the default set**: include it in `constraints.hpp` and
   `emplace` it in `defaultConstraints()`. Start with `kDefaultWeight = 0.0`
   (opt-in): default solves and published scores stay as they are.
3. **Expose it to Python**: a spec class in `optimaru/<uc>/constraints.py`
   (name, labels, descriptions), its default instance in `_DEFAULTS`.
4. **Test it**: a hand-calculated case in `tests/<uc>/cpp/<ns>_plugin_test.cpp`,
   and the catalog rows in `tests/<uc>/test_constraint_specs.py`.
5. **Document it**: the constraint tables in `docs/use-cases/<slug>/`.

Full steps, including on-by-default costs and global constraints:
[Adding a cost](docs/extending.md#adding-a-cost).

## Add a move

Check first whether reweighting the built-in moves (`MoveMix`) does the job.
A domain move is for a step they cannot express, such as "empty the lightest
bin".

1. **Write the operator**: an `optimaru::MoveOperator<Solution>` with
   `apply()` (do one random move through `ctx.editor`) and `undo_last()`
   (revert it exactly).
2. **Register it**: `engine.add_move_operator(std::make_unique<MyMove>())` in
   `optimaru/<uc>/cpp/src/solver.cpp`. The engine picks between it and the
   built-in moves adaptively.
3. **Expose a switch**: a `Solver::Config` field, bound in `bindings.cpp` and
   declared in `_core.pyi`, so the move can be turned off for comparison.
4. **Test it**: a solve with `ScoreChecks::Full`, which compares every
   incremental score with a full recompute, and evidence that the move helps
   (same seeds and budget, on and off).

Full steps, plus custom ruins and construction heuristics:
[Adding a move](docs/extending.md#adding-a-move).

## Changing the engine

The engine is used by every use case, so a change there has to keep all of
them correct and as fast as before.

- Run the engine tests (`ctest`) and every use case's tests (`uv sync && uv run
  pytest -q`) before opening the PR.
- A refactor must not change what a seeded solve does: the same seed and step
  limit give the same score before and after. Say in the PR how you checked,
  and include timings for a change on the step loop or in
  `score/decomposed_objective.hpp`.
- Engine code is snake_case functions and PascalCase types, in the flat
  `optimaru::` namespace. Use-case code keeps its own style (camelCase).

## Pull requests

- Everything is in English: code, comments, docs, commit messages.
- Keep docs in step with the code in the same PR: the use case's guide, the
  `_core.pyi` stubs, its notebook in `examples/`, and `CHANGELOG.md` for
  anything a user can see.
- Do not change `version` in `pyproject.toml`; a version bump publishes a
  release.
- New source files start with the SPDX header
  (`python3 scripts/check_license_headers.py` checks it).
- Format before you push; CI's `lint` job checks all three:

  ```sh
  uv run ruff check --fix . && uv run ruff format .                 # Python
  git ls-files '*.cpp' '*.hpp' | xargs uv run clang-format -i       # C++ (.clang-format)
  ```

  `uv sync` installs both tools at the versions `uv.lock` pins, and
  `pre-commit install` runs them on every commit if you use
  [pre-commit](https://pre-commit.com). Guides (`*.md`) and notebooks keep
  their hand layout: the formatter skips them.

By contributing you agree that your contribution is licensed under the
[Apache License 2.0](LICENSE).
