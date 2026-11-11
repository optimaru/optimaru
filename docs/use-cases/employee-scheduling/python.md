# optimaru.employee_scheduling (Python)

You have a list of *shifts* (work slots: "Tuesday 09:00–17:00 at Location A,
needs Skill A") and a list of *workers* (each with skills, days they cannot work,
days they would rather not work, days they would like to work). This package
decides **who works which shift** without breaking the constraints, while keeping
everyone as happy as possible.

The search runs in C++: the domain wrapper in `optimaru/employee_scheduling/cpp` is built on the generic
engine in `optimaru/cpp/optimaru`, and pybind11 exposes it to Python. A
pydantic layer (`optimaru.employee_scheduling.models`) adds JSON-friendly requests and solutions.

```
 workers                  shifts                             the answer
 ───────                  ──────                             ──────────
 Worker 01 (Skill A)      Shift 1  Mon 09-17  needs Skill A     Shift 1 → Worker 01
 Worker 02 (Skill B)   +  Shift 2  Mon 09-12  needs Skill B  →  Shift 2 → Worker 02
 Worker 03 (Skill A)      Shift 3  Tue 09-17  needs Skill A     Shift 3 → Worker 03
```

## Install and first solve

```sh
OPTIMARU_USE_CASES=employee_scheduling uv sync      # builds only this use case's C++ extension
uv run pytest -q tests/employee_scheduling
```

```python
from optimaru import employee_scheduling as es

first = es.Employee("Worker 01", {"Skill A"})
second = es.Employee("Worker 02", {"Skill B"}, undesired_dates={1})  # would rather not work day 1

shifts = [
    es.Shift("Shift 1", es.minutes(0, 9), es.minutes(0, 17), required_skill="Skill A"),
    es.Shift("Shift 2", es.minutes(1, 9), es.minutes(1, 12), required_skill="Skill B"),
]

schedule = es.EmployeeSchedule([first, second], shifts)
es.solve(schedule, time_limit_seconds=1.0, random_seed=42)   # solves in place and returns the schedule

for shift, who in zip(schedule.shifts, schedule.assignment()):
    print(shift.id, "→", schedule.employees[who].name)   # Shift 1 → Worker 01, Shift 2 → Worker 02
print(schedule.score)                    # 0hard/-180soft; 0hard means no must-constraint is broken
```

`es.solve` also takes `parallel_solver_count`, `stop_at_first_feasible`,
`full_assert`, `on_new_best`, `constraints` (a list of constraint specs or a
`DecomposedCalculator`, see [The constraints](#the-constraints)) and a full `config`, an
`es.SolverConfig` (see [Solver options](#solver-options)) whose fields the keywords
override on a copy. `es.Solver(config).solve(schedule)` is the same solve one level
down.

Three things to know about time:

- A moment is an **integer: minutes since day 0, 00:00**. `es.minutes(day, hour,
  minute)` builds one, so `es.minutes(1, 9)` is day 1, 09:00. Times are never
  negative: day 0 is the first day of the problem, carried-in history included.
- A **date is a day number**: `unavailable_dates={2}` means "cannot work day 2".
- A shift has a `location`. Any constraint that talks about a location reads this
  string, so it can equally hold a site, a team or a shift type.

## Understanding the score

Every schedule gets one score with two numbers, written `Hhard/Ssoft`, such as
`-602hard/357.34soft`.

**Two levels, strictly ordered.** `hard` counts broken **must**-constraints, `soft` sums
the **nice-to-have** constraints. Scores compare hard first, with soft only as the
tiebreaker: `0hard/-50000soft` beats `-1hard/+10000soft`.

**Bigger is better, so violations are negative.** Each constraint measures a raw amount
`c` (shifts without the skill, overlap minutes, excess workdays) and contributes

```
contribution = −( weight × sign(c)·|c|^exponent )
```

to the hard or soft level. A penalty (`c > 0`) subtracts from the score. A reward
(`DesiredDayCost`, `DesiredShiftCost`) has a negative cost and adds to it, which is
why a good schedule can show a positive soft score. A reward is always soft:
`level="hard"` on it raises `ValueError`.

**Worked example.** `-602hard/357.34soft`, the fixed instance in
`tests/employee_scheduling/_reference.py`:

| constraint (hard) | raw cost | | constraint (soft) | raw cost |
|---|---|---|---|---|
| `required_skill_cost` | 1 | | `undesired_day_cost` (minutes) | 480 |
| `employee_unavailability_cost` (minutes) | 480 | | `desired_day_cost` (minutes, reward) | −839 |
| `shift_overlap_cost` (minutes) | 120 | | `shift_count_imbalance_cost` | 1.66 |
| `max_one_shift_per_day_cost` | 1 | | | |

hard = −(1 + 480 + 120 + 1) = **−602**; soft = −(480 − 839 + 1.66) = **+357.34**.
`calc.constraint_totals()` gives this table (weighted) for any schedule, and
`es.constraint_costs(schedule, constraints)` the raw per-employee costs.

**Feasible means hard is 0.** The solver first drives hard to 0, then spends the
rest of its budget raising soft. If hard ends below 0, the must-constraints cannot all
be met or the budget was too short; `constraint_totals()` shows which constraint is stuck.

- `hard` is always a whole number: each (employee, constraint) hard contribution is
  rounded, so fractional weights still give integer totals. `soft` may be
  fractional.
- `weight` scales a constraint linearly, `level` moves it between levels, and
  `exponent` bends it: with `exponent=2.0`, one employee with 4 violation units
  costs 16, while four employees with 1 unit each cost 4.
- Units differ per constraint (counts or minutes). Pick weights to convert them into one
  currency, such as `UndesiredDayCost(weight=1/60)` to count hours instead of minutes.

## The constraints

A schedule is judged by constraints; you choose which apply and how much each matters.
There is one class per constraint in `optimaru.employee_scheduling.constraints`, named `<Subject>Cost`;
its `name` is the snake_case registry name (`RequiredSkillCost.name ==
"required_skill_cost"`).

```python
from optimaru.employee_scheduling import (
    AssignmentCost, DecomposedCalculator, DesiredDayCost, EmployeeUnavailabilityCost,
    MaxConsecutiveDaysCost, MaxOneShiftPerDayCost, MaxWeeklyMinutesCost, MinRestCost,
    RequiredSkillCost, ShiftCountGapCost, ShiftOverlapCost, Solver, SolverConfig,
    UnassignedShiftsCost, UndesiredDayCost, make_random_roster,
)

constraints = [
    RequiredSkillCost(),                       # must: right skill for the shift
    EmployeeUnavailabilityCost(),              # must: never on unavailable days
    ShiftOverlapCost(),                        # must: no two shifts at once
    MaxOneShiftPerDayCost(),                   # must: one shift per day
    MinRestCost(min_gap_minutes=12 * 60),      # must: 12h rest between shifts
    UnassignedShiftsCost(),                    # must: every shift gets someone
    MaxConsecutiveDaysCost(value=6),           # must: at most 6 workdays in a row
    MaxWeeklyMinutesCost(minutes=40 * 60),     # must: at most 40h per week
    UndesiredDayCost(weight=2.0),              # nice: avoid disliked days (2x weight)
    DesiredDayCost(),                          # nice: grant wished days
    ShiftCountGapCost(tolerance=1),            # nice: busiest vs least-busy at most 1 apart
    AssignmentCost(cost_per_hour=10.0),        # nice: keep wage cost down
]

config = SolverConfig()
config.spent_limit_seconds = 1.0
schedule = make_random_roster(days=5, shifts_per_day=4, employees=6, seed=1)
calc = DecomposedCalculator(constraints)
Solver(config).solve(schedule, calc)       # search AND final score use these constraints
```

Every constraint takes the same five knobs, plus its own options:

| knob | meaning |
|---|---|
| `weight=None` | how much one unit of violation hurts; `None` is the constraint's default weight (1 for every constraint here), `UndesiredDayCost(weight=2.0)` hurts twice as much |
| `level=None` | `"hard"` (`Level.HARD`) makes it a must, `"soft"` (`Level.SOFT`) a nice-to-have, `None` keeps the constraint's default level; a reward refuses `"hard"`, and `"medium"` is refused (this use case has no medium level) |
| `exponent=1.0` | `2.0` squares each person's violation, so one person suffering a lot is much worse than many suffering a little |
| `offset=0.0` | (keyword) shifts that shaping per person (a global constraint: its total); `UndesiredDayCost(offset=120)` counts only the undesired minutes beyond two hours |
| `clip=True` | (keyword) `True` counts only what lies beyond the offset, `max(0, c - offset) ** exponent`; `False` shapes `c - offset` as it is, so everyone the constraint evaluates below the offset lowers the penalty (a worker-scoped constraint evaluates its workers only). With an offset, `False` is refused on hard (the constraint's `level`, or its default), because hard 0 must mean no violation |

The defaults are declared once, in the C++ components (`kDefaultWeight` /
`kDefaultLevel`); `es.constraint_info()` reports them, one
`ConstraintInfo(name, default_level, default_weight, level_flippable, reward,
default_enabled)` per constraint, the eight defaults first.

A constraint may be picked **several times**, with different options (two day patterns,
one cap per worker group), other knobs (`x**2 + x**3` is two picks, `exponent=2`
and `exponent=3`) or the same ones (identical picks simply count twice): each
instance is scored on its own, `constraint_totals()` tells them apart by `label`
(`"Day pattern[A,OFF,B]"`), and `constraint_costs()` keys the second instance
`"day_pattern_cost#2"`. An empty list is an error: `get_constraints()`,
`SchedulingInput.constraints`, `DecomposedCalculator(list)` and
`constraint_costs(schedule, list)` all refuse it. `DecomposedCalculator()` plus
`add(...)` builds a calculator incrementally (`add` takes the same knobs,
`offset` / `clip` as keywords).

Every constraint takes `offset` / `clip` (also in the JSON constraint models:
`{"name": "undesired_day_cost", "offset": 120, "clip": false}`); `offset=0` is the plain
power shaping. `clip=False` with an offset is refused on hard, in a spec list, in `add()`
and in a JSON constraint alike: `ValueError: constraint 'required_skill_cost': clip=False with a
non-zero offset cannot be hard, because hard 0 must mean no violation; use clip=True (a
threshold) or another level`. The level is `level`, in JSON too
(`"level": "hard" | "soft"`), and each repeated pick carries its own, so
`UndesiredDayCost()` plus `UndesiredDayCost(level="hard", offset=400)` makes the undesired
minutes beyond 400 a must as well.

**Scope.** Every per-employee constraint also takes `worker_indices` / `shift_indices`
(engine indices; `None` = everyone): `MaxConsecutiveDaysCost(value=4,
worker_indices=(0, 1))` applies to the first two employees only.
`ShiftCoverageCost` scopes natively (an empty list there means everyone); the
other global constraints take no scope.

### Constraint catalog

Default level and weight are what `constraint_info()` reports; the raw cost is
per employee (per schedule for the global constraints) and 0 means no violation.

**The default eight** (`get_constraints()`; at their default weights they
reproduce `calculate_score` exactly):

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `required_skill_cost` | `RequiredSkillCost` | hard | 1 | shifts given to someone without the needed skill (at most 64 distinct required skills) |
| `employee_unavailability_cost` | `EmployeeUnavailabilityCost` | hard | 1 | minutes worked on an unavailable day |
| `shift_overlap_cost` | `ShiftOverlapCost` | hard | 1 | minutes where two of an employee's shifts overlap |
| `max_one_shift_per_day_cost` | `MaxOneShiftPerDayCost` | hard | 1 | PAIRS of same-day shifts for one person |
| `min_rest_cost` | `MinRestCost` | hard | 1 | missing rest minutes below `min_gap_minutes` (600) between shifts |
| `undesired_day_cost` | `UndesiredDayCost` | soft | 1 | minutes worked on a disliked day (`per_day`: 1 per shift) |
| `desired_day_cost` | `DesiredDayCost` | soft (reward) | 1 | minus the minutes worked on a wished day (`per_day`: −1 per shift); raw ≤ 0 |
| `shift_count_imbalance_cost` | `ShiftCountImbalanceCost` | soft | 1 | sqrt of the squared deviations of the shift counts (global) |

**Opt-in constraints** (not in the default set; `default_enabled=False`):

| Constraint (Python name) | C++ component | Default level | Default weight | Raw cost (0 = no violation) |
|---|---|---|---|---|
| `desired_shift_cost` | `DesiredShiftCost` | soft (reward) | 1 | −1 per satisfied (day, location) wish in `Employee.desired_shifts`; raw ≤ 0 |
| `max_consecutive_days_cost` | `MaxConsecutiveDaysCost` | hard | 1 | workdays beyond `value` (6) in a row |
| `min_consecutive_days_cost` | `MinConsecutiveDaysCost` | soft | 1 | how far runs fall below `value` (2) |
| `max_consecutive_days_off_cost` | `MaxConsecutiveDaysOffCost` | soft | 1 | off days beyond `value` (3) in a run bounded by work on both sides |
| `rest_after_consecutive_work_cost` | `RestAfterConsecutiveWorkCost` | hard | 1 | rest-gap shortfall below `rest_days` (3) after a run of `work_days` (6)+ |
| `max_weekly_minutes_cost` | `MaxWeeklyMinutesCost` | hard | 1 | minutes beyond the weekly cap `minutes` (2400); weeks from `week_anchor` |
| `min_weekly_minutes_cost` | `MinWeeklyMinutesCost` | soft | 1 | minutes missing to the weekly guarantee `minutes` (480); an idle week costs the full amount |
| `min_weekly_rest_days_cost` | `MinWeeklyRestDaysCost` | hard | 1 | worked days beyond `7 - value` (1) per week |
| `assignment_cost` | `AssignmentCost` | soft | 1 | money: `shift_costs` override, else `cost_per_hour`, else `cost_per_shift` |
| `unassigned_shifts_cost` | `UnassignedShiftsCost` | hard | 1 | shifts nobody works (global) |
| `shift_count_gap_cost` | `ShiftCountGapCost` | soft | 1 | busiest minus least-busy shift count, beyond `tolerance` (global) |
| `weekly_minutes_gap_cost` | `WeeklyMinutesGapCost` | soft | 1 | per week, busiest minus least-busy minutes beyond `tolerance_minutes`, in HOURS (global) |
| `shift_coverage_cost` | `ShiftCoverageCost` | soft | 1 | per (day, location), assignees holding `worker_skill` outside [`min_count`, `max_count`] (global) |
| `location_run_length_cost` | `LocationRunLengthCost` | soft | 1 | runs of days at `location` outside [`min_run`, `max_run`] |
| `rest_after_location_run_cost` | `RestAfterLocationRunCost` | hard | 1 | worked days in the `rest_days` after a run at `location` |
| `location_run_gap_cost` | `LocationRunGapCost` | soft | 1 | shortfall below `min_gap` days between two runs at `location` |
| `worker_location_count_cost` | `WorkerLocationCountCost` | **soft** | 1 | per worker holding `worker_skill`, `location` shifts outside [`min_count`, `max_count`] |
| `day_pattern_cost` | `DayPatternCost` | soft | 1 | occurrences of an ordered pattern of days (see below) |
| `target_days_off_cost` | `TargetDaysOffCost` | soft | 1 | shortfall below `min_count` days off among `target_days` |
| `worker_day_count_in_range_cost` | `WorkerDayCountInRangeCost` | **soft** | 1 | distinct worked days in [`lo`, `hi`] outside [`min_count`, `max_count`] |
| `location_imbalance_cost` | `LocationImbalanceCost` | soft | 1 | fairness of a per-worker count, by `"ssd"` or `"gap"` (global) |
| `worker_pairing_cost` | `WorkerPairingCost` | soft | 1 | (day, location) buckets where a pair of workers fails to share (or, with `penalty`, shares) a shift; raw ≥ 0 (global) |

A `DayPatternCost.pattern` is comma-separated tokens: a location, `OFF` (a day with
no work), `*` (any day), `WORK` (any worked day) or `!x` (a worked day not at
location x). `"Location A,OFF,Location B"` costs 1 each time a day at Location A,
an off day and a day at Location B follow each other.

After solving, ask why the score is what it is:

```python
for total in calc.constraint_totals():
    print(total["name"], total["label"], total["cost"], total["level"])
```

The same constraints by name: `calc = DecomposedCalculator(); calc.add("day_pattern_cost",
pattern="Location A,OFF,Location B")` (weight, level, exponent, offset and clip as
keyword arguments, `weight=None` = the default; adding a name again adds another
instance). `es.list_constraints()` lists every name.

### Constraint models

- Every constraint is `<Subject>Cost` / `<subject>_cost` (Python name = C++ registry name in
  snake_case); a parametrized constraint's display name (`"Day pattern[...]"`) is the
  instance `label`.
- One class per constraint, the spec: the JSON models are generated from the specs
  (`models.CONSTRAINT_MODELS`, `models.constraint_model(...)`) and named `<SpecClass>Model`;
  `SchedulingInput.constraints` takes specs or JSON (`"level": "hard" | "soft"`, `weight`
  defaults to `null`, the constraint's default). Unknown names and keys are refused.
- `SchedulingInput.constraints` validates on assignment too
  (`model_input.constraints = [MaxConsecutiveDaysCost(value=5)]`), and a constraint model's
  `weight` / `exponent` refuse a bool or a numeric string, as the spec does.
- The per-employee specs declare the scope fields `worker_indices` /
  `shift_indices` before their domain parameters: build them with keyword
  arguments (`MaxConsecutiveDaysCost(value=5)`, not
  `MaxConsecutiveDaysCost(None, None, 1.0, 5)`).
- The scope keys `worker_indices` / `shift_indices` are accepted only on the
  per-employee constraints and on `shift_coverage_cost`. The other global constraints
  (`unassigned_shifts_cost`, `shift_count_imbalance_cost`, `shift_count_gap_cost`,
  `weekly_minutes_gap_cost`, `location_imbalance_cost`, `worker_pairing_cost`)
  refuse them, even as `null`.
- `weight=None` everywhere (also `DecomposedCalculator.add`); the defaults come
  from C++ (`constraint_info()`). The two rewards refuse `level="hard"`.
- `constraint_costs(schedule, constraints)` and `constraint_info()` report the raw
  costs and the catalog; `constraint_totals()` reports
  `{name, label, cost, level, is_hard}`.

## Pinning

`ShiftModel(pinned=True, worker="Worker 01")` fixes an assignment: the shift is
scored like any other but never moved. Use it to carry in the last days of the
previous planning period, so week-spanning constraints see across the boundary, or for
assignments that are already settled. A pinned shift requires `worker`. Number the
days so the carried-in ones come first: day 0 is the first day of history.

- **Per-shift constraints** (required skill, availability, one shift per day, rest
  between shifts): a pinned shift is settled history and is not re-judged.
- **Pattern constraints** (consecutive days, location runs, weekly rest, horizon counts):
  pinned days are context. A violation lying entirely within pinned days is not
  penalized; one that involves a movable day is, with the pinned days counted.
  Horizon-count constraints exclude pinned days.
- **The end of the horizon**: a run reaching the last day has an unknown future, so
  a shortfall constraint waives it while an excess constraint still counts it.

## JSON in, JSON out

```python
from optimaru.employee_scheduling.constraints import MaxConsecutiveDaysCost
from optimaru.employee_scheduling.models import (
    ScheduleRequest, SchedulingInput, ShiftModel, WorkerModel, default_constraints, solve_request,
)

request = ScheduleRequest(
    time_limit_seconds=1,
    model_input=SchedulingInput(
        workers=[WorkerModel(id="Worker 01", skills=["Skill A"])],
        shifts=[ShiftModel(id="Shift 1", start=540, end=1020, required_skill="Skill A")],
        constraints=[*default_constraints(), MaxConsecutiveDaysCost(value=6, weight=2)],
    ),
)
solution = solve_request(request)
print(solution.schedule)                 # {"Worker 01": [ShiftAssignmentModel(...)]}
print(solution.model_dump_json())        # ready to send over the wire
```

`ScheduleRequest.model_validate_json(...)` accepts the same JSON back: the constraint
list is a discriminated union generated from the specs, so
`{"name": "max_consecutive_days_cost", "value": 6, "level": "soft"}` picks the right
constraint model, and `constraint.to_spec()` is the `MaxConsecutiveDaysCost` it states.
`solution.constraint_totals` explains the score per constraint instance (`name`, `label`,
`cost`, `level`).

`optimaru.employee_scheduling.demo.generate_demo_data(seed=7, workers=12, days=14)` builds a
made-up request with skills, unavailable and preferred days, and constraints that solve
to hard 0.

## Starting from an earlier solution

`with_initial_solution(problem, solution)` returns a new request whose solve
starts from `solution`, which is what `solve_request` returned: a
`ScheduleSolution` or the plain dict of `model_dump(mode="json")`. `problem` is
not changed.

```python
from optimaru.employee_scheduling import with_initial_solution
from optimaru.employee_scheduling.demo import generate_demo_data
from optimaru.employee_scheduling.models import solve_request

request = generate_demo_data()
request.time_limit_seconds = 1.0
first = solve_request(request)

# Keep Worker 01's shifts and let the solve place everything else again.
partial = first.model_dump(mode="json")
partial["schedule"] = {"Worker 01": partial["schedule"]["Worker 01"]}
partial["unassigned"] = []
second = solve_request(with_initial_solution(request, partial))
print(second.score_hard, second.score_soft)
```

- Workers and shifts are matched by id, never by position. An assignment is read
  for its `shift_id` only.
- The assignments reach the solver through `ShiftModel.worker`. A shift that has
  one is left alone by construction; a partial solution is therefore fine, and
  only the shifts it leaves out, or lists as `unassigned`, are constructed. Any
  earlier `worker` on such a shift is cleared.
- Local search may still move a seeded shift. `time_limit_seconds=0` returns a complete seed
  unchanged, and `pinned` fixes a shift for good. A pinned shift keeps its worker
  whatever the solution says about it.
- `ValueError`, naming the worker or shift, for an unknown id, a shift assigned
  twice, or both assigned and unassigned, a malformed assignment, or a pinned shift
  the solution moves or unassigns.

A seed that breaks a constraint is not an error: it is a starting point, and the score
says what it costs.

## Solver options

```python
from optimaru import employee_scheduling as es

config = es.SolverConfig()
config.spent_limit_seconds = 5.0        # how long it may think (default 30)
config.step_count_limit = 100_000       # 0 disables step-count termination
config.no_improvement_limit_seconds = 0.5   # stop after no new best
config.no_improvement_step_count_limit = 50_000
config.random_seed = 42                 # same seed, same search
config.parallel_solver_count = 1        # independent parallel starts (seed + i each); ignored with a calculator
config.full_assert = False              # re-verify every incremental score (a debugging aid; much slower)
config.guided_moves = True
config.employee_greedy_construction = True
config.late_acceptance_size = 400
config.ruin_recreate = True
config.ruin_recreate_unimproved_step_trigger = 2_000
config.descent_weight = 0.02            # best-improvement descent over a shift pair
config.descent_max_sweeps = 4
config.ruin_recreate_as_move_weight = 0.0    # ruin+repair as an ordinary move
config.ruin_recreate_as_move_max_ruin = 12
config.change_move_weight = 0.8         # assignment-shape move mix; weights are relative
config.swap_move_weight = 0.2
config.smart_change_moves = False       # sample several employees for one shift
config.smart_change_candidate_count = 6
config.employee_day_repair_moves = True # move duplicate same-day assignments to off employees
config.employee_run_repair_moves = True # break overlong consecutive-work runs
config.employee_run_repair_max_days = 3
config.employee_window_moves = True     # swap two employees' assignments over days
config.employee_window_max_days = 3
config.custom_move_probability = 0.30   # cap the adaptive custom-move pool
config.nearby_shifts = True             # temporal nearby swap/descent/ruin bias
config.nearby_shifts_probability = 0.65 # 0.8 pays on large-only (1000+ shift) rosters
config.employee_ruin_moves = True       # LNS targets hot employee/day regions
config.best_score_limit = (0, float("-inf"))    # stop at the first feasible schedule
config.on_new_best = lambda step, score: print(step, score)   # progress callback
solver = es.Solver(config)
```

`guided_moves=False` also disables the employee-aware moves and falls back to the
generic assignment-shape change/swap search. The `relocate/list_swap/two_opt/or_opt/tail_swap`
weights are generic sequence-shaped options and do not affect this domain.

`es.calculate_score(schedule)` scores a fixed assignment without solving (the
default eight at their default weights).

## Adding a constraint

Every constraint ships its own incremental evaluation: when the solver moves one shift
between two employees, only the change is recomputed
(`UnitConstraint::delta_cost`), and constraints precompute lookup tables once per solve
(`UnitConstraint::on_reset`). See the use case's
[README](README.md#extending) for the steps. Weights, level changes and
exponents need no code.

## Files

```
optimaru/employee_scheduling/
├── __init__.py       re-exports core + constraints + helpers
├── _core.pyi         type stub with full API docs
├── constraints.py    constraint spec classes, one per constraint (declarative assembly)
├── models.py         pydantic request/response layer, constraint models generated from the specs
├── demo.py           generate_demo_data, a made-up sample request
├── helpers.py        pure-Python conveniences (list_constraints, make_random_roster, ...)
└── cpp/              bindings.cpp (the _core module), optimaru/employee_scheduling/cpp/include/es/, src/
tests/employee_scheduling/   correctness, raw costs by hand, catalog + model tests (pytest)
```
