# Sports league scheduling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its ten constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | three C++ test cases, five checks that count or list the catalog |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `workdayAwayTripCost`, the kilometres between the two teams' homes for
every match played in a round that is neither a weekend nor a holiday. It moves the long away trips
to the weekends. Every snippet below was compiled and run, and every figure measured, on optimaru
0.4.3; a later change to the solver moves the figures, not the steps.

## How a schedule is scored

A match is assigned a round. The score is kept per **unit**: one round position (its
place in `rounds`), with the matches assigned to it as its **members**. A match
without a round (`-1`) is in no unit and scores nothing. A move changes one or two
matches, so only the rounds they leave and join are scored again. Most constraints here
compare matches on the same or the next round index (`Round.index`), which no single
unit owns: three of the ten constraints keep one cost per round, seven keep one total.

There are two scoring paths, and they give the same score for the same schedule
(`sls_plugin_test` checks it on 500 random assignments):

| | `sl.solve(schedule)` | `sl.solve(schedule, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators, `cpp/src/score_calculator.cpp` and `cpp/src/incremental_calculator.cpp` | the assembly: one component per constraint, `cpp/include/sls/constraints/` |
| Constraints | the ten, at their defaults | the ones you list, with your knobs |
| Steps per second | 2.6 million | 1.3 million |
| Parallel starts | `parallel_solver_count` | one start |
| The match a move takes | the costliest of `costliest_match_sample` random ones (4) | a random one |

The last row is why the two paths do not make the same moves: the default path
reads what each match costs off the monolithic calculator and moves the expensive
ones first. With `SolverConfig.costliest_match_sample = 1` it draws like the
assembly, and a seeded solve of the same length then ends on the same score on both.

The speeds are the demo league (14 teams, 32 rounds, 182 matches), 5 million steps,
one thread of an Apple M4 Max; [step 6](#6-measure) has the script. Steps 1 to 4 work
on the assembly. Step 5 moves a constraint to the monolithic calculators.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.sports_league_scheduling as sl
from optimaru.sports_league_scheduling.constraints import (
    AwayToAwayHopCost,
    RepeatMatchOnNextDayCost,
    WorkdayClassicMatchesCost,
    get_constraints,
)

schedule = sl.demo_schedule()
changed = {
    "workday_classic_matches_cost",
    "repeat_match_on_next_day_cost",
    "away_to_away_hop_cost",
    "away_to_end_hop_cost",
}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    WorkdayClassicMatchesCost(weight=5000),                             # heavier (default 1000)
    RepeatMatchOnNextDayCost(level="soft", weight=500),                 # hard by default
    AwayToAwayHopCost(exponent=2, weight=0.001),                        # the total, squared
]                                                                       # away_to_end_hop_cost: dropped
sl.solve(schedule, time_limit_seconds=10, constraints=picks)
```

`sl.solve()` solves a `LeagueSchedule` in place, and a schedule solved again starts
from the solution it holds: take a new `demo_schedule()` for every solve you compare.
To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.sports_league_scheduling.constraints import score

raw = sl.constraint_costs(schedule, picks)  # raw cost of every constraint: per round, or one total
print(score(raw, picks))                    # the score those picks give this schedule
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/sports_league_scheduling/constraints.py).
Stop here if the constraint you need is one of the ten.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

[`constraints/common.hpp`](../../../optimaru/sports_league_scheduling/cpp/include/sls/constraints/common.hpp)
has a base for each kind of constraint. Pick the base by what the constraint compares:

| The constraint is | Base | You write |
| --- | --- | --- |
| a sum of one term per match, read off the match and its own round | `PerMatchConstraint` | `matchCost(schedule, unit, matchIndex)` |
| a sum over pairs of matches on the same or the next round index | `RoundIndexPairConstraint` | `marginal(schedule, matchIndex, index)` |
| a penalty on a team's run of home (or away) round indices | `TeamSideRunConstraint` | `teamOf(match)` |
| anything else | `optimaru::MemberChangeGlobalConstraint<LeagueSchedule>` | `reset()`, `on_member_changed()`, `cost()` |

The first is a unit constraint: one raw cost per round position. The other three
are global constraints with one total, so a pick's `exponent`, `offset` and `clip`
shape that total and not each round.

The example reads a match and its round, so it is one function. Four edits:

**1. The component**, `optimaru/sports_league_scheduling/cpp/include/sls/constraints/workday_away_trip_cost.hpp`:

```cpp
#ifndef SLS_CONSTRAINTS_WORKDAY_AWAY_TRIP_COST_HPP
#define SLS_CONSTRAINTS_WORKDAY_AWAY_TRIP_COST_HPP

#include "sls/constraints/common.hpp"

namespace sls {

// Soft, off by default (weight 0): workdayAwayTripCost — the kilometres between
// the two teams' homes for a match played on a workday, i.e. a round that is
// neither a weekend nor a holiday: a long away trip belongs on a weekend. Raw
// cost per round position = the summed dist(away, home) of its matches when
// the round is not a weekend/holiday, else 0; 0 = no away team travels to a
// workday match.
struct WorkdayAwayTripCost final : PerMatchConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit WorkdayAwayTripCost(double weight = kDefaultWeight)
        : PerMatchConstraint("workdayAwayTripCost", weight, kDefaultLevel) {}
    double matchCost(const LeagueSchedule& schedule, int unit, int matchIndex) const override {
        if (schedule.rounds[unit].weekendOrHoliday) return 0.0;
        const Match& match = schedule.matches[matchIndex];
        return static_cast<double>(schedule.distance(match.awayTeam, match.homeTeam));
    }
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_WORKDAY_AWAY_TRIP_COST_HPP
```

The name passed to the base, `"workdayAwayTripCost"`, is the struct name with a
lower-case first letter. Python derives `workday_away_trip_cost` from it.

The defaults live in the two constants and nowhere else; Python reads them through
`sl.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this use case has no
medium level, and a pick with `level="medium"` raises `ValueError`. A hard constraint
returns whole numbers, because a hard contribution is rounded per round (or per
total).

**2. The assembly**, `cpp/include/sls/constraints.hpp`: include the header, name
the constraint in the comment above `defaultConstraints()`, and add one line at its end:

```cpp
    assembly.emplace<WorkdayAwayTripCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes and the monolithic calculators stay as they are.
The catalog lists the unit constraints first and the globals after them, each group
in `emplace` order, so the constraint is the fourth name of `sl.list_constraints()`.

**3. The Python spec**, `optimaru/sports_league_scheduling/constraints.py`:

```python
class WorkdayAwayTripCost(Constraint):
    """Km between the two teams' homes for matches in a round that is neither a weekend nor a holiday.
    Opt-in: soft, weight 0."""

    name = "workday_away_trip_cost"
    label_en = "Away trip on a workday"
    label_ko = "평일 원정 이동"
    description_en = (
        "Kilometres from the away team's home to the home team's, for every match in a round that is neither "
        "a weekend nor a holiday. One unit of cost is one kilometre; 0 means no away team travels to a "
        "workday match. Opt-in: the default weight is 0."
    )
    description_ko = (
        "주말도 공휴일도 아닌 라운드의 경기마다 원정 팀 연고지에서 홈 팀 연고지까지의 거리(km)를 더합니다. "
        "1km가 비용 1이며, 0이면 평일 경기를 위해 이동하는 원정 팀이 없습니다. "
        "기본 가중치가 0이므로 가중치를 주어야 점수에 반영됩니다."
    )
```

**4. Its default instance**, in the same file: `workday_away_trip_cost =
WorkdayAwayTripCost()` with the other instances, that instance in `_DEFAULTS` after
`workday_classic_matches_cost` (the order of `list_constraints()`), and both names
in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=sports_league_scheduling
uv sync                                    # rebuilds optimaru.sports_league_scheduling._core
```

```python
from optimaru.sports_league_scheduling.constraints import WorkdayAwayTripCost

others = [spec for spec in get_constraints() if spec.name != "workday_away_trip_cost"]
picks = [*others, WorkdayAwayTripCost(weight=1)]
config = sl.SolverConfig()
config.step_count_limit = 5_000_000
config.random_seed = 1
schedule = sl.demo_schedule()
sl.solve(schedule, config=config, constraints=picks)
print(sum(sl.constraint_costs(schedule, picks)["workday_away_trip_cost"]["costs"]))
```

On the demo league that sum is 130,898 km. A default solve of the same length
(`constraints=None`) leaves it at 146,540 km. Only 9 of the 32 rounds are weekends,
so most trips stay on workdays whatever the weight.

What the base gave you: `cost()` of a round is the sum of `matchCost` over its
matches, and when a match moves the engine calls `matchCost` once for the round it
leaves and once for the round it joins. The constraint is already incremental.

## 3. Check it

**Cases worked out by hand**, in `tests/sports_league_scheduling/cpp/sls_plugin_test.cpp`.
One with the other single-component cases in `verifyPartialAssemblies()`:

```cpp
    {  // workdayAwayTripCost alone: team 1 travels d(1,0) = 21 km to the
        // weekday round 0; the match on the weekend round 1 is free.
        LeagueSchedule s = makeBaseContiguous(4, 2);
        useSyntheticDistances(s);
        s.rounds[1].weekendOrHoliday = true;
        addMatch(s, 0, 1, /*round*/ 0);
        addMatch(s, 2, 3, /*round*/ 1);
        DecomposedCalculator calculator;
        calculator.emplace<WorkdayAwayTripCost>(1.0);
        checkScore("workdayAwayTripCost, a trip on a weekday", scoreWith(calculator, s), 0, -21.0);
    }
```

And two in `verifyRawCostsByHand()`, where every constraint has a case that costs and a
case that costs 0, read with `checkRaw()`.

```sh
cmake -B build -DOPTIMARU_USE_CASES=sports_league_scheduling && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The counts.** An eleventh constraint fails five checks that count or list the catalog.
Update them:

- `sls_plugin_test.cpp`: `constraint_totals().size() == 10`, and `rows.size() == 10`
  in `verifyCatalog()`. Add the new row there as `rows[3]`; the globals move down by
  one. `rowIs()` expects a constraint that is on by default; make it expect
  `row.default_enabled == (weight > 0.0)`, as vehicle routing's test does;
- `tests/sports_league_scheduling/test_constraint_specs.py`:
  `test_list_constraints_matches_the_spec_names` (the list), and `CATALOG`, which
  `test_constraint_info_reports_the_cpp_defaults` and
  `test_constraint_info_matches_the_raw_cost_metadata` read. The first of the two
  also says that every row is on by default.

**Incremental against full.** Solve with every score checked against a full
recompute. It raises `RuntimeError` ("Score corruption: incremental score ... !=
full recompute ...") if a move leaves the score stale:

```python
sl.solve(sl.demo_schedule(), time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

Pick the constraint with a weight for this, as `picks` does. `sls_plugin_test` walks 3,000
random moves and solves under full checks on the default assembly, but there the new
constraint has weight 0 and adds 0 whatever its raw cost is: with a `delta_cost()` made
wrong on purpose, both C++ tests and a `full_assert` solve of `get_constraints()`
still passed, and the solve above raised. To keep the check, add a pick list with
the constraint to `test_full_assert_solves_with_repeats_and_offsets`, and a case by hand
to `tests/sports_league_scheduling/test_constraints.py`.

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `sl.constraint_info()`; add the row. Both, the
docstrings in `_core.pyi` and `constraints.py`, and
`benchmarks/sports-league-scheduling/cpsat/README.md` say "the three unit
constraints". The notebook `examples/sports-league-scheduling.ipynb` prints the list.

## 4. Make it cheaper per move

On the assembly a move costs, for every constraint, one call for the round a match leaves
and one for the round it joins. What that call is depends on the constraint:

| The constraint is | Use | Per move | Example to copy |
| --- | --- | --- | --- |
| a sum of per-match terms | `PerMatchConstraint` | one `matchCost` call | step 2 |
| a sum over pairs of matches on the same or the next round index | `RoundIndexPairConstraint`: the base keeps the matches of every round index, you write `marginal()` | one pass over the matches of the round indices the match can pair with | [`team_conflict_cost.hpp`](../../../optimaru/sports_league_scheduling/cpp/include/sls/constraints/team_conflict_cost.hpp), [`home_to_away_hop_cost.hpp`](../../../optimaru/sports_league_scheduling/cpp/include/sls/constraints/home_to_away_hop_cost.hpp) |
| a run of round indices per team | `TeamSideRunConstraint` | one pass over that team's round indices | [`consecutive_home_matches_cost.hpp`](../../../optimaru/sports_league_scheduling/cpp/include/sls/constraints/consecutive_home_matches_cost.hpp) |
| anything else across rounds | `optimaru::MemberChangeGlobalConstraint<LeagueSchedule>`: keep your own counters in `on_member_changed()` | what your update costs | [`repeat_match_on_next_day_cost.hpp`](../../../optimaru/sports_league_scheduling/cpp/include/sls/constraints/repeat_match_on_next_day_cost.hpp) |

A global's update must be exactly reversible: the engine undoes a rejected move by
sending the opposite joins and leaves. `RoundIndexPairConstraint` gets that by adding
the marginal before a match enters its bucket and subtracting it after the match
left. `full_assert` (step 3) checks it.

The example is in the first row, and one call per round is as cheap as a constraint gets.
Two things were tried on top of it, and neither moved the rate.

**Compute once per solve.** The facts never change during a solve, so a term that
reads only facts can be a table. `on_reset()` runs once, when the solve starts:

```cpp
    // One table per solve: matchCost() below is then two array reads.
    void on_reset(const LeagueSchedule& schedule) override {
        PerMatchConstraint::on_reset(schedule);
        tripKm_.resize(schedule.matches.size());
        for (std::size_t m = 0; m < tripKm_.size(); ++m) {
            const Match& match = schedule.matches[m];
            tripKm_[m] = static_cast<double>(schedule.distance(match.awayTeam, match.homeTeam));
        }
    }
    double matchCost(const LeagueSchedule& schedule, int unit, int matchIndex) const override {
        return schedule.rounds[unit].weekendOrHoliday ? 0.0 : tripKm_[matchIndex];
    }

private:
    std::vector<double> tripKm_;  // per match: dist(away, home)
```

With the constraint at weight 1 the assembly ran at 1.24 million steps per second before
and 1.26 million after, and the solve ended on the same score, as it must. That is
no gain this machine can show: each figure is the median of eleven runs that spread
from 1.14 to 1.30 million. The term was three reads and is now two; a term that
walks a list or a map gains more. Under `full_assert` the engine rebuilds on every
step, so `on_reset()` runs every step there.

**Say which units the constraint can charge.** `scoped_units()` returns the units a constraint
is ever non-zero for, and the engine skips it on the others. Returning the 23
workday rounds of the demo gave 1.29 million in a session where the table alone
gave 1.30, and it changes what a caller sees: `sl.constraint_costs()` then lists
the constraint's `units`, and a pick with `clip=False` charges the 23 rounds instead of
all 32. None of the ten constraints is scoped, and the example is not either.

**A constraint at weight 0 is still computed.** `sl.constraint_costs()` reports it, so the
assembly keeps its raw cost on every move: `get_constraints()` now returns eleven
constraints, and a solve with them ran at 1.29 million steps per second where the ten ran
at 1.33 million. A caller who does not want the constraint leaves it out of the list, as
`others` does in step 2. `sl.solve(schedule)` is not affected.

**Keep raw costs whole numbers.** Every constraint here counts matches or kilometres, so
sums stay exact and a hard total is never rounded. Return `static_cast<double>` of
an integer.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`sl.solve(schedule)` must score it, and `sl.solve(schedule)` scores with the
monolithic calculators: twice the steps per second of the assembly, the only path
that runs parallel starts, and the only one that picks the costliest match.

`sls_plugin_test` compares the default assembly with the monolithic calculators on
random assignments, so it fails from the moment the weight is not 0 until both
sides agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
The monolithic calculators read it as `kSoftWeight<WorkdayAwayTripCost>`, which
fails to compile if the weight is not a whole number or the level is not soft
(`kHardWeight<>` for a hard constraint; both are in `common.hpp`).

**2. The full recompute.** `Breakdown` in `cpp/include/sls/score_calculator.hpp`
has one member per constraint, named like the constraint: add
`std::int64_t workdayAwayTripCost = 0;`. In `cpp/src/score_calculator.cpp`, add
`kSoftWeight<WorkdayAwayTripCost> * workdayAwayTripCost` to `Breakdown::softTotal()`,
and the term to the loop over the matches in `computeBreakdown()`:

```cpp
        // soft 11: the away trip to a round outside weekends/holidays.
        if (!schedule.rounds[match.round].weekendOrHoliday)
            b.workdayAwayTripCost += schedule.distance(match.awayTeam, match.homeTeam);
```

**3. The incremental calculator**, `cpp/src/incremental_calculator.cpp`, keeps one
running total per constraint. Declare `std::int64_t workdayTrip_ = 0;` in
`cpp/include/sls/incremental_calculator.hpp`, set it to 0 in `rebuild()`, add
`kWorkdayTripWeight * workdayTrip_` to the soft level in `score()`, and keep it in
`insert()` (and, with `-=` twice, in `retract()`):

```cpp
constexpr std::int64_t kWorkdayTripWeight = kSoftWeight<WorkdayAwayTripCost>;
```

```cpp
    // soft 11: the away trip to a round outside weekends/holidays.
    if (!schedule_->rounds[round].weekendOrHoliday) {
        const int d = schedule_->distance(match.awayTeam, match.homeTeam);
        workdayTrip_ += d;
        matchCost_[matchIndex] += kWorkdayTripWeight * d;
    }
```

The last line is this use case's own: `matchCost_` is the soft cost each match
carries, which the default path reads to pick the match to move. Leave it out and
the scores still agree, but `sls_test` fails ("per-match travel cost tracks the soft
breakdown"), and the search no longer sees the new cost when it picks.

That is all for a per-match constraint: the default path ran at 2.48 million steps per
second with the eleventh constraint in it. A constraint over pairs of matches scans the buckets
of the neighbouring round indices in `insert()` and `retract()`, as the three hop
constraints do, and credits each pair to both matches in `matchCost_`.

**4. What else moves.** A constraint that is on by default changes every default score.
With the edits above the parity checks pass and these fail:

- `sls_test`: six hand-computed scores, "startToAwayHopCost", the three
  "...HopCost (+start/end)", "awayToEndHopCost on the only round" and "sparse
  indices: both boundary hops";
- `sls_plugin_test`: "named edits == the same assembly listed by hand" (the named
  side drops one constraint from the defaults and now keeps the new one), and the catalog
  row of step 3 (weight 0, off by default);
- `test_constraint_specs.py`: `test_the_hand_instance`, and through `CATALOG`
  `test_constraint_info_reports_the_cpp_defaults` and
  `test_weight_none_means_the_component_default`; `test_hops` in
  `test_constraints.py`;
- the tables, the score formula and the "ten" in [README.md](README.md),
  [python.md](python.md), `constraints.py`, `__init__.py`, `_core.pyi`,
  `score_calculator.hpp`, `incremental_calculator.hpp` and `common.hpp`;
- the CP-SAT model the benchmark compares against,
  `external_solver.sports_league_scheduling` in
  [external-solver](https://github.com/optimaru/external-solver): a plugin class that
  subclasses the spec, in its `constraints.py`, and its entry in `REGISTRY`. Without
  it [`benchmarks/sports-league-scheduling`](../../../benchmarks/sports-league-scheduling)
  no longer compares one objective. Its published results and `CHANGELOG.md` follow.

## 6. Measure

**Steps per second.** The same number of steps on both paths, with one seed:

```python
import time

import optimaru.sports_league_scheduling as sl
from optimaru.sports_league_scheduling.constraints import get_constraints

steps = 5_000_000
lines = (("default path", None, 4), ("default path, uniform pick", None, 1), ("assembly", get_constraints(), 4))
for label, picked, sample in lines:
    schedule = sl.demo_schedule()
    config = sl.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    config.costliest_match_sample = sample  # 4 is the default; the assembly does not use it
    start = time.perf_counter()
    sl.solve(schedule, config=config, constraints=picked)
    seconds = time.perf_counter() - start
    print(f"{label:27s} {steps / seconds / 1e6:5.2f} M steps/s  soft {schedule.score.soft}")
```

The last two lines end on the same soft score: same seed, same steps, same moves,
same scoring. If they differ after a change, the two paths no longer score alike.
The first line makes other moves, so it ends elsewhere.

| Measured on the demo league, 5 million steps | Steps per second |
| --- | --- |
| default path, ten constraints | 2.61 million |
| default path, ten constraints, `costliest_match_sample = 1` | 2.65 million |
| assembly, ten constraints | 1.33 million |
| assembly, ten constraints and the constraint of step 2 at weight 0 | 1.29 million |
| assembly, ten constraints and the constraint of step 2 at weight 1 | 1.24 million |
| assembly, the same with the table of step 4 | 1.26 million |
| default path, eleven constraints (step 5) | 2.48 million |
| default path, eleven constraints, `costliest_match_sample = 1` | 2.51 million |
| assembly, eleven constraints (step 5) | 1.26 million |

Each figure is the median of three runs, the two around step 4 of eleven. Other
work on the machine moved single runs by up to 8%.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after: solve
`sl.demo_schedule()` with `time_limit_seconds` and `random_seed`, with and without
the constraint in `constraints`. `./build/sports_league_scheduling/sls_demo 10` solves the
C++ demo league for ten seconds and prints every new best. Against CP-SAT:
`uv run sports-league-scheduling-compare --seconds 10 --workers 8` in
`benchmarks/sports-league-scheduling/cpsat`
([README](../../../benchmarks/sports-league-scheduling/README.md)). The last two run
the default path only, so they measure a constraint after step 5.

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/sls/constraints/<subject>_cost.hpp` | new | | table | weight |
| `cpp/include/sls/constraints.hpp` | include, `emplace`, comment | | | comment |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/sports_league_scheduling/cpp/sls_plugin_test.cpp` | | three cases, count, catalog rows | | builder case, catalog row |
| `tests/sports_league_scheduling/test_constraint_specs.py`, `test_constraints.py` | | list, `CATALOG`, cases with the constraint picked | | default scores |
| `docs/use-cases/sports-league-scheduling/README.md`, `python.md`, `_core.pyi` | | table row, lists | | defaults, formula |
| `cpp/include/sls/score_calculator.hpp`, `cpp/src/score_calculator.cpp` | | | | `Breakdown` member, the term |
| `cpp/include/sls/incremental_calculator.hpp`, `cpp/src/incremental_calculator.cpp` | | | | the running total, `matchCost_` |
| `tests/sports_league_scheduling/cpp/sls_test.cpp` | | | | expected scores |
| external-solver, `benchmarks/sports-league-scheduling` | | | | the same constraint, results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.
