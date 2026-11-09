# Tournament Scheduling

Derived from the Timefold Quickstarts `tournament-scheduling` example (Apache-2.0), translated
and modified; see the repository [NOTICE](../../NOTICE).

Domain model, constraints and API details: [`python/`](python/README.md).

## Usage

```sh
cd use-cases/tournament-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import tournament_scheduling as ts
from tournament_scheduling.models import demo_data

schedule = demo_data()
result = ts.solve(schedule, time_limit_seconds=5.0, random_seed=42)
print(result.score_hard, result.score_soft)
```

`solve` takes `time_limit_seconds`, `random_seed`, `parallel_solver_count`, `stop_at_first_feasible`, `on_new_best`, a full `config` (`tournament_scheduling.SolverConfig`) and `constraints` (see below).

## Constraints

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `team_conflict_cost` | `TeamConflictCost` | hard | 1 | per team: pairs of its assignments on the same day (`C(c, 2)` per day) |
| `team_unavailability_cost` | `TeamUnavailabilityCost` | hard | 1 | per team: its unavailability facts whose day holds at least one of its assignments (per fact, not per assignment) |
| `assignment_count_imbalance_cost` | `AssignmentCountImbalanceCost` | medium | 1 | LoadBalance unfairness of the per-team assignment counts over the teams with at least one assignment |
| `confrontation_count_imbalance_cost` | `ConfrontationCountImbalanceCost` | soft | 1 | LoadBalance unfairness of the per-team-pair confrontation counts (same-day assignment pairs) over the pairs that meet |

The unfairness is `round6(sqrt(sum(load^2) - sum(load)^2 / n))` (6 significant figures,
HALF_EVEN) over the `n` items with a load of at least 1, so 0 means every loaded team (pair)
carries the same load. The first two are unit constraints (one cost per team, so `exponent`
shapes per team); the two imbalance constraints are global (one total, shaped as a whole).
The rows are in `list_constraints()` order, and `tournament_scheduling.constraint_info()`
reports exactly this table from the C++ components (`kDefaultLevel` / `kDefaultWeight`):
every constraint is flippable onto any of the three levels (`level="hard"`, `"medium"` or
`"soft"`, or a `Level`), none is a reward and all four are scored by default
(`default_enabled`). The medium level folds into the engine's soft level as `medium x 1e6`
(`MEDIUM_SCALE`, `kMediumFold` in `cpp/include/ts/domain.hpp`); a weight is in units of the
level in effect, so a medium weight is in medium units. Any other constraint may move onto
medium the same way (`ConfrontationCountImbalanceCost(level="medium")` charges one medium
unit per raw unit of head-to-head unfairness), and a repeated pick carries its own level.

The spec class `constraints.TeamUnavailabilityCost` does not share its name with the
`UnavailabilityPenalty` fact (`UnavailabilityPenaltyModel`, the `unavailability_penalties`
field). The medium constraint defaults to weight 1 in medium units, so `level="hard"`
charges 1 hard per raw unit of unfairness (the total, rounded), `level="soft"` demotes it
to soft at 1 per raw unit, and an explicit `weight` is in medium units.

Every constraint takes the knobs `weight`, `level`, `exponent`, `offset` and `clip`
(`offset` and `clip` keyword-only), and may be picked several times. A raw cost `x` (per team for the two unit constraints, on the
one total for the two imbalance constraints) is shaped as `sign(x) * max(0, |x| - offset)
** exponent` (`clip=True`, the default) or `(x - offset) ** exponent`, sign-preserving
(`clip=False`, which charges every unit the constraint evaluates, a raw 0 included, and acts
as a reward below the offset); `offset=0` is the plain power. `team_conflict_cost`
evaluates every team; `team_unavailability_cost` is scoped to the teams with an
unavailability fact (its `constraint_costs()` entry lists them as `"units"`), so its
literal offset charges those teams only. Both apply per team (or on an imbalance's total),
not to the total violation. `clip=False` with an offset is refused on hard (the two team
constraints by default), because hard 0 must mean no violation; medium folds into soft
here, so it takes the literal formula. Each pick is its own instance with its own knobs,
and the instances add up:

```python
ts.solve(schedule, constraints=[
    *get_constraints(),
    ConfrontationCountImbalanceCost(exponent=2, offset=2),   # + (head-to-head imbalance beyond 2)^2
])
```

`ts.constraint_costs(schedule.to_core(), picks)` (it takes the engine object, not the
pydantic model) keys the second pick
`confrontation_count_imbalance_cost#2`. CP-SAT takes integral offsets on the two hard
constraints (one term per evaluated team); its imbalance surrogates have no raw-identical
total to shift, so it refuses an offset on them (the engine takes it).

The C++ side builds and tests with:

```sh
cmake -S use-cases/tournament-scheduling/cpp -B build/tournament-scheduling && cmake --build build/tournament-scheduling -j
ctest --test-dir build/tournament-scheduling
```

## Starting from an existing solution

`with_initial_solution(problem, solution)` returns a copy of `problem` whose search starts from `solution`, which is the model a solve returned or that model as a JSON dict:

```python
solved = ts.solve(schedule, time_limit_seconds=5.0, random_seed=42)
seeded = ts.with_initial_solution(schedule, solved.model_dump(mode="json"))
result = ts.solve(seeded, time_limit_seconds=5.0, random_seed=43)
print(result.score_hard, result.score_soft)
```

Assignments are matched by id. An assignment the solution leaves out is placed by the construction heuristic, and a complete solution is exactly where the search starts. Pinned assignments keep their team on `problem`, whatever the solution says. `team` is a position in `teams`, so a solution that lists different teams, or the same ones in another order, raises `ValueError`. So do unknown ids and out-of-range values, before anything reaches the engine, which does not range-check.

## Extending

Engine contracts and code skeletons: [use-cases/README.md](../README.md#extending-a-use-case).
The solution has the **assignment** shape; the domain solver that configures the
engine is `cpp/include/ts/solver.hpp` + `cpp/src/solver.cpp`.

**Adding a move.** The engine already mixes change / swap moves
(weights in `Solver::Config`). `TeamCountMove` is defined and registered in `cpp/src/solver.cpp` — use it as the template. A new move is a
`optimaru::MoveOperator<…>` registered with `engine.add_move_operator(...)` next
to the engine construction in `cpp/src/solver.cpp`; a ruin for LNS is a
`RuinOperator` registered with `engine.add_ruin_operator(...)`.

**Changing the construction heuristic.** Entities are placed in index order (First Fit). Set
`engineConfig.construction.difficulty` for First Fit
Decreasing ordering, or pre-assign part of the solution in the domain
solver's `solve` before the engine runs — the engine only fills what is still unassigned.

**Adding a hard or soft cost.** One file per constraint under
`cpp/include/ts/constraints/` (currently `assignment_count_imbalance_cost`,
`confrontation_count_imbalance_cost`, `team_conflict_cost`, `team_unavailability_cost`, plus
`common.hpp` with the calculator aliases and the member-change global base):
1. add `constraints/<subject>_cost.hpp` with `struct <Subject>Cost` (a `UnitConstraint` or
   `GlobalConstraint`; positive raw cost = penalty, 0 = no violation) declaring
   `static constexpr double kDefaultWeight` and
   `static constexpr optimaru::Level kDefaultLevel`, an
   `explicit <Subject>Cost(double weight = kDefaultWeight)` constructor and the registry
   name `"<subject>Cost"` (the struct name with a lower-case first letter);
2. include it in `cpp/include/ts/constraints.hpp` and `emplace` it in
   `defaultConstraints()` if it belongs to the default set;
3. for a default constraint, add the same raw cost to
   `score_calculator` / `incremental_calculator`, weighted by the component's
   `kDefaultWeight` (`kHardWeight<...>` / `kMediumWeight<...>` / `kSoftWeight<...>`) —
   `ts_plugin_test` checks they agree bit for bit — and a positive and a zero
   hand-calculated case to `ts_test` / `ts_plugin_test`;
4. add a `Constraint` spec with the snake_case `name` and `label_en` / `label_ko` /
   `description_en` / `description_ko` to `python/tournament_scheduling/constraints.py` (plus
   its module-level default instance); callers then pick, reweight or flip it:
   `constraints=get_constraints([..., MyRuleCost(weight=5, level="hard")])`;
5. mirror it in the CP-SAT model in external-solver's
   [`external_solver.tournament_scheduling.constraints`](https://github.com/optimaru/external-solver/tree/main/src/external_solver/tournament_scheduling/constraints.py) (a plugin
   subclassing the spec; defaults come from `constraint_info()`) so the benchmark stays like
   for like, and state in [`benchmarks/tournament-scheduling/cpsat`](../../benchmarks/tournament-scheduling/cpsat) whether its CP-SAT term is raw-identical to the engine's.

## Benchmark

[`benchmarks/tournament-scheduling`](../../benchmarks/tournament-scheduling) compares this use case with OR-Tools CP-SAT on the same instances.
The CP-SAT model is raw-identical to the engine for the two hard constraints only; the two
imbalance constraints minimize an integral variance surrogate there (see
[its README](../../benchmarks/tournament-scheduling/cpsat/README.md#constraints-and-parity-with-the-engine)).
