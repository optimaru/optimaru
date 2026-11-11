# Employee scheduling CP-SAT

CP-SAT backend for the constraint list of an `optimaru.employee_scheduling` request,
`external_solver.employee_scheduling` from
[external-solver](https://github.com/optimaru/external-solver); this project runs it
against the engine in this checkout. One boolean per
(shift, worker) plus one "unassigned" boolean per shift, then one plugin per constraint
under the constraint's own name and units. Hard cost is minimized before soft cost, and
the reported score always comes from the engine's own calculator.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run employee-scheduling-compare --seconds 10
uv run employee-scheduling-compare --instance demo --seconds 2 --json
```

`optimaru.employee_scheduling.constraint_costs(schedule, constraints)` works on an engine
schedule and a constraint list, while the runner hands over a request, so
`external_solver.employee_scheduling` provides the `constraint_costs(request)` the
runner calls. It builds
`optimaru.employee_scheduling.DecomposedCalculator` from the request's constraints and returns what
that calculator reports for the roster, which is the calculator the engine
searches with. The engine runs through `solve_request` with its default
settings and one start.

## Constraints

All 30 constraints of `optimaru.employee_scheduling.constraints` are modelled; external-solver's `tests/employee_scheduling/test_solve.py`
checks that list against the constraint union and `catalog()` against
`optimaru.employee_scheduling.constraint_info()` row for row. Each plugin subclasses its spec
(`class MaxConsecutiveDaysCost(domain.MaxConsecutiveDaysCost, RulePlugin)`) and
adds only `build()`: the default level, weight and reward flag are the engine's
`constraint_info()` rows (read by external-solver), and `level="soft"` demotes to soft
exactly as in the engine. A request may hold one constraint several times; the
k-th plugin of a name is `"<name>#<k>"`. On fixed rosters each constraint must match
the engine's calculator, including rosters with double-booked and unassigned
shifts.

| Constraints | Formulation | Against the engine |
| --- | --- | --- |
| `required_skill_cost`, `employee_unavailability_cost`, `undesired_day_cost`, `desired_day_cost`, `desired_shift_cost`, `assignment_cost` | a table per (shift, worker) | exact; money to 0.01 |
| `unassigned_shifts_cost` | the unassigned booleans; without this constraint every shift must be assigned, as in the engine | exact |
| `shift_overlap_cost`, `max_one_shift_per_day_cost`, `min_rest_cost` | pair counts over groups of shifts with the same times | exact |
| `max_consecutive_days_cost`, `min_consecutive_days_cost`, `max_consecutive_days_off_cost`, `rest_after_consecutive_work_cost`, `location_run_length_cost`, `rest_after_location_run_cost`, `location_run_gap_cost`, `day_pattern_cost` | one literal per matching window of days | exact |
| `max_weekly_minutes_cost`, `min_weekly_minutes_cost`, `min_weekly_rest_days_cost`, `target_days_off_cost`, `worker_day_count_in_range_cost`, `worker_location_count_cost`, `shift_coverage_cost` | slack variables on a linear count | exact |
| `shift_count_gap_cost`, `location_imbalance_cost` with `metric="gap"`, `worker_pairing_cost` | min and max variables; pair literals | exact |
| `weekly_minutes_gap_cost` | as above, in hours | to 0.01 per week |
| `shift_count_imbalance_cost`, `location_imbalance_cost` with `metric="ssd"` | squared counts over multiplication constraints | a stand-in for `sqrt(ssd)`, see below |

Worker and shift scopes (`worker_indices`, `shift_indices`) are modelled.

The engine scores balance as `sqrt(ssd)`, rounded to 6 significant figures. The
objective holds `ssd / 2`, the root's tangent at `ssd = 1` less a constant. Both
fall as loads even out, but `ssd / 2` weighs a large imbalance more heavily
against the other soft constraints. `shift_count_imbalance_cost` also drops
`sum(load)^2 / n`, a constant once every shift is assigned, so it holds
`sum(load^2) / 2`. With a soft `unassigned_shifts_cost` constraint that is not so, and
it keeps the term.

Over 30 s runs on `large`, CP-SAT missed hard 0 in three of four with an exact
fixed-point root, in five of six with `ssd / 2` and that term kept, and in one of
four as it stands. The model has 51,657 variables on `large`, and presolve takes
about 5.5 of the 30 seconds.

## Repeated constraints

A constraint may be listed several times, with the same parameters too:
`registry_for` makes one plugin per entry, the k-th repeat named `"<name>#<k>"`, and
each adds its own terms with its own knobs, as the engine adds one component per pick.

## Not modelled

These are refused with a `ValueError`, not approximated:

- `exponent` other than 1. It shapes a cost non-linearly, per unit.
- A fractional `weight`, as in every CP-SAT backend here, and a fractional `offset`.
- An `offset` on a constraint whose terms are not one per employee (runs, windows, pairs,
  weeks, the imbalance surrogates). The engine shapes each employee's cost (or a global
  constraint's total) on its own, which only the per-employee plugins reproduce:
  `required_skill_cost`, `employee_unavailability_cost`, `undesired_day_cost`,
  `desired_day_cost` (one term per evaluated employee, a worker scope's in-range ones,
  a 0 included) and `unassigned_shifts_cost` (its total). On the soft level the offset
  moves to the `SCALE` grid with the term (external-solver's `solve.selection`). Every other constraint refuses
  an offset with "offset is not supported by this CP-SAT model".
- `clip=False` with an offset on a hard constraint (its `level`, or its default level), for
  every plugin. The constraint schema refuses it already, and the objective refuses it again
  when it is built, with the engine's message (`constraint 'required_skill_cost':
  clip=False with a non-zero offset cannot be hard, ...`): hard 0 must mean no violation.
- `assignment_cost`, `shift_count_imbalance_cost`, `weekly_minutes_gap_cost` and
  `location_imbalance_cost` with `metric="ssd"` at the hard level, even if this
  request's values are whole. Only the soft level has a fixed-point grid, and the engine
  rounds a hard cost per unit.
- Pinned shifts. The engine exempts pinned-only runs and weeks from several constraints.
  That depends on whole runs and does not fit the window literals above.

`ShiftModel.worker` on an unpinned shift is a starting point for the engine, not
a constraint. This model does not read it.
