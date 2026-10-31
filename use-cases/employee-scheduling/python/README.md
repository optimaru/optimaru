# es_scheduling — employee shift scheduling

You have a list of *shifts* (work slots: "Tuesday 09:00–17:00 at Location A,
needs Skill A") and a list of *workers* (each with skills, days they cannot work,
days they would rather not work, days they would like to work). This package
decides **who works which shift** without breaking the rules, while keeping
everyone as happy as possible.

The search runs in C++: the domain wrapper in `../cpp` is built on the generic
engine in `../../../include/optimaru`, and pybind11 exposes it to Python. A
pydantic layer (`es_scheduling.models`) adds JSON-friendly requests and solutions.

```
 workers                  shifts                             the answer
 ───────                  ──────                             ──────────
 Worker 01 (Skill A)      Shift 1  Mon 09-17  needs Skill A     Shift 1 → Worker 01
 Worker 02 (Skill B)   +  Shift 2  Mon 09-12  needs Skill B  →  Shift 2 → Worker 02
 Worker 03 (Skill A)      Shift 3  Tue 09-17  needs Skill A     Shift 3 → Worker 03
```

## Install and first solve

```sh
cd use-cases/employee-scheduling/python
uv sync --all-groups --all-extras      # builds the C++ extension
uv run pytest -q
```

```python
import es_scheduling as es

first = es.Employee("Worker 01", {"Skill A"})
second = es.Employee("Worker 02", {"Skill B"}, undesired_dates={1})  # would rather not work day 1

shifts = [
    es.Shift("Shift 1", es.minutes(0, 9), es.minutes(0, 17), required_skill="Skill A"),
    es.Shift("Shift 2", es.minutes(1, 9), es.minutes(1, 12), required_skill="Skill B"),
]

config = es.SolverConfig()
config.spent_limit_seconds = 1.0
schedule = es.EmployeeSchedule([first, second], shifts)
es.Solver(config).solve(schedule)        # solves in place

for shift, who in zip(schedule.shifts, schedule.assignment()):
    print(shift.id, "→", schedule.employees[who].name)
print(schedule.score)                    # 0hard means no must-rule is broken
```

Three things to know about time:

- A moment is an **integer: minutes since day 0, 00:00**. `es.minutes(day, hour,
  minute)` builds one, so `es.minutes(1, 9)` is day 1, 09:00. Times are never
  negative: day 0 is the first day of the problem, carried-in history included.
- A **date is a day number**: `unavailable_dates={2}` means "cannot work day 2".
- A shift has a `location`. Any rule that talks about a location reads this
  string, so it can equally hold a site, a team or a shift type.

## Understanding the score

Every schedule gets one score with two numbers, written `Hhard/Ssoft`, such as
`-602hard/357.34soft`.

**Two levels, strictly ordered.** `hard` counts broken **must**-rules, `soft` sums
the **nice-to-have** rules. Scores compare hard first, with soft only as the
tiebreaker: `0hard/-50000soft` beats `-1hard/+10000soft`.

**Bigger is better, so violations are negative.** Each rule measures a raw amount
`c` (shifts without the skill, overlap minutes, excess workdays) and contributes

```
contribution = −( weight × sign(c)·|c|^exponent )
```

to the hard or soft level. A penalty (`c > 0`) subtracts from the score. A reward
(`DesiredDay`, `DesiredShift`) has a negative cost and adds to it, which is why a
good schedule can show a positive soft score.

**Worked example.** `-602hard/357.34soft`, the fixed instance in
`tests/_reference.py`:

| rule (hard) | raw cost | | rule (soft) | raw cost |
|---|---|---|---|---|
| missing skill | 1 | | undesired-day minutes | 480 |
| unavailable-day minutes | 480 | | desired-day minutes (reward) | −839 |
| overlap minutes | 120 | | balance unfairness | 1.66 |
| same-day pair | 1 | | | |

hard = −(1 + 480 + 120 + 1) = **−602**; soft = −(480 − 839 + 1.66) = **+357.34**.
`calc.constraint_totals()` gives this table for any schedule.

**Feasible means hard is 0.** The solver first drives hard to 0, then spends the
rest of its budget raising soft. If hard ends below 0, the must-rules cannot all
be met or the budget was too short; `constraint_totals()` shows which rule is stuck.

- `hard` is always a whole number: each (employee, rule) hard contribution is
  rounded, so fractional weights still give integer totals. `soft` may be
  fractional.
- `weight` scales a rule linearly, `hard=True/False` moves it between levels, and
  `exponent` bends it: with `exponent=2.0`, one employee with 4 violation units
  costs 16, while four employees with 1 unit each cost 4.
- Units differ per rule (counts or minutes). Pick weights to convert them into one
  currency, such as `UndesiredDay(1/60)` to count hours instead of minutes.

## The rules

A schedule is judged by rules; you choose which apply and how much each matters.

```python
from es_scheduling import (
    AssignmentCost, BalanceMinMaxGap, DecomposedCalculator, DesiredDay, MaxConsecutiveDays,
    MaxOneShiftPerDay, MaxWeeklyMinutes, MinimumRest, OverlappingShift, RequiredSkill,
    Solver, SolverConfig, UnassignedShift, UnavailableEmployee, UndesiredDay, make_random_roster,
)

rules = [
    RequiredSkill(),                       # must: right skill for the shift
    UnavailableEmployee(),                 # must: never on unavailable days
    OverlappingShift(),                    # must: no two shifts at once
    MaxOneShiftPerDay(),                   # must: one shift per day
    MinimumRest(min_gap_minutes=12 * 60),  # must: 12h rest between shifts
    UnassignedShift(),                     # must: every shift gets someone
    MaxConsecutiveDays(value=6),           # must: at most 6 workdays in a row
    MaxWeeklyMinutes(minutes=40 * 60),     # must: at most 40h per week
    UndesiredDay(2.0),                     # nice: avoid disliked days (2x weight)
    DesiredDay(),                          # nice: grant wished days
    BalanceMinMaxGap(tolerance=1),         # nice: busiest vs least-busy at most 1 apart
    AssignmentCost(cost_per_hour=10.0),    # nice: keep wage cost down
]

config = SolverConfig()
config.spent_limit_seconds = 1.0
schedule = make_random_roster(days=5, shifts_per_day=4, employees=6, seed=1)
calc = DecomposedCalculator(rules)
Solver(config).solve(schedule, calc)       # search AND final score use these rules
```

Every rule takes the same three dials, plus its own options:

| dial | meaning |
|---|---|
| `weight=1.0` | how much one violation hurts; `UndesiredDay(2.0)` hurts twice as much |
| `hard=None` | `True` makes it a must (hard), `False` a nice-to-have (soft), `None` keeps the rule's default |
| `exponent=1.0` | `2.0` squares each person's violation, so one person suffering a lot is much worse than many suffering a little |

### Rule catalog

**The default eight** (`get_constraints()`; at weight 1 they reproduce
`calculate_score` exactly):

| rule | default | counts what |
|---|---|---|
| `RequiredSkill()` | hard | shifts given to someone without the needed skill (at most 64 distinct required skills) |
| `UnavailableEmployee()` | hard | minutes worked on an unavailable day |
| `OverlappingShift()` | hard | minutes where two of your shifts overlap |
| `MaxOneShiftPerDay()` | hard | pairs of same-day shifts for one person |
| `MinimumRest(min_gap_minutes=600)` | hard | missing rest minutes between shifts |
| `UndesiredDay()` | soft | minutes worked on a disliked day |
| `DesiredDay()` | soft | minutes worked on a wished day (a reward) |
| `BalanceShiftAssignments()` | soft | statistical unfairness of shift counts |

**Opt-in specs** (`es_scheduling.constraints`, usable from Python and C++):

| rule | default | counts what |
|---|---|---|
| `DesiredShift()` | soft | satisfied (day, location) wishes in `Employee.desired_shifts` (a reward) |
| `MaxConsecutiveDays(value=6)` | hard | workdays beyond `value` in a row |
| `MinConsecutiveDays(value=2)` | soft | how far short runs fall below `value` |
| `MaxConsecutiveOff(value=3)` | soft | off days beyond `value` in a run bounded by work on both sides |
| `RestAfterConsecutiveWork(work_days=6, rest_days=3)` | hard | rest-gap shortfall after a run of `work_days`+ worked days |
| `MaxWeeklyMinutes(minutes=2400)` | hard | minutes beyond the weekly cap |
| `MinWeeklyMinutes(minutes=480)` | soft | minutes missing to the weekly guarantee (an idle week costs the full amount) |
| `MinWeeklyRestDays(value=1)` | hard | worked days beyond `7 - value` per week |
| `AssignmentCost(cost_per_hour=…, cost_per_shift=…, shift_costs={…})` | soft | money: per-cell override, else per-hour, else per-shift |
| `UnassignedShift()` | hard | shifts nobody works |
| `BalanceMinMaxGap(tolerance=0)` | soft | busiest minus least-busy shift count, beyond tolerance |
| `BalanceWeeklyMinutesGap(tolerance_minutes=120)` | soft | per week, busiest minus least-busy minutes, beyond tolerance, in hours |

**Rule models** (`es_scheduling.models`; also reachable through
`DecomposedCalculator.add(name, ...)`):

| rule | default | counts what |
|---|---|---|
| `ShiftCoverageRule` | soft | per (day, location) group, assignees holding `worker_skill` outside [`min_count`, `max_count`] |
| `LocationRunRule` | soft | runs of consecutive days at `location` outside [`min_run`, `max_run`] |
| `RestAfterLocationRunRule` | hard | worked days in the `rest_days` after a run at `location` |
| `LocationRunGapRule` | soft | shortfall below `min_gap` days between two runs at `location` |
| `WorkerLocationCountRule` | hard | per worker holding `worker_skill`, `location` shifts outside [`min_count`, `max_count`] |
| `DayPatternRule` | soft | occurrences of an ordered pattern of days (see below) |
| `TargetDaysOffRule` | soft | shortfall below `min_count` days off among `target_days` |
| `WorkerDayCountInRuleRange` | hard | distinct worked days in [`lo`, `hi`] outside [`min_count`, `max_count`] |
| `BalanceLocationRule` | soft | fairness of a per-worker count, by `"ssd"` or `"gap"` |
| `WorkerPairSameShiftRule` | soft | (day, location) buckets a pair of workers should share or avoid sharing |

A `DayPatternRule.pattern` is comma-separated tokens: a location, `OFF` (a day with
no work), `*` (any day), `WORK` (any worked day) or `!x` (a worked day not at
location x). `"Location A,OFF,Location B"` costs 1 each time a day at Location A,
an off day and a day at Location B follow each other.

After solving, ask why the score is what it is:

```python
for total in calc.constraint_totals():
    print(total["name"], total["cost"], "hard" if total["is_hard"] else "soft")
```

## Pinning

`ShiftModel(pinned=True, worker="Worker 01")` fixes an assignment: the shift is
scored like any other but never moved. Use it to carry in the last days of the
previous planning period, so week-spanning rules see across the boundary, or for
assignments that are already settled. A pinned shift requires `worker`. Number the
days so the carried-in ones come first: day 0 is the first day of history.

- **Per-shift rules** (required skill, availability, one shift per day, rest
  between shifts): a pinned shift is settled history and is not re-judged.
- **Pattern rules** (consecutive days, location runs, weekly rest, horizon counts):
  pinned days are context. A violation lying entirely within pinned days is not
  penalized; one that involves a movable day is, with the pinned days counted.
  Horizon-count rules exclude pinned days.
- **The end of the horizon**: a run reaching the last day has an unknown future, so
  a shortfall rule waives it while an excess rule still counts it.

## JSON in, JSON out

```python
from es_scheduling.models import (
    MaxConsecutiveDaysRule, ScheduleRequest, SchedulingInput, ShiftModel, WorkerModel,
    default_rules, solve_request,
)

request = ScheduleRequest(
    timelimit=1,
    model_input=SchedulingInput(
        workers=[WorkerModel(id="Worker 01", skills=["Skill A"])],
        shifts=[ShiftModel(id="Shift 1", start=540, end=1020, required_skill="Skill A")],
        rules=[*default_rules(), MaxConsecutiveDaysRule(value=6, weight=2)],
    ),
)
solution = solve_request(request)
print(solution.schedule)                 # {"Worker 01": [ShiftAssignmentModel(...)]}
print(solution.model_dump_json())        # ready to send over the wire
```

`ScheduleRequest.model_validate_json(...)` accepts the same JSON back: the rule
list is a discriminated union, so `{"name": "max_consecutive_days", "value": 6}`
picks the right rule class.

`es_scheduling.demo.generate_demo_data(seed=7, workers=12, days=14)` builds a
made-up request with skills, unavailable and preferred days, and rules that solve
to hard 0.

## Starting from an earlier solution

`with_initial_solution(problem, solution)` returns a new request whose solve
starts from `solution`, which is what `solve_request` returned: a
`ScheduleSolution` or the plain dict of `model_dump(mode="json")`. `problem` is
not changed.

```python
from es_scheduling import with_initial_solution
from es_scheduling.demo import generate_demo_data
from es_scheduling.models import solve_request

request = generate_demo_data()
request.timelimit = 1.0
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
- Local search may still move a seeded shift. `timelimit=0` returns a complete seed
  unchanged, and `pinned` fixes a shift for good. A pinned shift keeps its worker
  whatever the solution says about it.
- `ValueError`, naming the worker or shift, for an unknown id, a shift assigned
  twice, or both assigned and unassigned, a malformed assignment, or a pinned shift
  the solution moves or unassigns.

A seed that breaks a rule is not an error: it is a starting point, and the score
says what it costs.

## Solver options

```python
import es_scheduling as es

config = es.SolverConfig()
config.spent_limit_seconds = 5.0        # how long it may think (default 30)
config.step_count_limit = 100_000       # 0 disables step-count termination
config.no_improvement_limit_seconds = 0.5   # stop after no new best
config.no_improvement_step_count_limit = 50_000
config.random_seed = 42                 # same seed, same search
config.guided_moves = True
config.employee_greedy_construction = True
config.late_acceptance_size = 400
config.ruin_recreate = True
config.ruin_recreate_unimproved_step_trigger = 2_000
config.descent_weight = 0.02            # best-improvement descent over a shift pair
config.descent_max_sweeps = 4
config.ruin_recreate_as_move_weight = 0.0    # ruin+repair as an ordinary move
config.ruin_recreate_as_move_max_ruin = 12
config.change_move_weight = 0.8         # BASIC move mix; weights are relative
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
config.best_score_limit = (0, -1e18)    # stop at the first feasible schedule
config.on_new_best = lambda step, score: print(step, score)   # progress callback
solver = es.Solver(config)
```

`guided_moves=False` also disables the employee-aware moves and falls back to the
generic BASIC change/swap search. The `relocate/list_swap/two_opt/or_opt/tail_swap`
weights are generic LIST-variable options and do not affect this domain.

`es.calculate_score(schedule)` scores a fixed assignment without solving (the
default eight at weight 1).

## Adding a rule

Every rule ships its own incremental evaluation: when the solver moves one shift
between two employees, only the change is recomputed
(`UnitConstraint::deltaCost`), and rules precompute lookup tables once per solve
(`UnitConstraint::onReset`). See the use case's
[README](../README.md#extending) for the steps. Weights, hard/soft flips and
exponents need no code.

## Files

```
bindings.cpp                 pybind11 module (PYBIND11_MODULE(_core))
es_scheduling/__init__.py    re-exports core + constraints + helpers
es_scheduling/_core.pyi      type stub with full API docs
es_scheduling/constraints.py constraint spec classes (declarative assembly)
es_scheduling/models.py      pydantic request/response layer (optional)
es_scheduling/demo.py        generate_demo_data, a made-up sample request
es_scheduling/helpers.py     pure-Python conveniences (list_constraints, make_random_roster, ...)
setup.py, pyproject.toml     build (links ../cpp + imports ../../../include)
tests/                       correctness + model tests (pytest)
```
