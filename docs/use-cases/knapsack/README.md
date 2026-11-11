# Knapsack

The 0/1 knapsack with multidimensional resources, mandatory and forbidden
items, selection-group bounds and item dependencies, on this repository's C++
engine (`optimaru/cpp/optimaru/`): choose the items that maximize the total value
while the selection fits the capacity and every side constraint. The basic
formulation is the classic 0-1 knapsack problem (Martello & Toth 1990);
the extra contracts expose common multiple-choice and multidimensional variants
without changing the default instance format.
[`benchmarks/knapsack/cpsat`](../../../benchmarks/knapsack/cpsat) holds the exact
CP-SAT reference.

## Usage

```sh
OPTIMARU_USE_CASES=knapsack uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/knapsack
```

```python
from optimaru import knapsack
from optimaru.knapsack.models import ItemModel, KnapsackProblemModel

problem = KnapsackProblemModel(
    capacity=10,
    items=[ItemModel(id="A", weight=6, value=9), ItemModel(id="B", weight=4, value=7),
           ItemModel(id="C", weight=5, value=8)],
)
problem = knapsack.solve(problem, time_limit_seconds=1.0, random_seed=42)   # the model: returns a new, solved model
print(problem.score_hard, problem.score_soft)                             # 0 16.0
print([item.id for item in problem.items if item.selected], problem.total_value)

core = problem.to_core()                                                  # or the engine object
print(knapsack.solve(core, time_limit_seconds=1.0, random_seed=42).score)   # 0hard/16soft
print(KnapsackProblemModel.from_core(core).total_weight)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `full_assert`, `on_new_best`, a full `config` (`knapsack.SolverConfig`)
and `constraints`:

```python
from optimaru.knapsack.constraints import ForbiddenItemsCost, SelectedValueCost, get_constraints, score

knapsack.constraint_info()     # [ConstraintInfo(name='knapsack_capacity_cost', default_level='hard', ...), ...]
problem = knapsack.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:-1], SelectedValueCost(weight=2)])
raw = knapsack.constraint_costs(problem)            # raw totals, re-priced in pure Python:
score(raw), score(raw, [*get_constraints()[:3], ForbiddenItemsCost(level="soft"), *get_constraints()[4:]])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping of the total (clip=True: only the value beyond 10 is rewarded).
score(raw, [SelectedValueCost(), SelectedValueCost(weight=0.5, offset=10)])
```

## Domain (assignment shape)

- `Item.selected` — the only decision variable: `1` = selected, `0` = not
  selected (the value range is `{0, 1}`, so the engine's change move is a flip
  and its swap move trades a selected item for an unselected one). `-1`
  (undecided) exists only transiently in the engine's ruin-and-recreate or for
  a C++ caller, and counts as not selected everywhere.
- Facts: `capacity`; per item `weight` (> 0), `value` (>= 0), `resource_usage`
  and `requires` (`required_items` on the engine object); `resource_capacities`
  (in declaration order), `mandatory_items`, `forbidden_items` (an id may
  repeat; each entry counts), `selection_groups` (`min_selected`,
  `max_selected`, None = the group size).
- `warm_start` — start from the selection on the items; a plain solve ignores the
  input selection and starts from the greedy construction. Every solve resets it.

## Constraints

The score is the engine's `Score` type as it is, with no mapping.

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (one total; 0 = no violation) |
|---|---|---|---|---|
| `knapsack_capacity_cost` | `KnapsackCapacityCost` | hard | 1 | `max(0, selected weight - capacity)` |
| `resource_capacity_cost` | `ResourceCapacityCost` | hard | 1 | sum over declared resources of `max(0, selected usage - capacity)` (usage of an undeclared resource is ignored) |
| `mandatory_items_cost` | `MandatoryItemsCost` | hard | 1 | mandatory entries not selected (an id listed twice counts twice) |
| `forbidden_items_cost` | `ForbiddenItemsCost` | hard | 1 | forbidden entries selected (an id listed twice counts twice) |
| `selection_group_bounds_cost` | `SelectionGroupBoundsCost` | hard | 1 | per group `max(0, min_selected - n) + max(0, n - max_selected)`, `n` = selected members |
| `item_dependency_cost` | `ItemDependencyCost` | hard | 1 | `(item, required)` pairs with the item selected and the required item not (a self-dependency never counts) |
| `selected_value_cost` | `SelectedValueCost` | soft | 1 | **reward**: `-(selected value)`, always <= 0, so the soft score is `+selected value` |

Every constraint is global (one raw total, kept up to date from each flip), all
seven are scored by default (`default_enabled`), and the six hard ones are
flippable (`level="hard"` / `level="soft"`, or `Level.HARD` / `Level.SOFT`;
`level="medium"` raises `ValueError`, the domain has no medium level).
`selected_value_cost` is a reward: it is always soft — `level="hard"` raises
`ValueError` in `solve`, `calculate_score`, `constraints.score()` and the CP-SAT
reference — and its level cannot change.
`knapsack.constraint_info()` reports exactly this table from the C++ components
(`kDefaultLevel` / `kDefaultWeight`).

### Knobs: exponent, offset, clip, repeats

Every constraint takes `weight`, `level` and `exponent`, plus `offset` and `clip`
(keyword-only), and may be picked several times. Every knapsack constraint is global, so
the knobs shape its single raw total `x`: `sign(x) * max(0, |x| - offset) ** exponent`
(`clip=True`, the default) or `(x - offset) ** exponent`, sign-preserving (`clip=False`,
which charges a total of 0 too and rewards a total below the offset); `offset=0` is the
plain shaping. The exponent therefore shapes the summed overflow of
`resource_capacity_cost` / `selection_group_bounds_cost` and the count of
`mandatory_items_cost`, `forbidden_items_cost` and `item_dependency_cost` (3 missing
mandatory items with `exponent=2` cost 9). Hard contributions are rounded per constraint
(two constraints at weight 0.5 with one violation each: `-2`). `clip=False` with an offset
is refused on hard (the six penalties by default), because hard 0 must mean no violation: a
fitting capacity total would otherwise turn into a hard reward that cancels another
constraint's overflow. On the `selected_value_cost` reward (raw `-value`) a threshold
offset rewards only the value beyond it and a literal one adds `offset` to the reward.
Each pick is its own instance with its own knobs, its level included, and the instances
add up:

```python
solved = knapsack.solve(problem, constraints=[
    KnapsackCapacityCost(),
    ResourceCapacityCost(),                         # the resource overflow
    ResourceCapacityCost(exponent=2, offset=10),    # + (overflow beyond 10 units)^2
    SelectedValueCost(),
])
```

`knapsack.constraint_costs(problem, picks)` returns
`{name: {"costs", "weight", "exponent", "offset", "clip", "global", "level", "reward",
"level_flippable", "name", "label", ...}}` with one total per constraint in `"costs"`,
keys the second pick `resource_capacity_cost#2`, and reports every entry's `offset`, `clip`
and the `level` in effect. The CP-SAT reference takes the same picks (integral offsets; it
rejects an exponent other than 1).

### Python API

- **`SolverConfig`** is the engine's (the other C++ packages' knobs:
  `spent_limit_seconds`, `step_count_limit`, `unimproved_*`, `random_seed`,
  `parallel_solver_count`, `late_acceptance_size`, `full_assert`, `ruin_recreate*`,
  `change/swap_move_weight`, `descent_weight`, `nearby_probability` /
  `nearby_size`, `best_score_limit`, `on_new_best`), set by attribute. Zero budget =
  `spent_limit_seconds = 0` (`step_count_limit = 0` means "no limit"); a bounded search =
  `step_count_limit = N`. Without a time limit a solve runs the 30 s budget.
  `on_new_best(step, score)`.
- `solve()` returns the solved problem: the engine object it was given, solved in place (its
  `score` is the engine `Score`: `.hard`, `.soft`, `is_feasible()`), or, for a model, a new
  solved model with `score_hard` / `score_soft` (the model passed in is left as it was). `calculate_score(problem,
  constraints)` returns the engine `Score` for the defaults and a
  `constraints.Score(hard, soft)` named tuple for a selection; `constraints.score()`
  re-prices raw costs. With `constraints=...` a solve is single-start
  (`parallel_solver_count` is ignored), as in every C++ package.
- `KnapsackProblemModel.to_core()` / `from_core()` convert to and from the real engine
  object. `from_core()` returns each item's `resource_usage` with its keys sorted (the
  engine keeps it in a map). `solve(model)` returns a copy of the model with only the
  selection and the results filled in, so it keeps the model's own order.

## C++

- `optimaru/knapsack/cpp/include/ks/domain.hpp` — domain + `optimaru::SolutionTraits` specialization
- `optimaru/knapsack/cpp/include/ks/constraints/` — one component per constraint:
  `knapsack_capacity_cost.hpp`, `resource_capacity_cost.hpp`,
  `mandatory_items_cost.hpp`, `forbidden_items_cost.hpp`,
  `selection_group_bounds_cost.hpp`, `item_dependency_cost.hpp`,
  `selected_value_cost.hpp` (+ `common.hpp`: the aliases and the unit-1
  `SelectionConstraint` base); `optimaru/knapsack/cpp/include/ks/constraints.hpp` assembles the default
  set (`defaultConstraints()`, also the catalog order)
- `construction` — the greedy construction (stable descending value / weight order,
  weight capacity only; decided items kept), matching the Python reference
  (`tests/knapsack/reference_knapsack.py`)
- `score_calculator` — `prepare()` (validation + derived indexes) and the full
  recompute (the reference)
- `incremental_calculator` — O(resources + groups + dependencies of the item) per flip
- `solver` — engine wrapper (greedy construction + Late Acceptance over flip / swap
  moves + adaptive ruin & recreate, also as an ordinary move; multi-start through
  the engine)
- `optimaru/knapsack/cpp/src/main.cpp` — the web demo (camping trip) solved and explained per constraint

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPTIMARU_USE_CASES=knapsack
cmake --build build -j4
ctest --test-dir build --output-on-failure --no-tests=error
./build/knapsack/ks_demo 2
```

`ks_test`: per-constraint hand checks (positive and zero) on the full recompute and
the incremental calculator, incremental == full recompute over random flip / swap /
undecided / batch moves (the score and every constraint's raw total after each move,
with every constraint non-zero during the walk), the greedy cases, ScoreChecks::Full
solves, brute-force optima (0/1 multidimensional instances and instances with every
fact), the zero budget with a seed, determinism, multi-start and validation.
`ks_plugin_test`: default assembly == monolith bit for bit (score and raw totals),
plugin incremental == full recompute per constraint after each move, partial
assemblies, overrides (exponent on the total, per-constraint hard rounding), raw
costs by hand, a ScoreChecks::Full plugin solve, the catalog and the reward guards, and
repeated / shifted components (a hand-calculated repeat, threshold and literal offsets
by hand, the literal formula refused on hard at reset and kept on soft, incremental == a
fresh recompute over a random walk, a ScoreChecks::Full solve).

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Items are matched by id. `selected: false` means not selected and is kept as it
is; items the solution leaves out go through the greedy selection around the
seeded load (`optimaru.knapsack._core.greedy_selection`). With a zero budget
(`spent_limit_seconds = 0`) the seed comes back unchanged. Unknown ids,
duplicates and non-boolean values raise `ValueError`.

```python
from optimaru.knapsack import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
warm = knapsack.solve(warm, time_limit_seconds=1.0, random_seed=42)
print([item.id for item in warm.items if item.selected], warm.total_value)
```

## Extending

Engine contracts and code skeletons: [docs/extending.md](../../extending.md).
The solution has the **assignment** shape; the domain solver that configures the engine is
`optimaru/knapsack/cpp/include/ks/solver.hpp` + `optimaru/knapsack/cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes flip (change) and swap moves (weights in
`Solver::Config`) and runs ruin-and-recreate both on stagnation and as an ordinary
move. A new move (a 2-for-1 exchange, say) is an `optimaru::MoveOperator<…>`
registered with `engine.add_move_operator(...)` in `optimaru/knapsack/cpp/src/solver.cpp`; registered
operators do not reach the engine's multi-start threads, so a solver that registers
one runs its own worker threads (see facility-location's `CloseFacilityMove`).

**Changing the construction heuristic.** `optimaru/knapsack/cpp/src/construction.cpp` decides every item
before the engine starts. It is the reference's greedy; `ks_test` and the
Python reference-parity tests pin its results (and `with_initial_solution` uses it).

**Adding a hard or soft cost.** One file per constraint under
`optimaru/knapsack/cpp/include/ks/constraints/`:
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a
   `detail::SelectionConstraint` — a member-change `GlobalConstraint` told when an item
   becomes selected or stops being selected; positive raw cost = penalty, 0 = no
   violation) declaring `static constexpr double kDefaultWeight` and
   `static constexpr optimaru::Level kDefaultLevel`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `optimaru/knapsack/cpp/include/ks/constraints.hpp` and add it to `defaultConstraints()` if
   it belongs to the default set (that is also the catalog order);
3. for a default constraint, add the same raw cost to `score_calculator` /
   `incremental_calculator`, weighted by the component's `kDefaultWeight` —
   `ks_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `ks_test` / `ks_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `optimaru/knapsack/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, level="hard")]`;
5. mirror it in the CP-SAT model, `external_solver.knapsack` in
   [external-solver](https://github.com/optimaru/external-solver) (a plugin subclassing the
   spec; defaults come from `constraint_info()`), with an exact encoding so its
   `tests/knapsack/test_constraint_parity.py` can pin selections;
   [`benchmarks/knapsack/cpsat`](../../../benchmarks/knapsack/cpsat) runs it against this engine.

## Benchmark

[`benchmarks/knapsack`](../../../benchmarks/knapsack) compares this use case with OR-Tools
CP-SAT on the same instances, rescored by the same evaluator.

Latest run (30 s per solver, 7 instances): best score by the engine on 3, CP-SAT on 7
(ties count for each). Full table: [benchmarks/knapsack/README.md](../../../benchmarks/knapsack/README.md#results).
