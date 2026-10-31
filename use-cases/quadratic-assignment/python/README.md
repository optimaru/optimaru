# quadratic_assignment (Python)

The pybind11 package of the quadratic-assignment use case (`../cpp`). Places one
facility on every location — `Facility.location` is the only planning variable,
and the solver keeps the facilities a permutation of the locations (swap moves
only).

Score (HardSoftScore), at the default weights:

- hard = -(fixed locations not holding their facility + facilities at a forbidden
  location + demand above location capacities) — `pinned_assignments_cost`,
  `forbidden_assignments_cost`, `location_capacity_cost`, hard x1 each, one raw cost
  per location
- soft = -(direct assignment cost + flow-distance cost) — `assignment_cost`,
  `flow_distance_cost`, soft x1 each, one raw total each

The problem carries no weights. Pick, re-weight or flip constraints with
`quadratic_assignment.constraints`, and read the defaults with
`quadratic_assignment.constraint_info()`:

```python
import quadratic_assignment as qap
from quadratic_assignment.constraints import FlowDistanceCost, LocationCapacityCost, get_constraints, score

qap.constraint_info()
# [ConstraintInfo(name='pinned_assignments_cost', default_level='hard', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True), ...,
#  ConstraintInfo(name='flow_distance_cost', default_level='soft', default_weight=1.0,
#                 level_flippable=True, reward=False, default_enabled=True)]
qap.solve(problem, constraints=[*get_constraints()[:-1], FlowDistanceCost(weight=2)])
raw = qap.constraint_costs(problem)   # {name: {"costs": [...], "weight": ..., ...}}
score(raw), score(raw, [LocationCapacityCost(exponent=2.0)])
```

The side constraints report one raw cost per location (location order) and the
exponent shapes each one; the two objectives report one total each and the
exponent shapes that total — the same terms 0.1.x shaped, so exponents score as
before. 0.2.0 renamed every constraint (no aliases: `fixed_assignments` ->
`pinned_assignments_cost`, `forbidden_assignments` -> `forbidden_assignments_cost`,
`location_capacities` -> `location_capacity_cost`, `minimize_assignment_cost` ->
`assignment_cost`, `minimize_flow_distance` -> `flow_distance_cost`), replaced the
flat `{name: [terms]}` of `constraint_costs()` by the standard dict, rounds hard per
location, refuses an assignment that puts one facility on two locations, and replaced
the simulated-annealing `SolverConfig` knobs (`max_iterations`, `stagnation_limit`,
`temperature`, `cooling_rate`) by the engine's (`spent_limit_seconds`,
`step_count_limit`, ...); see the [use-case README](../README.md#020).

## Install / test

```bash
uv sync --all-groups --all-extras
uv run pytest -q
```

## Usage

```python
import quadratic_assignment as qap
from quadratic_assignment.models import QuadraticAssignmentProblemModel

problem = QuadraticAssignmentProblemModel.model_validate(data)    # the canonical JSON
qap.solve(problem, time_limit_seconds=5.0, random_seed=42)          # in place: assignment, total_cost, score
print(problem.score_hard, problem.score_soft, problem.assignment, problem.total_cost)

core = problem.to_core()                                            # the engine object
qap.solve(core, time_limit_seconds=5.0, random_seed=42)
print(core.score, core.assignment(), core.total_cost())
```

For finer control build `qap.SolverConfig()` (time budget, `step_count_limit`,
multi-start, the acceptor and its sizes, the `on_new_best(step, score)` callback,
`best_score_limit` early stop, `full_assert`) and call `qap.Solver(cfg).solve(core)`
or `qap.solve(problem, config=cfg)`. A zero budget (`spent_limit_seconds = 0`)
returns the start unchanged: the identity, or a warm-start seed.

Note: `core.facilities[i]` returns a copy — write the assignment in bulk with
`core.set_assignment([...])` (per location: the facility index, -1 = empty), and
assign whole fact lists (`core.flows = [...]`), which re-validates on the next score.
