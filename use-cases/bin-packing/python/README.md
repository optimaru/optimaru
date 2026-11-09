# bin_packing (Python)

The pybind11 package of the bin-packing C++ use case (`../cpp`). Puts every item in
one of n identical bins — `Item.bin` is the only planning variable (bin index, -1 =
unassigned).

Score, at the default weights (1 each):

- hard = -(load above capacity, per bin — `bin_capacity_cost`
  + items beyond the first of their colour, per bin — `color_conflict_cost`
  + listed conflict pairs sharing a bin — `item_conflict_cost`)
- soft = -(bins in use) — `used_bins_cost`

The problem carries no weights (removed in 0.2.0, together with the rename of
`bin_capacity` / `color_compatibility` / `item_conflicts` / `minimize_bins` to
`bin_capacity_cost` / `color_conflict_cost` / `item_conflict_cost` / `used_bins_cost`).
Pick, re-weight or flip constraints with `bin_packing.constraints`, and read the
defaults with `bp.constraint_info()`:

```python
import bin_packing as bp
from bin_packing.constraints import UsedBinsCost, get_constraints, score

bp.constraint_info()
# [ConstraintInfo(name='bin_capacity_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...]
bp.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:3], UsedBinsCost(weight=2)])
raw = bp.constraint_costs(problem)   # raw per-bin costs (used_bins_cost: [count])
score(raw), score(raw, [UsedBinsCost(exponent=2.0)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping (clip=True: only the bins beyond the tenth count).
score(raw, [UsedBinsCost(), UsedBinsCost(weight=100, offset=10)])
```

`weight` (None = the default, 1), `level` (None = the default level, otherwise "hard" or
"soft", or `Level.HARD` / `Level.SOFT`; the domain has no medium level, so "medium" raises
`ValueError`), `exponent` (1.0) and the keyword-only `offset` (0.0) / `clip` (True) are the
knobs. The exponent shapes each bin's cost for the three per-bin constraints and the bin
count for `used_bins_cost`; 0.1.x shaped colour excess per (bin, colour) and conflicts per
pair, so only exponent != 1 scores change (totals and exponent-1 scores are identical).
`offset` / `clip` (0.3.0) shift that shaping at the same granularity: `clip=True` counts
only what lies beyond the offset, `clip=False` shapes `cost - offset` as it is, on every
bin, an empty one included (per bin, not over the sum). That is refused on hard, where
hard 0 must mean no violation: `ValueError: constraint 'bin_capacity_cost': clip=False with
a non-zero offset cannot be hard, because hard 0 must mean no violation; use clip=True (a
threshold) or another level`. On soft it is allowed.

## Install / test

```bash
uv sync --all-groups --all-extras   # builds the extension
uv run pytest -q
```

`tests/reference_bin_packing.py` is the frozen 0.1.x pure-Python scorer and
construction; `tests/test_reference_parity.py` checks the C++ port against it.

## Usage

```python
import bin_packing as bp

problem = bp.BinPackingProblem(
    10,
    [bp.Item("A", 8, color="red"), bp.Item("B", 7), bp.Item("C", 3, color="red"), bp.Item("D", 2)],
    conflicts=[("A", "D")],
)
bp.solve(problem, time_limit_seconds=1.0, random_seed=42)   # in-place solve
print(problem.score)                                        # 0hard/-3soft: A shares with nobody
print(problem.assignment(), problem.bins_used(), problem.bin_loads())
```

For finer control build `bp.SolverConfig()` and set attributes (time budget
`spent_limit_seconds`, `step_count_limit`, `parallel_solver_count`, the ALNS knobs, the
move mix, the bin-emptying macro moves `empty_bin_moves` / `empty_bin_candidates` and
`repack_moves` / `repack_partner_bins` (see the use case README's
[Search](../README.md#search)), `best_score_limit`, the
`on_new_best(step, score)` callback, `full_assert`, ...), then call
`bp.Solver(cfg).solve(problem)` or `bp.solve(problem, config=cfg)`. The 0.1.x
simulated-annealing knobs (`max_iterations`, `stagnation_limit`, `temperature`,
`cooling_rate`) are gone: zero budget is `spent_limit_seconds = 0`, a fixed number of
steps is `step_count_limit`.

The JSON contract is `bin_packing.models`:

```python
from bin_packing.models import BinPackingProblemModel

model = BinPackingProblemModel.from_core(problem)            # + score_hard, score_soft, bins_used, bin_loads
problem2 = BinPackingProblemModel.model_validate_json(model.model_dump_json()).to_core()
```

Note: `problem.items[i]` returns a copy — write bins in bulk with
`problem.set_assignment([...])`.
