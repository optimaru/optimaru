# Tournament scheduling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its four constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | two C++ test cases, two counts in the C++ test, five lists in the Python tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `consecutiveDaysCost`, for every team the pairs of its assignments on
two days in a row. It gives every team a day off between two matches. Every snippet below was
compiled and run, and every figure measured, on optimaru 0.4.3, except these, measured again on
optimaru 0.5.1, where the assembly gained `TeamCountMove`: the steps per second (but the
0.4.3 comparison step 4 names as such), the solve results with constraints picked and the
test outputs of step 5. A later change to the solver moves the figures, not the steps.

## How a schedule is scored

A match slot (`TeamAssignment`) on a day is assigned a team. For the engine a
**unit** is one team, and its **members** are the slots given to it. A slot without a
team (`-1`) is in no unit and scores nothing. A move changes one or two slots, so
only the teams they leave and join are scored again. Two constraints are sums per team and
are unit constraints (`teamConflictCost`, `teamUnavailabilityCost`). The two
imbalance constraints are statistics across teams or team pairs, so they are **global**:
each keeps one total for the whole schedule.

There are two scoring paths, and they give the same score for the same schedule.
`ts_plugin_test` compares the default assembly with the full recompute,
`ScoreCalculator::score()`, on 500 random schedules; `ts_test` compares the
incremental calculator with it over 3,000 random moves.

| | `ts.solve(schedule)` | `ts.solve(schedule, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/ts/constraints/` |
| Constraints | the four, at their defaults | the ones you list, with your knobs |
| Moves | change, swap, ruin and recreate, and `TeamCountMove` | the same |
| Steps per second | 2.33 million (3.12 million without `TeamCountMove`) | 1.20 million (1.72 million without) |
| Parallel starts | `parallel_solver_count` | one start |

The speeds are the benchmark's `xlarge` instance (800 slots, 20 teams, 100 days),
4 million steps on the default path and 1 million on the assembly, one thread of an
Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work on the assembly.
Step 5 moves a constraint to the monolithic calculators.

`TeamCountMove` (`cpp/src/solver.cpp`) empties or refills whole teams in one move. It matters for
the medium level: on the demo instance, after 1 million seeded steps, both paths end on `0 medium`
(soft -1.54919), on the same schedule; with `config.team_count_moves = False` both end on a medium
of 1.19523 (soft -1195232.56348). The move steers by the per-team loads and the per-(team, day)
counts. On the default path it reads them off the monolithic calculator; the assembly has no such
counters, so there the move keeps its own, recounted from the schedule each time it is drawn
(`OwnedCounts` in `solver.cpp`, `verifyTeamCountMove()` in `ts_plugin_test`). It picks its target
team counts from the two imbalance constraints at their default weights (`idealPenaltyFor()`),
whatever the assembly holds; the assembly's score judges every reshape, so with an assembly
that leaves those two out the move only costs steps, and `team_count_moves = False` turns it off.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.tournament_scheduling as ts
from optimaru.tournament_scheduling.constraints import ConfrontationCountImbalanceCost, TeamUnavailabilityCost, get_constraints
from optimaru.tournament_scheduling.models import demo_data

schedule = demo_data()
changed = {"confrontation_count_imbalance_cost", "team_unavailability_cost", "assignment_count_imbalance_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    ConfrontationCountImbalanceCost(weight=10),                         # heavier (default 1)
    TeamUnavailabilityCost(level="soft"),                               # hard by default
]                                                                       # assignment_count_imbalance_cost: dropped
result = ts.solve(schedule, time_limit_seconds=10, constraints=picks)
```

To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.tournament_scheduling.constraints import score

raw = ts.constraint_costs(result.to_core(), picks)  # raw cost of every constraint, per team or one total
print(score(raw, picks))                            # the score those picks give this schedule
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/tournament_scheduling/constraints.py).
`assignment_count_imbalance_cost` is on the medium level; any constraint may move onto any
of the three levels. Stop here if the constraint you need is one of the four.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/tournament_scheduling/cpp/include/ts/constraints/common.hpp)
has no base of its own for a constraint; it names the engine's two. Pick one by what the constraint
is:

| The constraint is | Base | You write |
| --- | --- | --- |
| a sum per team, read off the team's own slots | `TsUnitConstraint` (`optimaru::UnitConstraint<TournamentSchedule>`) | `cost(schedule, unit, members)` |
| a statistic across teams or team pairs | `MemberChangeGlobalConstraint` | `reset()`, `on_member_changed()` and `cost()` |

The example counts pairs inside one team, so it is a unit constraint and one
function. Four edits:

**1. The component**, `optimaru/tournament_scheduling/cpp/include/ts/constraints/consecutive_days_cost.hpp`:

```cpp
#ifndef TS_CONSTRAINTS_CONSECUTIVE_DAYS_COST_HPP
#define TS_CONSTRAINTS_CONSECUTIVE_DAYS_COST_HPP

#include "ts/constraints/common.hpp"

namespace ts {

// Soft, off by default (weight 0): consecutiveDaysCost — a team playing on
// two days in a row. Raw cost per team = the number of pairs of assignments
// it holds on days whose dateIndex differ by one, so 0 = the team has a day
// off between any two of its matches.
struct ConsecutiveDaysCost final : TsUnitConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit ConsecutiveDaysCost(double weight = kDefaultWeight)
        : TsUnitConstraint("consecutiveDaysCost", weight, kDefaultLevel) {}
    double cost(const TournamentSchedule& schedule, int /*unit*/,
                const std::vector<int>& members) const override {
        std::int64_t pairs = 0;
        for (std::size_t i = 0; i < members.size(); ++i) {
            for (std::size_t j = i + 1; j < members.size(); ++j) {
                if (inARow(schedule, members[i], members[j])) ++pairs;
            }
        }
        return static_cast<double>(pairs);
    }

private:
    static bool inARow(const TournamentSchedule& schedule, int left, int right) {
        const int gap = schedule.days[detail::dayOf(schedule, left)].dateIndex -
                        schedule.days[detail::dayOf(schedule, right)].dateIndex;
        return gap == 1 || gap == -1;
    }
};

}  // namespace ts

#endif  // TS_CONSTRAINTS_CONSECUTIVE_DAYS_COST_HPP
```

A slot's `day` is a position in `days`; time is `Day::dateIndex`. The constraint compares
dates, so two days next to each other in the list but two dates apart are not in a row.

The name passed to the base, `"consecutiveDaysCost"`, is the struct name with a
lower-case first letter; Python derives `consecutive_days_cost` from it. The defaults
live in the two constants and nowhere else; Python reads them through
`ts.constraint_info()`. `kDefaultLevel` is `Hard`, `Medium` or `Soft`, and the weight
is in units of that level. A medium constraint states its weight in medium units
(`assignmentCountImbalanceCost`: 1): the domain folds the medium level into the
engine's soft level itself, as `medium * 1e6` (`kMediumFold` and `kMediumScale` in
[`domain.hpp`](../../../optimaru/tournament_scheduling/cpp/include/ts/domain.hpp)), so
no weight carries the 10^6. A hard constraint returns whole numbers, because a hard
contribution is rounded.

**2. The assembly**, `cpp/include/ts/constraints.hpp`: include the header, and add
one line to `defaultConstraints()` after `TeamUnavailabilityCost`:

```cpp
    assembly.emplace<ConsecutiveDaysCost>();
```

The catalog lists the unit constraints first, then the globals, each in `emplace`
order, so the constraint is the third row wherever its line goes. A constraint has to be in
`defaultConstraints()` for Python to see it. With `kDefaultWeight = 0.0` it scores
nothing until a caller gives it a weight, so no default solve changes and the
monolithic calculators stay as they are.

**3. The Python spec**, `optimaru/tournament_scheduling/constraints.py`:

```python
class ConsecutiveDaysCost(Constraint):
    """Pairs of assignments one team has on two days in a row. Opt-in: soft, weight 0."""

    name = "consecutive_days_cost"
    label_en = "Team plays on consecutive days"
    label_ko = "연속된 날의 경기"
    description_en = (
        "A team scheduled on two days in a row. Each pair of a team's assignments on days whose date indexes "
        "differ by one costs 1; 0 means every team has a day off between any two of its matches."
    )
    description_ko = (
        "한 팀이 연속된 이틀에 경기하는 경우입니다. 한 팀의 배정 가운데 날짜 인덱스가 1 차이 나는 두 날의 "
        "배정 쌍마다 비용 1이며, 0이면 모든 팀이 경기와 경기 사이에 하루 이상 쉽니다."
    )
```

**4. Its default instance**, in the same file: `consecutive_days_cost =
ConsecutiveDaysCost()` with the other instances, that instance in `_DEFAULTS` after
`team_unavailability_cost`, and both names in `__all__`. `_DEFAULTS` must be in
catalog order: `test_weight_none_means_the_component_default` pairs
`get_constraints()` with `ts.constraint_info()` one by one.

Build and use it:

```sh
export OPTIMARU_USE_CASES=tournament_scheduling
uv sync                                    # rebuilds optimaru.tournament_scheduling._core
```

```python
from optimaru.tournament_scheduling.constraints import ConsecutiveDaysCost

others = [spec for spec in get_constraints() if spec.name != "consecutive_days_cost"]
picks = [*others, ConsecutiveDaysCost(weight=1)]
for picked in (picks, others, None):
    config = ts.SolverConfig()
    config.step_count_limit = 1_000_000
    config.random_seed = 1
    result = ts.solve(demo_data(), config=config, constraints=picked)
    print(sum(ts.constraint_costs(result.to_core(), picks)["consecutive_days_cost"]["costs"]))
```

On the demo instance that sum is 34 with the constraint picked, and 42 with the same
assembly without it, as many as a default solve (`None`) leaves. All three reach a medium of
0 (six teams of 12 slots): the medium level outweighs a weight of 1 on soft, so the constraint
only chooses among the six-team schedules.

What the base gave you: the engine keeps one cost per team and calls `cost()` again
for the team a slot leaves and the team it joins, so the constraint is right after any move.
Each call compares every pair of the team's slots; [step 4](#4-make-it-cheaper-per-move)
cuts that.

## 3. Check it

**Cases worked out by hand**, in `tests/tournament_scheduling/cpp/ts_plugin_test.cpp`,
one positive and one zero case with the other raw costs in `verifyRawCostsByHand()`.
`makeBase()` gives day `d` the date `d`:

```cpp
    {  // consecutiveDaysCost, per team: T0 on D0, D1, D2 -> 2 pairs; T1 on
        // D3, D4 -> 1.
        TournamentSchedule positive = makeBase(/*teams*/ 2, /*days*/ 5);
        for (int day : {0, 1, 2}) addAssignment(positive, day, 0);
        for (int day : {3, 4}) addAssignment(positive, day, 1);
        checkRaw("consecutiveDaysCost: 2 and 1 pairs", positive, "consecutiveDaysCost", {2.0, 1.0});
        // Neighbours in the list, not in time: D1 is dateIndex 2.
        TournamentSchedule zero = makeBase(/*teams*/ 2, /*days*/ 2);
        zero.days[1].dateIndex = 2;
        addAssignment(zero, 0, 0);
        addAssignment(zero, 1, 0);
        checkRaw("consecutiveDaysCost: a day off between", zero, "consecutiveDaysCost", {0.0, 0.0});
    }
```

and a weighted score in `verifyPartialAssemblies()`, where the other single-component
cases are: T0 on days 0, 1, 2 and 4, `calculator.emplace<ConsecutiveDaysCost>(1.0)`,
expected `0hard/-2soft`. The constraint is off by default, so the weight has to be given.

```sh
cmake -B build -DOPTIMARU_USE_CASES=tournament_scheduling && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The counts.** A fifth constraint fails every check that counts or lists the catalog.
Update them:

- `ts_plugin_test.cpp`: `constraint_totals().size() == 4` in
  `verifyConstraintTotals()`, and `rows.size() == 4` in `verifyCatalog()`. The two
  globals move to `rows[3]` and `rows[4]`. `rowIs()` expects a constraint that is on by
  default, so check the new row's fields directly (`!rows[2].default_enabled`);
- `tests/tournament_scheduling/test_constraint_specs.py`, five tests:
  `test_list_constraints_matches_the_spec_names` (the list),
  `test_module_level_instances_keep_every_default` (the instances),
  `test_constraint_info_reports_the_cpp_defaults` (the row is
  `("soft", 0.0, True, False, False)`),
  `test_constraint_info_matches_the_raw_cost_metadata` (`unit_names =
  ts.list_constraints()[:3]`, three unit constraints now) and
  `test_weight_none_means_the_component_default` (`[1, 1, 0, 1, 1]`).

**Incremental against full.** Solve with every score checked against a full
recompute. A move that leaves the score stale raises `RuntimeError`, "Score
corruption: incremental score ... != full recompute ..." (the engine's
`ScoreMismatchError`):

```python
ts.solve(demo_data(), time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

With the `delta_cost()` of step 4 deliberately doubled, this raised within the second.
`ts_plugin_test` checks the default assembly, which now holds the constraint, the same way:
3,000 random moves, and a two-second solve under `ScoreChecks::Full`.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `ts.constraint_info()`; add the row. The text
around them says "all four are scored by default" and the docstring of
`constraints.py` says "every default is 1"; neither holds now. The notebook
`examples/tournament-scheduling.ipynb` prints the constraint list.

**The CP-SAT model.** `external_solver.tournament_scheduling` in
[external-solver](https://github.com/optimaru/external-solver) has one plugin per
constraint: a subclass of the Python spec that builds the CP-SAT terms and reads its
defaults from `ts.constraint_info()`. Its test compares its catalog with
`ts.list_constraints()`, so a new constraint needs its plugin there, also while it is off.
[`benchmarks/tournament-scheduling/cpsat`](../../../benchmarks/tournament-scheduling/cpsat)
states for each constraint whether the CP-SAT term is raw-identical to the engine's (today
the two hard constraints are, the two imbalance constraints are a surrogate); say it for the new
one too.

## 4. Make it cheaper per move

On the assembly a move costs, for every picked constraint, one call for the team a slot
leaves and one for the team it joins. What that call is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a sum per team that is not a sum of per-slot terms (a clamp, a "at least one") | `cost()` alone, `scoped_units()` if only some teams can be charged | `cost()` over the team | [`team_unavailability_cost.hpp`](../../../optimaru/tournament_scheduling/cpp/include/ts/constraints/team_unavailability_cost.hpp) |
| a sum over pairs of one team's slots | `cost()`, `supports_delta()` and `delta_cost()` | one pass over the team's other slots | [`team_conflict_cost.hpp`](../../../optimaru/tournament_scheduling/cpp/include/ts/constraints/team_conflict_cost.hpp) |
| a statistic across teams or team pairs | `MemberChangeGlobalConstraint`: keep your own aggregates in `on_member_changed()` | what your update costs | [`assignment_count_imbalance_cost.hpp`](../../../optimaru/tournament_scheduling/cpp/include/ts/constraints/assignment_count_imbalance_cost.hpp), [`confrontation_count_imbalance_cost.hpp`](../../../optimaru/tournament_scheduling/cpp/include/ts/constraints/confrontation_count_imbalance_cost.hpp) |

The example is a sum over pairs, and with `cost()` alone the engine compares every
pair of a touched team on every move: 780 pairs for a team of 40 slots, twice per
move. `delta_cost()` returns what one slot adds given the others, and must equal
`cost(with the slot) - cost(without it)` exactly; the slot itself may or may not be in
`members`, so skip it. Then the engine keeps the team's cost and adds or subtracts the
delta.

**Compute once per solve.** The facts never change during a solve, so the date of
each slot can be a table. `on_reset()` runs when the objective is rebuilt: when the
solve starts and once more for the final score (two calls in a seeded million-step
solve), and after every step under `full_assert`. In the header, after the
constructor; `cost()` keeps the loop of step 2 and calls `inARow(members[i], members[j])`:

```cpp
    // Once per solve: the date of every assignment, so a pair is one
    // subtraction.
    void on_reset(const TournamentSchedule& schedule) override {
        dateOf_.clear();
        dateOf_.reserve(schedule.teamAssignments.size());
        for (const TeamAssignment& assignment : schedule.teamAssignments)
            dateOf_.push_back(schedule.days[assignment.day].dateIndex);
    }
    // A sum over pairs inside one team, so one assignment adds the others
    // it sits next to in time. The engine then updates the cached cost by
    // this delta instead of calling cost() again.
    bool supports_delta() const override { return true; }
    double delta_cost(const TournamentSchedule& /*schedule*/, int /*unit*/,
                      const std::vector<int>& members, int entity) const override {
        std::int64_t pairs = 0;
        for (int assignment : members) {
            if (assignment != entity && inARow(assignment, entity)) ++pairs;
        }
        return static_cast<double>(pairs);
    }

private:
    bool inARow(int left, int right) const {
        const int gap = dateOf_[left] - dateOf_[right];
        return gap == 1 || gap == -1;
    }

    std::vector<int> dateOf_;  // per assignment: the dateIndex of its day
```

Measured on the instance of step 6, in million steps per second:

| Picked | Step 2: `cost()` alone | Step 4: delta and table |
| --- | --- | --- |
| the four and the constraint at weight 1 | 0.61 | 0.88 |
| `get_constraints()`, the constraint at its default weight 0 | 0.58 | 0.95 |
| the four, without the constraint | 1.20 | 1.20 (the same code) |

The delta does almost all of it: in a session on 0.4.3, before the assembly ran
`TeamCountMove`, the first row read 1.38 with the delta and no table, against 1.40 with both. Both forms end on the same score for the same
seed and steps, as they must.

**A constraint at weight 0 is still computed.** The assembly evaluates every picked unit
constraint on every move, also one whose weight is 0, because `constraint_costs()`
reports its raw cost. `get_constraints()` holds the new constraint at its default weight 0,
so `ts.solve(schedule, constraints=get_constraints())` pays for it (the second row). A
caller who does not want the constraint leaves it out of the picks, as the snippet of step 2
does with `others`.

**A global would be O(1), and means something else.** A `MemberChangeGlobalConstraint`
keeping a count per (team, date) needs no pass over the team. But `exponent` and
`offset` then shape the one total instead of each team's count, so `exponent=2` would
no longer punish one team with many back-to-back days more than several with one each.

**Keep raw costs whole numbers**, as every constraint here but the two unfairness values
does: sums stay exact and a hard total is never rounded.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`ts.solve(schedule)` must score it, and `ts.solve(schedule)` scores with the
monolithic calculators: 1.9 times the steps per second of the assembly here, and the only
path that runs parallel starts.

`ts_plugin_test` compares the default assembly with `ScoreCalculator::score()`
(`verifyAssemblyParity()`), and `ts_test` compares the incremental calculator with it.
So from the moment the weight is not 0 they fail, until all three agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators weigh in `int64`:
`kHardWeight<>`, `kMediumWeight<>` and `kSoftWeight<>` in `common.hpp` do not compile
otherwise, nor when the constraint is on another level than the one it is summed into.

**2. One definition of "next to".** Both calculators keep counts per (team, day), so
they need, per day, the days one date away. Add it to the component as a static
function, after `delta_cost()`:

```cpp
    // Per day: the days whose dateIndex differs from its own by one. The
    // monolithic calculators count the same pairs from it.
    static std::vector<std::vector<int>> neighbourDays(const TournamentSchedule& schedule) {
        std::vector<std::vector<int>> neighbours(schedule.days.size());
        for (std::size_t day = 0; day < schedule.days.size(); ++day) {
            for (std::size_t other = 0; other < schedule.days.size(); ++other) {
                const int gap = schedule.days[day].dateIndex - schedule.days[other].dateIndex;
                if (gap == 1 || gap == -1) neighbours[day].push_back(static_cast<int>(other));
            }
        }
        return neighbours;
    }
```

**3. The full recompute**, `cpp/src/score_calculator.cpp`, in `ScoreCalculator::score()`
after the confrontation unfairness, from the per-(team, day) counts `cellCnt`; then
add the weighted total to the soft penalty:

```cpp
    // soft 5: consecutiveDaysCost — per team, the pairs of its assignments on
    // days whose dateIndex differ by one (each pair of days counted once).
    std::int64_t consecutivePairs = 0;
    const std::vector<std::vector<int>> neighbours = ConsecutiveDaysCost::neighbourDays(schedule);
    for (int d = 0; d < dayCount; ++d) {
        for (int next : neighbours[d]) {
            if (next < d) continue;
            for (int t = 0; t < teamCount; ++t) {
                consecutivePairs += static_cast<std::int64_t>(
                                            cellCnt[static_cast<std::size_t>(t) * dayCount + d]) *
                                    cellCnt[static_cast<std::size_t>(t) * dayCount + next];
            }
        }
    }
```

```cpp
    score.soft = combinedSoft(
            static_cast<double>(kMediumWeight<AssignmentCountImbalanceCost>) * medium,
            static_cast<double>(kSoftWeight<ConfrontationCountImbalanceCost>) * soft +
                    static_cast<double>(kSoftWeight<ConsecutiveDaysCost> * consecutivePairs));
```

**4. The incremental calculator.** Declare the table
`std::vector<std::vector<int>> neighbourDays_` and the running total
`std::int64_t consecutivePairs_ = 0` in `cpp/include/ts/incremental_calculator.hpp`. In
`cpp/src/incremental_calculator.cpp`, `rebuild()` builds the table
(`neighbourDays_ = ConsecutiveDaysCost::neighbourDays(solution);`) and zeroes the
total. `insert()` adds, and `retract()` subtracts, the team's slots on the days next
to this one, after the hard terms of each:

```cpp
    for (int next : neighbourDays_[day]) consecutivePairs_ += cellCnt_[cellOffset(team, next)];
```

```cpp
    for (int next : neighbourDays_[day]) consecutivePairs_ -= cellCnt_[cellOffset(team, next)];
```

and `score()` adds
`static_cast<double>(kSoftWeight<ConsecutiveDaysCost> * consecutivePairs_)` to the
soft penalty after the confrontation unfairness, in the same order as the full
recompute. The neighbour days never include the slot's own day, so it does not
matter whether the cell has been counted yet.

That is all: with the parity checks passing, the default path ran at 2.27 million
steps per second with the fifth constraint in it, against 2.33 million without
(2.96 against 3.12 million with `team_count_moves` off). A `full_assert` default
solve of the demo passed.

**5. What else moves.** With the weight at 1 and the three calculators in agreement,
these still failed:

- the row of step 3 in `verifyCatalog()` and in
  `test_constraint_info_reports_the_cpp_defaults`, and the weights in
  `test_weight_none_means_the_component_default`: they state weight 0, off;
- hand-computed scores that put a team on two days in a row: `teamConflictCost
  (basic)` and `teamUnavailabilityCost (basic)` in `ts_test.cpp` (`-707108` and
  `-1414212` soft instead of `-707107` and `-1414210`), their Python twins
  `test_team_conflict_cost` and `test_team_unavailability_cost` in
  `test_constraints.py`, `test_the_medium_level_folds_into_soft_at_1e6`, and all eight
  checks of `verifyMediumLevel()` in `ts_plugin_test.cpp`; these last two score the
  same schedule, where T1 plays days 0 to 3 and T2 days 0 and 1;
- `verifyTeamCountMove()` in `ts_test.cpp` and its twin on the assembly in
  `ts_plugin_test.cpp`: 72 slots on 18 days are best spread over four teams that play every
  day, which this constraint now charges. Both solves load six teams and end at -46.05 soft
  instead of 0. `TeamCountMove` picks its target team
  counts from the two imbalance constraints alone (`idealPenaltyFor()` in `solver.cpp`), so
  a default constraint that pulls against them is worth teaching it.

Beyond the tests: the tables and the counts ("four", "2 hard + 1 medium + 1 soft") in
[README.md](README.md), [python.md](python.md), `constraints.py`, `__init__.py`,
`_core.pyi`, `score_calculator.hpp` and `common.hpp`; the CP-SAT plugin of step 3 now
counts in every comparison of
[`benchmarks/tournament-scheduling`](../../../benchmarks/tournament-scheduling), so its
published results change, and so does `CHANGELOG.md`.

## 6. Measure

**Steps per second.** A seeded solve with a step limit on each path. The demo has 72
slots, few enough per team that a pair loop costs little, so the script builds the
benchmark's `xlarge` instance (the recipe of
`benchmarks/tournament-scheduling/cpsat/tournament_scheduling_cpsat/benchmark.py`):

```python
import random
import time

import optimaru.tournament_scheduling as ts
from optimaru.tournament_scheduling.constraints import get_constraints


def tournament(day_count=100, per_day=8, penalty_count=200, team_count=20):
    """The benchmark's xlarge instance: the same seeded draw of unavailable days."""
    rng = random.Random(0)
    penalties = []
    while len(penalties) < penalty_count:
        team, day = int(rng.random() * team_count), int(rng.random() * day_count)
        if (team, day) not in penalties:
            penalties.append((team, day))
    return ts.TournamentSchedule(
        [ts.Team(f"Team {t + 1}") for t in range(team_count)],
        [ts.Day(d) for d in range(day_count)],
        [ts.UnavailabilityPenalty(team, day) for team, day in penalties],
        [ts.TeamAssignment(d * per_day + i, d, i) for d in range(day_count) for i in range(per_day)],
    )


def solve(steps, picked, team_count_moves=True):
    schedule = tournament()
    config = ts.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    config.team_count_moves = team_count_moves
    start = time.perf_counter()
    ts.solve(schedule, config=config, constraints=picked)
    return time.perf_counter() - start, (schedule.score.hard, schedule.score.soft)


for label, picked, steps in (("default path", None, 4_000_000), ("assembly", get_constraints(), 1_000_000)):
    start_up, _ = solve(1, picked)  # the construction heuristic
    seconds, score = solve(steps, picked)
    print(f"{label:14s} {steps / (seconds - start_up) / 1e6:5.2f} M steps/s  {score}")
print(solve(1_000_000, None)[1], solve(1_000_000, get_constraints())[1])
```

The last line prints the same score twice: same seed, same steps, same scoring, same
moves. If the two differ after a change, the two paths no longer score or search alike.

| Measured on `xlarge`, median of three runs | Steps per second |
| --- | --- |
| default path, four constraints | 2.33 million |
| default path, four constraints, `team_count_moves` off | 3.12 million |
| assembly, four constraints | 1.20 million |
| assembly, four constraints, `team_count_moves` off | 1.72 million |
| assembly, four + the constraint at weight 1 (step 2, step 4) | 0.61 million, 0.88 million |
| default path, five constraints (step 5) | 2.27 million |

The machine was shared while these ran (at most two measurements at a time); a figure
moved by up to 8% between repeats, never the order of the rows. `TeamCountMove` walks every
slot each time it is drawn, so it costs the assembly about as much as the default path
(1.72 against 1.20 million on the assembly, 3.12 against 2.33 on the default path), and it
buys a better end: after 1 million seeded steps both paths end on soft -6.97615 with it and
on -9.31891 without.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after. There is no C++
quality bench for this use case; the comparison runs from Python, on the default path:

```sh
cd benchmarks/tournament-scheduling/cpsat
export OPTIMARU_USE_CASES=tournament_scheduling    # keep it set: uv rebuilds when it changes
uv sync
uv run tournament-scheduling-compare --seconds 10 --workers 8
```

It solves six instances with both solvers and rescores both answers with the engine
([README](../../../benchmarks/tournament-scheduling/README.md)). `xlarge` is the one
that still separates seeds at 10 seconds; for the engine alone, run
`ts.solve(tournament(), time_limit_seconds=10, random_seed=seed)` over a few seeds on
two builds.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/ts/constraints/<subject>_cost.hpp` | new | | table, delta | weight, `neighbourDays()` |
| `cpp/include/ts/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/tournament_scheduling/cpp/ts_plugin_test.cpp` | | two cases, counts | | the row, medium fixtures, team-count instance |
| `tests/tournament_scheduling/test_constraint_specs.py` | | lists | | the row, the weights |
| `docs/use-cases/tournament-scheduling/README.md`, `python.md` | | table row | | defaults, counts |
| `cpp/src/score_calculator.cpp`, `cpp/src/incremental_calculator.cpp`, `incremental_calculator.hpp` | | | | the term |
| `tests/tournament_scheduling/cpp/ts_test.cpp`, `test_constraints.py` | | | | expected scores, team-count instance |
| external-solver, `benchmarks/tournament-scheduling` | | the plugin | | results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.
