# Extending a use case

How to add a move or a cost to one of the use cases under `optimaru/`. The
steps are the same for all 22, with the few differences in file names listed
under the table below. The examples use real code you can open next to this
guide: bin packing's `EmptyBinMove` for a move and vehicle routing's
`LatencyCost` for a cost.

## Where things are

For a use case `<uc>` (the subpackage name, such as `vehicle_routing`) with the
C++ namespace `<ns>` (such as `vrp`):

| What | Where |
| --- | --- |
| Problem facts, decision variable, `SolutionTraits` | `optimaru/<uc>/cpp/include/<ns>/domain.hpp` |
| Engine wiring: settings, registered moves, construction | `optimaru/<uc>/cpp/include/<ns>/solver.hpp`, `optimaru/<uc>/cpp/src/solver.cpp` |
| One file per constraint | `optimaru/<uc>/cpp/include/<ns>/constraints/<subject>_cost.hpp` |
| The default constraint set | `optimaru/<uc>/cpp/include/<ns>/constraints.hpp` (`defaultConstraints()`) |
| The fast default-path calculators | `optimaru/<uc>/cpp/src/score_calculator.cpp`, `incremental_calculator.cpp` |
| The Python module `optimaru.<uc>._core` | `optimaru/<uc>/cpp/bindings.cpp`, stubs in `optimaru/<uc>/_core.pyi` |
| Constraint specs for Python | `optimaru/<uc>/constraints.py` |
| Python models (`to_core()` / `from_core()`) | `optimaru/<uc>/models.py` |
| What the use cases' Python shares | `optimaru/_constraint.py` (the constraint-spec base), `optimaru/_solve.py` (the steps of `solve()`), `optimaru/_models.py` (the model base and helpers) |
| C++ tests | `tests/<uc>/cpp/<ns>_test.cpp`, `tests/<uc>/cpp/<ns>_plugin_test.cpp` |
| Python tests | `tests/<uc>/test_*.py` |
| The use case's guide | `docs/use-cases/<slug>/README.md` and `python.md` |

Where a use case differs from the table:

- School timetabling keeps `defaultConstraints()` in
  `st/canonical_constraints.hpp`, its constraints in subdirectories of
  `st/constraints/`, and its plugin tests in `canonical_plugin_test.cpp`.
- Employee scheduling has `es_plugin_test.cpp` only, and a second catalog of
  opt-in rules (see [step 3](#3-add-it-to-the-assembly) of adding a cost).
- Employee scheduling and school timetabling have no
  `Solver.solve_with_constraints`: their `Solver.solve(x, calculator)` takes the
  picks as a calculator. The package-level `solve(x, constraints=picks)` is the
  same for all 22.

The engine is header-only, in `optimaru/cpp/optimaru/`. You never edit it to add a
move or a cost: both plug in from the use case.

## The edit, build, test loop

Run everything from the repository root. Build only the use case you are
changing:

```sh
cmake -B build -DOPTIMARU_USE_CASES=<uc> && cmake --build build -j
ctest --test-dir build --output-on-failure      # the engine tests and <uc>'s C++ tests

export OPTIMARU_USE_CASES=<uc>                  # keep it exported for the test run too
uv sync                                         # builds optimaru.<uc>._core in place
uv run pytest -q tests/<uc>
```

`uv sync` rebuilds the extension when a `.cpp` or `.hpp` under `optimaru/`
changes. If you ever get a stale `_core`, run `uv sync --reinstall-package optimaru`.

The extensions are built in place (`optimaru/<uc>/_core*.so`) and shared by every
environment that installs this checkout, the benchmark projects included. A
narrowed build (`OPTIMARU_USE_CASES=<uc>`) refreshes only that use case and
leaves the others' `_core` from earlier builds. After a change to the engine
headers, rebuild everything (`uv sync` without the variable) before you run
other use cases' tests, or they run against an old binary.

## Adding a move

### First, check the built-in moves

The engine already draws the generic moves for the solution's shape:

- assignment shape: change, swap;
- sequence shape: relocate, swap, 2-opt, Or-opt, 2-opt\* tail swap, CROSS-exchange;
- every shape: ruin and recreate applied as a move, and descent where the use case
  enables it.

Their relative weights are fields of `MoveMix` (`settings.search.moves`), usually
exposed through the use case's `Solver::Config` and, in Python, as
`SolverConfig.*_move_weight`. If a reweighting gets you what you need, stop
there. Write a domain move when the step you want can't be expressed as one of
these moves, for example "empty the lightest bin" (bin packing) or "close a
station" (assembly-line balancing).

### 1. Write the operator

A move is an `optimaru::MoveOperator<Solution>`
([`search/operators.hpp`](../optimaru/cpp/optimaru/search/operators.hpp)). Put it in
the use case's include directory, for example
`optimaru/<uc>/cpp/include/<ns>/moves.hpp` (bin packing keeps its moves in
[`bp/moves.hpp`](../optimaru/bin_packing/cpp/include/bp/moves.hpp)):

```cpp
#include <random>
#include <vector>

#include "optimaru/search/operators.hpp"
#include "<ns>/domain.hpp"

namespace <ns> {

class MyMove final : public optimaru::MoveOperator<MyProblem> {
public:
    const char* name() const override { return "myMove"; }

    // Do ONE random move. Return false when no move is possible right now
    // (the step is then skipped).
    bool apply(optimaru::OperatorContext<MyProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        MyProblem& problem = ctx.editor.solution();  // read freely, never write
        // ... pick entity e and its new value v, using rng only ...
        previous_ = problem.entities[e].value;      // remember how to undo
        entity_ = e;
        ctx.editor.assign(e, v);                     // every write goes through the editor
        entities.push_back(e);                       // what the move touched (for tabu)
        return true;
    }

    // Exactly revert the last successful apply().
    void undo_last(optimaru::OperatorContext<MyProblem>& ctx) override {
        ctx.editor.assign(entity_, previous_);
    }

private:
    int entity_ = -1;
    int previous_ = -1;
};

}  // namespace <ns>
```

The contract:

- **Write only through `ctx.editor`.** Use `assign(entity, value)` for the
  assignment shape, and `remove(list, position)`, `insert(list, position,
  element)` and `reverse(list, first, last)` for the sequence shape. The editor
  keeps the incremental score and any derived state right. A direct write
  leaves the score stale.
- **Group a multi-write move.** Wrap it in `ctx.editor.begin_batch()` …
  `end_batch()`, as `EmptyBinMove` does, so the objective scores it as one change.
- **Undo exactly.** `undo_last()` is called at most once per `apply()`, before
  anything else, and must restore the previous state exactly.
- **Use only `rng` for randomness.** Seeded solves then stay reproducible.
- **Keep every entity or element assigned.** A custom move must not leave
  anything unassigned.
- **Check `ctx.stop_requested` in long moves.** A move that runs many
  evaluations (a descent, a repair chain) should check it every few hundred
  evaluations and return early when it answers true. It is a pointer to a
  function and may be null: `if (ctx.stop_requested && (*ctx.stop_requested)())`.
- **Optional: `outcome(MoveOutcome)`.** It tells the move how it fared, if the
  move adapts something of its own.

For the sequence shape, `ctx.element_list[e]` and `ctx.element_position[e]` tell
you where element `e` currently is (`-1` = unassigned), and `ctx.nearby` holds
the k-nearest-neighbour lists when the use case configures
`settings.search.nearby.distance`.

### 2. Register it

In `optimaru/<uc>/cpp/src/solver.cpp`, where the use case builds its
`optimaru::Solver<MyProblem> engine(...)`:

```cpp
if (config.myMoves)
    engine.add_move_operator(std::make_unique<MyMove>());
```

The engine chooses between the built-in mix and every registered operator
adaptively (a roulette wheel rewarded by each move's outcome), so you don't tune
a weight for it.

**Multi-start caveat.** Registered operators belong to one `optimaru::Solver`.
`engine.solve_multistart(...)` runs its parallel starts with the built-in moves
only, so a custom move would silently vanish from them. A use case with custom
moves runs its parallel starts itself, with one `optimaru::Solver` per worker,
each registering its own operators. `bin_packing/cpp/src/solver.cpp` shows the
pattern. If the use case you extend calls `solve_multistart` (knapsack does, for
example), switch it to that pattern before adding a move.

### 3. Expose a switch (recommended)

Add a field to the use case's `Solver::Config` (`myMoves = true`), bind it in
`optimaru/<uc>/cpp/bindings.cpp` (`.def_readwrite("my_moves", &Solver::Config::myMoves)`)
and declare it in `optimaru/<uc>/_core.pyi`. This lets you turn the move off for an
A/B comparison and keeps Python users in control. Bin packing's `empty_bin_moves` /
`empty_bin_candidates` are a complete example, including value checks in the
binding.

### 4. Test it

- **Score consistency.** Solve once with `ScoreChecks::Full`
  (`config.score_checks` in C++; `SolverConfig.full_assert = True` or
  `solve(..., full_assert=True)` in Python).
  Every incremental score is then compared with a full recompute, and the solve
  throws `ScoreMismatchError` if the move leaves the score stale. Add such a
  solve to `tests/<uc>/cpp/<ns>_test.cpp`.
- **Undo.** Apply and undo the move on a small instance and check that the
  solution and the score are back where they started.
- **Value.** Show that the move helps: the same seeds and budget with the move
  on and off, or an instance the built-in moves cannot escape. The use case's
  benchmark (`benchmarks/<slug>/`) is the place for a wider comparison.

### A ruin instead of a move

For large-neighbourhood search, implement `optimaru::RuinOperator<Solution>`.
It only chooses which entities or elements to remove
(`select_ruin_set(ctx, ruin_count, rng)`, without changing the solution). The
engine removes them, recreates them greedily and adapts the operator's weight.
Register it with `engine.add_ruin_operator(...)`. Employee scheduling's
`EmployeeHotRuinOperator` is an example.

### Construction instead of search

To change how the initial solution is built:

1. **Order.** Set `settings.construction.difficulty = [&](int entity) { ... }`.
   Entities are then placed hardest first (first fit decreasing) instead of in
   index order.
2. **A domain constructor.** Build a full or partial solution in the use case's
   `Solver` before calling the engine. The engine keeps what is assigned and
   fills in only the rest. Pickup-delivery's pair construction places each pickup
   together with its drop-off this way.

## Adding a cost

A constraint is one named component. Its level (hard / medium / soft), weight
and the shaping knobs (`exponent`, `offset`, `clip`) are defaults a caller may
override per solve, and a caller may pick it several times. See
[Constraints: names and knobs](use-cases/README.md#constraints-names-and-knobs).
The engine computes the raw cost of a repeated pick once and shapes it per
pick, so `cost()`, `delta_cost()` and `piecewise_cost()` must not read the
component's own knobs, must return the same double for the same input every
time, and a default factory must build the same components every time it is
called. Every reset checks a repeated pick's `cost()` against the first
pick's; run a new component's tests with a repeated pick under
`ScoreChecks::Full` to cover its deltas and previews as well. A constraint with domain parameters of its own per
pick is built by the domain's factory instead (employee scheduling).
The raw cost is positive for a penalty and `0` for no violation. A reward (a
quantity to maximize) has a raw cost `<= 0`.

The steps below add a vehicle-routing cost, following
[`vrp/constraints/latency_cost.hpp`](../optimaru/vehicle_routing/cpp/include/vrp/constraints/latency_cost.hpp).

### 1. Facts (only if the cost needs new input)

Add the field to `domain.hpp`, read it in `bindings.cpp`, and carry it through
`models.py` (`to_core()` / `from_core()`, with a field description). Domain
parameters belong in the problem model. Weights, seeds and time limits never do:
they come from the constraint picks and the solver config.

### 2. The component

Add `optimaru/<uc>/cpp/include/<ns>/constraints/<subject>_cost.hpp`. Most costs
are a **unit constraint**: a pure function of one unit and its members.

| Shape | Unit | Members |
| --- | --- | --- |
| sequence (routes) | one list, e.g. a vehicle's route | its elements, in order |
| assignment | one value, e.g. an employee or a bin | the entities assigned to it (order not meaningful) |

```cpp
#ifndef VRP_CONSTRAINTS_MY_RULE_COST_HPP
#define VRP_CONSTRAINTS_MY_RULE_COST_HPP

#include "vrp/constraints/common.hpp"

namespace vrp {

// Soft, weight 1: myRuleCost -- what one unit of raw cost means; when it is 0.
struct MyRuleCost final : detail::PerVehicleConstraint {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit MyRuleCost(double weight = kDefaultWeight)
        : PerVehicleConstraint("myRuleCost", weight, kDefaultLevel) {}

    double cost(const VehicleRoutePlan& p, int u, const std::vector<int>& m) const override {
        if (detail::isPool(p, u)) return 0;   // the pool of unassigned visits is not a vehicle
        return /* raw cost of route u, visits m */ 0;
    }
};

}  // namespace vrp

#endif
```

- **Naming.** The struct is `<Subject>Cost`. The registry name is the same with a
  lower-case first letter (`"myRuleCost"`), and Python derives
  `my_rule_cost` from it. Nobody maintains an alias table.
- **Defaults.** Declare them only as `kDefaultWeight` / `kDefaultLevel`. Python
  reads them through `constraint_info()`.
- **The base class.** Each use case's `constraints/common.hpp` has its own base
  for unit constraints (`detail::PerVehicleConstraint` here). In general it is an
  `optimaru::UnitConstraint<Solution>`
  ([`score/decomposed_objective.hpp`](../optimaru/cpp/optimaru/score/decomposed_objective.hpp)).
- **What `cost()` may read.** Only `(solution, unit, members)`: no derived
  values, because move previews evaluate candidate members before the move. To
  precompute lookup tables, override `on_reset(solution)`. For assignment-shape
  speed, implement `supports_delta()` / `delta_cost()`.
- **Global constraints.** A cost that belongs to no single unit (an
  unassigned count, a balance across units) is an
  `optimaru::GlobalConstraint<Solution>`. It keeps its own aggregates in
  `reset()` / `on_unit_changed()` (or the O(1) `on_member_changed()` when
  `supports_member_change()` is true), and `cost()` returns the total. All of
  knapsack's constraints are globals, for example
  [`ks/constraints/mandatory_items_cost.hpp`](../optimaru/knapsack/cpp/include/ks/constraints/mandatory_items_cost.hpp).
- **Rewards and structural constraints.** A reward sets `reward = true;` in its
  constructor (its raw cost must be `<= 0`, and it can never be hard). A
  structural constraint that must keep its level sets `level_flippable = false;`.
- **Hard costs** should be whole numbers: a hard contribution is rounded per unit.

### 3. Add it to the assembly

Include the header in `constraints.hpp` and `emplace` it in
`defaultConstraints()`, in catalog order (unit constraints before globals in
vehicle routing):

```cpp
assembly.emplace<MyRuleCost>();
```

Python can only pick a constraint that `defaultConstraints()` holds:
`constraint_info()`, `list_constraints()` and the pick resolution are all read
off this assembly. You then choose between two kinds of constraint:

- **Opt-in** (`kDefaultWeight = 0.0`). The constraint scores nothing until a
  caller gives it a weight, and `constraint_info()` reports it with
  `default_enabled=False`. Vehicle routing's `LatencyCost` and
  `RouteDurationCost` work this way. This is the right choice for most new
  rules, and step 4 doesn't apply.
- **On by default** (`kDefaultWeight > 0`). This changes every default solve and
  the published scores. Do step 4.

(Employee scheduling instead has a separate catalog of opt-in rules with
parameters, built by the rule factory in its `bindings.cpp`. Its guide describes
that variant.)

### 4. Default fast path (on-by-default costs only)

`solve()` without picks scores with the hand-written monolithic calculators,
`score_calculator.cpp` and `incremental_calculator.cpp`, not with the plugin
assembly. Add the same raw cost to both, multiplied by the component's
`kDefaultWeight` (read the constant; don't copy the number).
`<ns>_plugin_test` checks that the default assembly and the monolithic
calculators agree bit for bit, so it fails until both sides match.

### 5. The Python spec

In `optimaru/<uc>/constraints.py`:

```python
class MyRuleCost(Constraint):
    """What it measures. Default: soft, weight 1."""

    name = "my_rule_cost"
    label_en = "My rule"
    label_ko = "..."
    description_en = "What one unit of cost is; what 0 means."
    description_ko = "..."
```

Then add a module-level default instance (`my_rule_cost = MyRuleCost()`), put
that instance into `_DEFAULTS` at the same position as in `defaultConstraints()`,
and add both names to `__all__`. The spec declares no defaults: they come from
`constraint_info()`. Callers can now write
`solve_with_constraints(plan, get_constraints([..., MyRuleCost(weight=5, level="hard")]))`.

### 6. Tests

- **C++ (`tests/<uc>/cpp/<ns>_plugin_test.cpp`).** A hand-calculated positive case
  and a zero case. Incremental equals full recompute on random moves with the
  constraint picked. For an on-by-default cost, the monolith still agrees with
  the plugin assembly.
- **Python (`tests/<uc>/test_constraint_specs.py`).** Update the expected
  `list_constraints()` list and the `constraint_info()` rows. Add a case
  that picks, reweights and flips the level of the new constraint, and check
  that `constraints.score(constraint_costs(...), picks)` equals the solver's
  score.
- **Python reference scorer.** Six use cases carry an independent Python oracle
  (`tests/<uc>/reference_<uc>.py`). Add the cost there too, so the parity test
  covers it.

### 7. Docs and the CP-SAT comparison

- Add the constraint to the tables in `docs/use-cases/<slug>/README.md` and
  `python.md`. The tables must equal `constraint_info()`: name, C++ component,
  default level and weight, flippability, reward.
- Update the `_core.pyi` docstrings if they list constraints, and the use case's
  notebook in `examples/` if it shows the constraint list.
- The CP-SAT models the benchmarks compare against live in
  [external-solver](https://github.com/optimaru/external-solver). A new
  on-by-default cost needs its CP-SAT counterpart there, or the comparison in
  `benchmarks/<slug>/` no longer scores the same objective.

## Checklist

Adding a move:

- [ ] `MoveOperator` that writes only through `ctx.editor` and undoes exactly
- [ ] registered in `solver.cpp` (not lost to `solve_multistart`)
- [ ] `Solver::Config` switch, bound in `bindings.cpp`, declared in `_core.pyi`
- [ ] a `ScoreChecks::Full` solve in the C++ tests
- [ ] evidence that it helps, documented in the use case's guide

Adding a cost:

- [ ] `constraints/<subject>_cost.hpp` with `kDefaultWeight` / `kDefaultLevel`
- [ ] included and emplaced in `constraints.hpp`
- [ ] if on by default: the same cost in both monolithic calculators
- [ ] Python spec, default instance, `_DEFAULTS`, `__all__`
- [ ] C++ plugin test, Python spec tests, the reference scorer where there is one
- [ ] guide tables, stubs, notebook; the CP-SAT counterpart in external-solver
