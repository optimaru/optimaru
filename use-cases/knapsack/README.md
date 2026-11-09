# Knapsack

The 0/1 knapsack with multidimensional resources, mandatory and forbidden
items, selection-group bounds and item dependencies, on this repository's C++
engine (`include/optimaru/`): choose the items that maximize the total value
while the selection fits the capacity and every side rule. The basic
formulation follows the official
[Hexaly knapsack template](https://www.hexaly.com/templates/knapsack-problem);
the extra contracts expose common multiple-choice and multidimensional variants
without changing the default instance format.
[`benchmarks/knapsack/cpsat`](../../benchmarks/knapsack/cpsat) holds the exact
CP-SAT reference.

## Domain (BASIC variable)

- `Item.selected` — the only planning variable: `1` = selected, `0` = not
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

## Constraints (Score — the engine score type as-is, no mapping)

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

### 0.3.0

Every constraint takes two more knobs, `offset` and `clip` (keyword-only), and may be
picked several times. Every knapsack constraint is global, so the offset shapes its single
raw total `x`: `sign(x) * max(0, |x| - offset) ** exponent` (`clip=True`, the default) or
`(x - offset) ** exponent`, sign-preserving (`clip=False`, which charges a total of 0 too
and rewards a total below the offset); `offset=0` is the 0.2.0 shaping. Both apply to that
one total, a raw 0 included. `clip=False` with an offset is refused on hard (the six
penalties by default), because hard 0 must mean no violation: a fitting capacity total
would otherwise turn into a hard reward that cancels another constraint's overflow. On the
`selected_value_cost` reward (raw `-value`) a threshold offset rewards only the value
beyond it and a literal one adds `offset` to the reward. Each pick is its own instance with
its own knobs, its level included (`level` replaced the `hard` knob; `hard=` raises
`TypeError`), and the instances add up:

```python
knapsack.solve(problem, constraints=[
    KnapsackCapacityCost(),
    ResourceCapacityCost(),                         # the resource overflow
    ResourceCapacityCost(exponent=2, offset=10),    # + (overflow beyond 10 units)^2
    SelectedValueCost(),
])
```

`knapsack.constraint_costs(problem, picks)` scores a pick list and keys the second pick
`resource_capacity_cost#2`; every entry reports its `offset`, `clip` and the `level` in
effect. The CP-SAT reference takes the same picks (integral offsets).

### 0.2.0

The package was a pure-Python reference (simulated annealing over
`constraint_costs()`); it is now the C++ engine with pybind11 bindings, like the
other C++ use cases.

- **Names** (no aliases): `capacity` -> `knapsack_capacity_cost`,
  `resource_capacities` -> `resource_capacity_cost`, `mandatory_items` ->
  `mandatory_items_cost`, `forbidden_items` -> `forbidden_items_cost`,
  `selection_group_bounds` -> `selection_group_bounds_cost`, `item_dependencies`
  -> `item_dependency_cost`, `maximize_value` -> `selected_value_cost`; the spec
  classes and module-level instances follow (`Capacity` -> `KnapsackCapacityCost`,
  `ResourceCapacities` -> `ResourceCapacityCost`, `ItemDependencies` ->
  `ItemDependencyCost`, `MaximizeValue` -> `SelectedValueCost`, every other class
  gains `Cost`). The model fields `capacity`, `resource_capacities`,
  `mandatory_items` and `forbidden_items` keep their names. The specs carry
  `label_en` / `label_ko` / `description_en` / `description_ko` instead of `label`,
  and no `default_hard` / `default_weight` (the defaults live in C++ only).
- **Raw costs and the default score are unchanged** (each constraint's total for
  every selection; `selected_value_cost` keeps its reward semantics: `(-1, 17.0)`
  stays `(-1, 17.0)`). Changed by design:
  - **exponent granularity**: the engine shapes a global constraint's total.
    0.1.5 shaped every term, so `resource_capacity_cost` and
    `selection_group_bounds_cost` are now shaped on the summed overflow /
    violation (0.1.5: per resource / per group), and `mandatory_items_cost`,
    `forbidden_items_cost` and `item_dependency_cost` on their count (0.1.5 shaped
    each 0/1 entry, so the exponent had no effect: 3 missing mandatory items with
    `exponent=2` cost 9 now, 3 before). `knapsack_capacity_cost` and
    `selected_value_cost` were one term already and are unchanged. Identical at
    exponent 1 (CP-SAT rejects any other exponent);
  - hard contributions are rounded per constraint (0.1.5 rounded the hard total
    once): different only for fractional hard weights (two constraints at weight
    0.5 with one violation each: `-2`, was `-1`);
  - `constraint_costs()` returns the standard dict
    `{name: {"costs", "weight", "exponent", "hard", "global", "level", "reward",
    "level_flippable", "name", "label"}}` with one total per constraint in
    `"costs"` (0.1.5: `{name: [one term per resource | entry | group | pair]}`);
  - `selected_value_cost` with `level="hard"` raises `ValueError` (0.1.5 scored a
    positive hard score);
  - a constraint picked twice raised `ValueError` (0.1.5 counted it twice; since
    0.3.0 each pick is scored again).
- **`SolverConfig`** is the engine's (the other C++ packages' knobs:
  `spent_limit_seconds`, `step_count_limit`, `unimproved_*`, `random_seed`,
  `parallel_solver_count`, `late_acceptance_size`, `full_assert`, `ruin_recreate*`,
  `change/swap_move_weight`, `descent_weight`, `nearby_probability` /
  `nearby_size`, `best_score_limit`, `on_new_best`), set by attribute. The
  simulated-annealing knobs `max_iterations`, `stagnation_limit`, `temperature`,
  `cooling_rate`, the keyword constructor and the `anneal` / `configure` /
  `_greedy_selection` helpers are gone. Zero budget = `spent_limit_seconds = 0`
  (`step_count_limit = 0` means "no limit"); a bounded search =
  `step_count_limit = N`. Without a time limit a solve now runs the 30 s budget
  (0.1.5 stopped after 50 000 iterations). `on_new_best(step, score)` (0.1.5:
  `on_new_best(state)`).
- `solve()` returns the engine `Score` (`.hard`, `.soft`, `is_feasible()`; 0.1.5:
  a tuple) and takes the engine object or the model (written back in place).
  `calculate_score(problem, constraints)` returns the engine `Score` for the
  defaults and a `constraints.Score(hard, soft)` named tuple for a selection.
  `DEFAULTS` and the top-level `score` are gone (use `constraints.get_constraints()`
  / `constraints.score()`). With `constraints=...` a solve is single-start
  (`parallel_solver_count` is ignored), as in every C++ package.
- `KnapsackProblemModel.to_core()` / `from_core()` convert to and from the real
  engine object (0.1.5: model copies); the JSON input/output schema is unchanged.
  `from_core()` returns each item's `resource_usage` with its keys sorted (the
  engine keeps it in a map; the dict compares equal, 0.1.5 kept the input order).
  `solve(model)` writes only the selection and the results back, so the model
  keeps its own order.

## cpp/

- `include/ks/domain.hpp` — domain + `optimaru::SolutionTraits` specialization
- `include/ks/constraints/` — one component per constraint:
  `knapsack_capacity_cost.hpp`, `resource_capacity_cost.hpp`,
  `mandatory_items_cost.hpp`, `forbidden_items_cost.hpp`,
  `selection_group_bounds_cost.hpp`, `item_dependency_cost.hpp`,
  `selected_value_cost.hpp` (+ `common.hpp`: the aliases and the unit-1
  `SelectionConstraint` base); `include/ks/constraints.hpp` assembles the default
  set (`defaultConstraints()`, also the catalog order)
- `construction` — the 0.1.5 greedy, ported exactly (stable descending
  value / weight order, weight capacity only; decided items kept)
- `score_calculator` — `prepare()` (validation + derived indexes) and the full
  recompute (the reference)
- `incremental_calculator` — O(resources + groups + dependencies of the item) per flip
- `solver` — engine wrapper (greedy construction + Late Acceptance over flip / swap
  moves + adaptive ruin & recreate, also as an ordinary move; multi-start through
  the engine)
- `src/main.cpp` — the web demo (camping trip) solved and explained per constraint

```bash
cmake -S use-cases/knapsack/cpp -B build/knapsack -DCMAKE_BUILD_TYPE=Release
cmake --build build/knapsack -j4
ctest --test-dir build/knapsack --output-on-failure --no-tests=error
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

## Usage

```sh
cd use-cases/knapsack/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import knapsack
from knapsack.models import ItemModel, KnapsackProblemModel

problem = KnapsackProblemModel(
    capacity=10,
    items=[ItemModel(id="A", weight=6, value=9), ItemModel(id="B", weight=4, value=7),
           ItemModel(id="C", weight=5, value=8)],
)
print(knapsack.solve(problem, time_limit_seconds=1.0, random_seed=42))   # 0hard/16soft; the model, in place
print([item.id for item in problem.items if item.selected], problem.total_value)

core = problem.to_core()                                                  # or the engine object
knapsack.solve(core, time_limit_seconds=1.0, random_seed=42)
print(KnapsackProblemModel.from_core(core).total_weight)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`,
`stop_at_first_feasible`, `on_new_best`, a full `config` (`knapsack.SolverConfig`)
and `constraints`:

```python
from knapsack.constraints import ForbiddenItemsCost, SelectedValueCost, get_constraints, score

knapsack.constraint_info()     # [ConstraintInfo(name='knapsack_capacity_cost', default_level='hard', ...), ...]
knapsack.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:-1], SelectedValueCost(weight=2)])
raw = knapsack.constraint_costs(problem)            # raw totals, re-priced in pure Python:
score(raw), score(raw, [*get_constraints()[:3], ForbiddenItemsCost(level="soft"), *get_constraints()[4:]])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping of the total (clip=True: only the value beyond 10 is rewarded).
score(raw, [SelectedValueCost(), SelectedValueCost(weight=0.5, offset=10)])
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose
search starts from `solution`, a solved model or the same thing as a JSON dict.
Items are matched by id. `selected: false` means not selected and is kept as it
is; items the solution leaves out go through the greedy selection around the
seeded load (`knapsack._core.greedy_selection`). With a zero budget
(`spent_limit_seconds = 0`) the seed comes back unchanged. Unknown ids,
duplicates and non-boolean values raise `ValueError`.

```python
from knapsack import with_initial_solution

warm = with_initial_solution(problem, problem.model_dump(mode="json"))
print(knapsack.solve(warm, time_limit_seconds=1.0, random_seed=42))
print([item.id for item in warm.items if item.selected], warm.total_value)
```

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The planning variable is **BASIC**; the domain solver that configures the engine is
`cpp/include/ks/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes flip (change) and swap moves (weights in
`Solver::Config`) and runs ruin-and-recreate both on stagnation and as an ordinary
move. A new move (a 2-for-1 exchange, say) is an `optimaru::MoveOperator<…>`
registered with `engine.add_move_operator(...)` in `cpp/src/solver.cpp`; registered
operators do not reach the engine's multi-start threads, so a solver that registers
one runs its own worker threads (see facility-location's `CloseFacilityMove`).

**Changing the construction heuristic.** `cpp/src/construction.cpp` decides every item
before the engine starts. It is the 0.1.5 greedy, ported exactly; `ks_test` and the
Python reference-parity tests pin its results (and `with_initial_solution` uses it).

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/ks/constraints/`:
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a
   `detail::SelectionConstraint` — a member-change `GlobalConstraint` told when an item
   becomes selected or stops being selected; positive raw cost = penalty, 0 = no
   violation) declaring `static constexpr double kDefaultWeight` and
   `static constexpr optimaru::Level kDefaultLevel`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/ks/constraints.hpp` and add it to `defaultConstraints()` if
   it belongs to the default set (that is also the catalog order);
3. for a default constraint, add the same raw cost to `score_calculator` /
   `incremental_calculator`, weighted by the component's `kDefaultWeight` —
   `ks_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `ks_test` / `ks_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/knapsack/constraints.py` (plus its
   module-level default instance); callers then pick, reweight or flip it:
   `constraints=[..., MyRuleCost(weight=5, level="hard")]`;
5. mirror it in the CP-SAT model in
   [`benchmarks/knapsack/cpsat`](../../benchmarks/knapsack/cpsat) (a plugin subclassing
   the spec; defaults come from `constraint_info()`), with an exact encoding so
   `test_constraint_parity.py` can pin selections.

## Benchmark

[`benchmarks/knapsack`](../../benchmarks/knapsack) compares this use case with OR-Tools
CP-SAT on the same instances, rescored by the same evaluator.

Latest run (10 s per solver, 3 instances): best score by the engine on 2, CP-SAT on 3
(ties count for each). Full table: [benchmarks/knapsack/README.md](../../benchmarks/knapsack/README.md#results).
