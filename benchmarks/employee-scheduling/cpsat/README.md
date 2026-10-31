# Employee scheduling CP-SAT

CP-SAT backend for the rule list of an `es_scheduling` request. One boolean per
(shift, worker) plus one "unassigned" boolean per shift, then one plugin per rule
under the rule's own name and units. Hard cost is minimized before soft cost, and
the reported score always comes from the engine's own calculator.

## Engine comparison

The comparison runner is owned by this use case. It builds deterministic
instances, gives both backends the same solve budget, and re-scores both answers
with the engine evaluator.

```sh
uv run employee-scheduling-compare --seconds 10
uv run employee-scheduling-compare --instance demo --seconds 2 --json
```

`es_scheduling` has no `constraint_costs` function, so this package provides the
one the runner calls. It builds `es_scheduling.DecomposedCalculator` from the
request's rules and returns what that calculator reports for the roster, which
is the calculator the engine searches with. The engine runs through
`solve_request` with its default settings and one start.

## Rules

All 30 rule types of `es_scheduling.models` are modelled; `tests/test_solve.py`
checks that list against the rule union. On fixed rosters each rule must match
the engine's calculator, including rosters with double-booked and unassigned
shifts.

| Rules | Formulation | Against the engine |
| --- | --- | --- |
| `required_skill`, `unavailable_employee`, `undesired_day`, `desired_day`, `desired_shift`, `assignment_cost` | a table per (shift, worker) | exact; money to 0.01 |
| `unassigned_shift` | the unassigned booleans; without this rule every shift must be assigned, as in the engine | exact |
| `overlapping_shift`, `one_shift_per_day`, `min_rest` | pair counts over groups of shifts with the same times | exact |
| `max_consecutive_days`, `min_consecutive_days`, `max_consecutive_off`, `rest_after_consecutive_work`, `location_run`, `rest_after_location_run`, `location_run_gap`, `day_pattern` | one literal per matching window of days | exact |
| `max_weekly_minutes`, `min_weekly_minutes`, `min_weekly_rest_days`, `target_days_off`, `worker_day_count_in_range`, `worker_location_count`, `shift_coverage` | slack variables on a linear count | exact |
| `balance_min_max_gap`, `balance_location` with `metric="gap"`, `worker_pair_same_shift` | min and max variables; pair literals | exact |
| `balance_weekly_minutes_gap` | as above, in hours | to 0.01 per week |
| `balance_assignments`, `balance_location` with `metric="ssd"` | a fixed-point `sqrt(ssd)` over multiplication constraints | to 0.01, and the engine's rounding to 6 significant figures is not modelled |

Worker and shift scopes (`worker_indices`, `shift_indices`) are modelled. A
fractional rule moved to the hard level rounds up here, where the engine rounds
to nearest. None of the benchmark instances does this.

## Not modelled

These are refused with a `ValueError`, not approximated:

- `exponent` other than 1. It shapes a cost non-linearly, per unit.
- A fractional `weight`, as in every CP-SAT backend here.
- Pinned shifts. The engine exempts pinned-only runs and weeks from several rules.
  That depends on whole runs and does not fit the window literals above.

`ShiftModel.worker` on an unpinned shift is a starting point for the engine, not
a rule. This model does not read it.
