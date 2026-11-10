# quadratic_assignment (Python)

The pybind11 package of the quadratic-assignment use case (`optimaru/quadratic_assignment/cpp`). Places one
facility on every location — `Facility.location` is the only decision variable,
and the solver keeps the facilities a permutation of the locations (swap moves
only).

Score, at the default weights:

- hard = -(fixed locations not holding their facility + facilities at a forbidden
  location + demand above location capacities) — `pinned_assignments_cost`,
  `forbidden_assignments_cost`, `location_capacity_cost`, hard x1 each, one raw cost
  per location
- soft = -(direct assignment cost + flow-distance cost) — `assignment_cost`,
  `flow_distance_cost`, soft x1 each, one raw total each

The problem carries no weights. Pick, re-weight or flip constraints with
`optimaru.quadratic_assignment.constraints`, and read the defaults with
`quadratic_assignment.constraint_info()`:

```python
from optimaru import quadratic_assignment as qap
from optimaru.quadratic_assignment.constraints import FlowDistanceCost, LocationCapacityCost, get_constraints, score

qap.constraint_info()
# [ConstraintInfo(name='pinned_assignments_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...,
#  ConstraintInfo(name='flow_distance_cost', default_level='soft', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True)]
qap.solve(problem, time_limit_seconds=1.0, constraints=[*get_constraints()[:-1], FlowDistanceCost(weight=2)])
raw = qap.constraint_costs(problem)   # {name: {"costs": [...], "weight": ..., ...}}
score(raw), score(raw, [LocationCapacityCost(exponent=2.0)])
# A constraint may be picked several times, each pick with its own knobs; offset / clip
# shift the shaping (clip=True: only the flow cost beyond 1000 counts for the second pick).
score(raw, [FlowDistanceCost(), FlowDistanceCost(weight=10, offset=1000)])
```

The side constraints report one raw cost per location (location order) and the
exponent (with `offset` / `clip`) shapes each location they evaluate (their
`"units"`: the fixed, the forbidden-row and the capacitated locations), per location,
not over the sum; the two objectives report one total each and the exponent shapes that
total. `clip=False` with an offset is refused on hard, where hard 0 must mean no
violation: `ValueError: constraint 'location_capacity_cost': clip=False with a non-zero
offset cannot be hard, because hard 0 must mean no violation; use clip=True (a threshold)
or another level`. On soft it is allowed.
`constraint_costs(problem, picks)` scores a pick list, the second pick of a name keyed
`<name>#2`. `level` is None (the default level), "hard" or "soft" (or `Level.HARD` /
`Level.SOFT`); the domain has no medium level, so "medium" raises `ValueError`, and a
repeat carries its own level. An objective whose data holds a negative entry (a negative
assignment cost, or a negative flow or distance) cannot be hard, because its total can be
negative and would cancel a broken pin: `ValueError: constraint 'assignment_cost' can have
a negative raw cost on this problem (a bonus) and cannot be hard`; its `constraint_costs()`
entry carries `"signed": True`. Hard contributions are rounded per location, which matters
only when a hard constraint's weighted, shaped per-location value is fractional: a
fractional hard weight, or an exponent != 1 on `location_capacity_cost` (costs `[2, 2]` at
exponent 0.5 give `-2`). An assignment that puts one facility on two locations or names an
unknown id is refused in `to_core()`, so also on a plain solve without `warm_start`. The
`SolverConfig` is the engine's (`spent_limit_seconds`, `step_count_limit`, ...); see the
[use-case README](README.md#python-api).

## Install and test

```bash
OPTIMARU_USE_CASES=quadratic_assignment uv sync   # builds only this use case's C++ extension
uv run pytest -q tests/quadratic_assignment
```

## Usage

```python
from optimaru import quadratic_assignment as qap
from optimaru.quadratic_assignment.models import QuadraticAssignmentProblemModel

problem = QuadraticAssignmentProblemModel.model_validate(data)    # the canonical JSON
qap.solve(problem, time_limit_seconds=5.0, random_seed=42)          # in place: assignment, total_cost, score
print(problem.score_hard, problem.score_soft, problem.assignment, problem.total_cost)

core = problem.to_core()                                            # the engine object
qap.solve(core, time_limit_seconds=5.0, random_seed=42)
print(core.score, core.assignment(), core.total_cost())
```

`core.score` is None until the core is scored (`qap.calculate_score(core)` or a solve);
`QuadraticAssignmentProblemModel.from_core(core)` then fills `score_hard` / `score_soft`.

For finer control build `qap.SolverConfig()` (time budget, `step_count_limit`,
multi-start, the acceptor and its sizes, the `on_new_best(step, score)` callback,
`best_score_limit` early stop, `full_assert`) and call `qap.Solver(cfg).solve(core)`
or `qap.solve(problem, config=cfg)`. A zero budget (`spent_limit_seconds = 0`)
returns the start unchanged: the identity, or a warm-start seed.

Note: `core.facilities[i]` returns a copy — write the assignment in bulk with
`core.set_assignment([...])` (per location: the facility index, -1 = empty), and
assign whole fact lists (`core.flows = [...]`), which re-validates on the next score.
