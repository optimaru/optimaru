<!-- What changes and why. Link the issue if there is one. -->

## Checklist

- [ ] Tests for the use case(s) I changed pass (`export OPTIMARU_USE_CASES=<uc>`, then `uv sync && uv run pytest -q tests/<uc>`; `ctest` for C++).
- [ ] A new move or cost: tested with `ScoreChecks::Full` / `full_assert`, and on/off on the same seeds.
- [ ] An engine change: seeded results unchanged (or the change explained) and timings in this description.
- [ ] Docs in the same PR: the use case's guide, `_core.pyi`, `CHANGELOG.md` for anything a user sees.
- [ ] Formatted (`ruff`, `clang-format`); new files carry the SPDX header.

See [CONTRIBUTING.md](https://github.com/optimaru/optimaru/blob/main/CONTRIBUTING.md).
