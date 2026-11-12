# Conference scheduling: adding a constraint, step by step

Six steps, from a change that needs no build to a constraint on the default fast path.
Each step is complete by itself, so stop at the first one that does what you need.
The engine contracts behind the steps are in [Extending a use case](../../extending.md);
the model and its 37 constraints are in the [guide](README.md).

| Step | What you do | What it touches |
| --- | --- | --- |
| [1](#1-change-a-constraint-that-exists) | Reweight, drop, re-level or shape a constraint that exists | a Python call |
| [2](#2-a-new-constraint-from-one-function) | Add a constraint, off by default | one term in the full recompute, one header, two lines of C++, one Python class |
| [3](#3-check-it) | Prove it right | one C++ test case, one table row and three counts in the C++ test, two lists in the Python tests |
| [4](#4-make-it-cheaper-per-move) | Cut what it costs per move | the header |
| [5](#5-put-it-on-the-default-path) | Turn it on by default | the two monolithic calculators |
| [6](#6-measure) | Show what it costs and what it buys | no code |

The steps build one example: `roomOverflowCost`, for every talk the people who marked it as a
favorite beyond the seats of its room, times the minutes of its timeslot. It keeps a popular talk
out of a room that cannot hold its audience. (The existing `popularTalksRoomSizeCost` only compares
talks with each other.) Every snippet below was compiled and run, and every figure measured, on
optimaru 0.4.3, and the `idle_at_weight_zero()` override of step 4 on 0.5.1; a later change to
the solver moves the figures, not the steps.

## How a schedule is scored

A talk is assigned one value, a (timeslot, room) cell:
`slot_room = timeslot_index * room_count + room_index`. For the engine a **unit** is
one cell and its **members** are the talks placed in it. Most constraints here compare two
talks in different cells (a shared speaker, overlapping timeslots, shared tags), so
no constraint is a sum over cells. All 37 constraints are **global**: each has one total
for the whole schedule.

There are two scoring paths, and they give the same score for the same schedule.
Both are checked against the full recompute, `ScoreCalculator::score()`: the default
assembly on 150 random schedules (`cs_plugin_test`), the incremental calculator over
3,000 random moves (`cs_test`).

| | `cs.solve(schedule)` | `cs.solve(schedule, constraints=[...])` |
| --- | --- | --- |
| Scored by | the monolithic calculators: `cpp/src/incremental_calculator.cpp` keeps counters per timeslot and updates them for the talk that moves; `cpp/src/score_calculator.cpp` recomputes the final score in full | the assembly: one component per constraint, `cpp/include/cs/constraints/`. All of them read one `Breakdown`, which `ScoreCalculator::breakdownOfPrepared()` recomputes in full, every pair of talks, once per score |
| Constraints | the 37, at their defaults | the ones you list, with your knobs |
| Steps per second | 833 thousand | 13.6 thousand |
| Parallel starts | `parallel_solver_count`; the time limit is split into `restart_segments` (8) runs | one start, one run |

The speeds are a generated conference (70 talks, 18 timeslots, 8 rooms), 2 million steps on the
default path and 60,000 on the assembly, one thread of an Apple M4 Max; [step 6](#6-measure) has the
script. The factor of 60 is by design: this assembly is the path for changing the cost function, not
the fast one
([`constraints/common.hpp`](../../../optimaru/conference_scheduling/cpp/include/cs/constraints/common.hpp)
says why). Steps 1 to 4 work on the assembly. Step 5 moves a constraint to the incremental
calculator.

## 1. Change a constraint that exists

No build. Pass the constraints you want, with the knobs that differ from the defaults:

```python
import optimaru.conference_scheduling as cs
from optimaru.conference_scheduling.constraints import ContentConflictCost, SpeakerMakespanCost, get_constraints

schedule = cs.demo_conference()
changed = {"content_conflict_cost", "speaker_makespan_cost", "language_diversity_cost"}
picks = [
    *[spec for spec in get_constraints() if spec.name not in changed],  # the others as they are
    ContentConflictCost(weight=500),                                    # heavier (default 100)
    SpeakerMakespanCost(level="hard"),                                  # soft by default
]                                                                       # language_diversity_cost: dropped
cs.solve(schedule, time_limit_seconds=10, constraints=picks)            # solves in place
```

To try weights without solving again, score a finished schedule with other picks:

```python
from optimaru.conference_scheduling.constraints import score

raw = cs.constraint_costs(schedule, picks)  # raw cost of every constraint, one total each
print(score(raw, picks))                    # the score those picks give this schedule
```

The knobs (`weight`, `level`, `exponent`, `offset`, `clip`) and picking a constraint twice
are described in [`constraints.py`](../../../optimaru/conference_scheduling/constraints.py).
There is no medium level here: `level` is `"hard"` or `"soft"`. The three
`*_diversity_cost` constraints are rewards: they are never hard and keep their level.
Stop here if the constraint you need is one of the 37.

## 2. A new constraint from one function

A constraint is a component: a struct in its own header that returns a **raw cost**, a
number that is positive for a violation and 0 for none. Weight and level are
applied by the engine.

Here the one function is `select`. All 37 components derive from `BreakdownConstraint`
in [`constraints/common.hpp`](../../../optimaru/conference_scheduling/cpp/include/cs/constraints/common.hpp)
and say only which member of the `Breakdown` is theirs. The raw cost itself is computed
where all the others are, in `ScoreCalculator::breakdownOfPrepared()`. Pick the place
there by what the term reads:

| The term reads | Where it goes in `breakdownOfPrepared()` | Constraints to copy |
| --- | --- | --- |
| one talk, its timeslot and its room | the first loop, over the talks | `roomUnavailabilityCost`, the sixteen tag constraints |
| a count per timeslot, or a span per speaker | a counter filled in the first loop, summed after it | `crowdControlRiskCost`, `speakerMakespanCost` |
| two talks | the pair loop, which has `overlap`, `combined`, `sameDay` and the shared tag counts of the pair | `contentConflictCost`, `themeTrackRoomChangeCost` |

The example reads one talk and its room. Five edits:

**1. The term.** A member of `Breakdown` in `cpp/include/cs/score_calculator.hpp`,
after `speakerMakespanCost`, and its sum in `cpp/src/score_calculator.cpp`, in the
first loop of `breakdownOfPrepared()`, where `talk`, its room `r` and the timeslot's
minutes `dur` are at hand:

```cpp
    std::int64_t roomOverflowCost = 0;     // favorites beyond the capacity x duration
```

```cpp
        // roomOverflowCost: the favorites that do not fit the room.
        const std::int64_t overflow = talk.favoriteCount - schedule.rooms[r].capacity;
        if (overflow > 0) b.roomOverflowCost += overflow * dur;
```

**2. The component**, `optimaru/conference_scheduling/cpp/include/cs/constraints/room_overflow_cost.hpp`:

```cpp
#ifndef CS_CONSTRAINTS_ROOM_OVERFLOW_COST_HPP
#define CS_CONSTRAINTS_ROOM_OVERFLOW_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, off by default (weight 0): roomOverflowCost — a talk that more people
// marked as a favorite than its room seats, weighed by the missing seats times
// the timeslot's duration.
// Raw cost (one global total) = the sum of (favorites beyond the room's
// capacity) x the timeslot's duration, per talk; 0 = no violation (every room
// seats all the favorites of its talks).
struct RoomOverflowCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit RoomOverflowCost(double weight = kDefaultWeight)
        : BreakdownConstraint("roomOverflowCost", weight, kDefaultLevel) {}
    std::int64_t select(const Breakdown& breakdown) const override {
        return breakdown.roomOverflowCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_ROOM_OVERFLOW_COST_HPP
```

The name passed to the base, `"roomOverflowCost"`, is the struct name with a
lower-case first letter. Python derives `room_overflow_cost` from it.

The defaults live in these two constants and nowhere else; Python reads them through
`cs.constraint_info()`. `kDefaultLevel` is `Hard` or `Soft`: this use case has no medium level, and
`level="medium"` raises `ValueError`. Every `Breakdown` member is an `int64` counted positive, in
minutes of the timeslot, so a hard constraint's total is a whole number, as it has to be: a hard
contribution is rounded. A reward (a quantity to maximize) returns its member negated from `select`
and sets `reward = true; level_flippable = false;` in its constructor: it is never hard and keeps
its level
([`language_diversity_cost.hpp`](../../../optimaru/conference_scheduling/cpp/include/cs/constraints/language_diversity_cost.hpp)).

**3. The assembly**, `cpp/include/cs/constraints.hpp`: include the header, and add
one line to `defaultConstraints()` after `SpeakerMakespanCost` (the hard constraints come
first, then the soft penalties, the three rewards last):

```cpp
    assembly.emplace<RoomOverflowCost>();
```

A constraint has to be in `defaultConstraints()` for Python to see it. With
`kDefaultWeight = 0.0` it is there and scores nothing until a caller gives it a
weight, so no default solve changes: the weighted totals of `Breakdown` and the
incremental calculator stay as they are.

**4. The Python spec**, `optimaru/conference_scheduling/constraints.py`:

```python
class RoomOverflowCost(Constraint):
    """A talk with more favorites than its room seats. Valued by missing seats × duration. Opt-in: soft, weight 0."""

    name = "room_overflow_cost"
    label_en = "Room overflow"
    label_ko = "방 수용 인원 초과"
    description_en = (
        "For each talk: the favorites beyond the capacity of its room, times the timeslot's duration in minutes. 0 "
        "means every room seats all the favorites of its talks."
    )
    description_ko = (
        "발표마다 즐겨찾기 수가 배정된 방의 수용 인원을 넘는 만큼에 시간대 길이(분)를 곱해 더합니다. 0이면 즐겨찾기 수가 "
        "방의 수용 인원을 넘는 발표가 없습니다."
    )
```

**5. Its default instance**, in the same file: `room_overflow_cost = RoomOverflowCost()`
with the other instances, that instance in `_DEFAULTS` after `speaker_makespan_cost`
(the order of `defaultConstraints()`), and both names in `__all__`.

Build and use it:

```sh
export OPTIMARU_USE_CASES=conference_scheduling
uv sync                                    # rebuilds optimaru.conference_scheduling._core
```

```python
from optimaru.conference_scheduling.constraints import RoomOverflowCost

others = [spec for spec in get_constraints() if spec.name != "room_overflow_cost"]
picks = [*others, RoomOverflowCost(weight=1)]
config = cs.SolverConfig()
config.step_count_limit = 200_000
config.random_seed = 1
schedule = cs.demo_conference()
cs.solve(schedule, config=config, constraints=picks)
print(cs.constraint_costs(schedule, picks)["room_overflow_cost"]["costs"][0])
```

On the demo conference that total is 184,410 seat-minutes. A default solve of the same
length (`constraints=None`) leaves it at 185,310. The gap is small because the demo
leaves little choice: six of its 15 talks have more favorites than any room they may
use seats.

What the base gave you: every change of the schedule marks the shared breakdown as
stale, and the next score recomputes it once, for all the components that read it.
The constraint is right after any move, and costs one comparison per talk in a loop that
runs anyway.

## 3. Check it

**A case worked out by hand**, in `tests/conference_scheduling/cpp/cs_plugin_test.cpp`,
with the other raw costs in `verifyRawCostsByHand()`. Its base has rooms of 100, 200
and 300 seats and timeslots of 60 minutes:

```cpp
    // roomOverflowCost: 250 favorites in r0 (100 seats) for the 60 minutes
    // of s0: 150 missing seats x 60. r2 (300 seats) holds them all.
    {
        ConferenceSchedule c = rawBase();
        placeTalk(c, {0}, 0, 0).favoriteCount = 250;
        checkRaw(c, {{"roomOverflowCost", 9000.0}}, "roomOverflowCost: 150 seats x 60");
        // Off by default, so the default assembly never scores it: once
        // with a weight.
        DecomposedCalculator weighted;
        weighted.emplace<RoomOverflowCost>(2.0);
        const Score score = scoreWith(weighted, c);
        check(score.hard == 0 && score.soft == -18000.0, "roomOverflowCost at weight 2: -18000");
        c.talks[0].slotRoom = c.slotRoomOf(0, 2);
        checkRaw(c, {}, "roomOverflowCost: 0 in a room that seats them");
    }
```

```sh
cmake -B build -DOPTIMARU_USE_CASES=conference_scheduling && cmake --build build -j
ctest --test-dir build --output-on-failure
```

**The catalog table.** `verifyPartialAssemblies()` in the same file has one row per
component, in catalog order, and compares it with the `Breakdown` member, the
`constraint_costs()` entry and the `info()` row. Add the row after
`SpeakerMakespanCost`, and make the table's check of "on by default"
`rows[index].default_enabled == (item.weight > 0.0)`:

```cpp
            {"RoomOverflowCost", b.roomOverflowCost, 0.0, false, false,
             [](DecomposedCalculator& calc) { calc.emplace<RoomOverflowCost>(); }},
```

**The counts.** A 38th constraint fails every check that counts or lists the catalog.
Update them:

- `cs_plugin_test.cpp`: `constraint_totals().size() == 37` in
  `verifyConstraintTotals()`; `raw.size() == 37` in `checkRaw()`, which every hand case
  goes through, so all of them fail until it is 38; `flips == 34` in `verifyGuards()`,
  the constraints that are not rewards;
- `tests/conference_scheduling/test_constraint_specs.py`: `EXPECTED_NAMES`,
  `EXPECTED_INFO` (the row is `("room_overflow_cost", "soft", 0.0, True, False, False)`)
  and `len(available) == 37`. They fail `test_list_constraints_matches_the_spec_names`,
  `test_module_level_instances_keep_every_default` and
  `test_constraint_info_reports_the_cpp_defaults`.

**Incremental against full.** Solve with every score checked against a full recompute. It raises
`RuntimeError` ("Score corruption: ...", the engine's `ScoreMismatchError`) if a move leaves the
score stale:

```python
cs.solve(schedule, time_limit_seconds=1, random_seed=1, constraints=picks, full_assert=True)
```

**The tables.** The constraint tables in [README.md](README.md) and
[python.md](python.md) must equal `cs.constraint_info()`; add the row. The docstrings
of `list_constraints()` and `constraint_info()` in `_core.pyi` state the counts, and
the notebook `examples/conference-scheduling.ipynb` prints the constraint list.

**The CP-SAT model.** `external_solver.conference_scheduling` in
[external-solver](https://github.com/optimaru/external-solver) has one plugin per
constraint: a subclass of the Python spec that builds the CP-SAT terms and reads its
defaults from `cs.constraint_info()`. Its test compares its catalog with
`cs.list_constraints()`, so a new constraint needs its plugin there, also while it is off.

## 4. Make it cheaper per move

On this assembly a move does not cost one call per constraint. It costs one recompute of
the breakdown, whatever was picked: a loop over the talks and a loop over every pair
of talks. A constraint adds its term to one of them (the table in step 2), and the
example's term is one comparison per talk. There is nothing to cut inside it.

What can change is whether a constraint needs the breakdown at all. A component may keep
its own total. On the assignment shape the engine has a base for that,
`optimaru::MemberChangeGlobalConstraint<ConferenceSchedule>`: it calls
`on_member_changed()` for every talk that joins or leaves a cell, also when a move is
rolled back. Name it as the base in the struct line and in the constructor, include
`<cstddef>`, `<cstdint>` and `<vector>`, and replace `select` by:

```cpp
    // Once per solve: the total of the schedule the solve starts from. A unit
    // is one (timeslot, room) cell, its members the talks placed there.
    void reset(const ConferenceSchedule& schedule,
               const std::vector<std::vector<int>>& members_per_unit) override {
        total_ = 0;
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            for (int talk : members_per_unit[unit])
                total_ += term(schedule, static_cast<int>(unit), talk);
        }
    }
    // Once per talk that joins or leaves a cell.
    void on_member_changed(const ConferenceSchedule& schedule, int unit,
                           const std::vector<int>& /*members*/, int talk, bool joined) override {
        total_ += (joined ? 1 : -1) * term(schedule, unit, talk);
    }
    double cost() const override { return static_cast<double>(total_); }
    // At weight 0 (the default until step 5): send it no change, see below.
    bool idle_at_weight_zero() const override { return true; }

private:
    // Reads the facts only, so it does not need ScoreCalculator::prepare().
    static std::int64_t term(const ConferenceSchedule& schedule, int unit, int talk) {
        const std::int64_t missing = schedule.talks[talk].favoriteCount -
                                     schedule.rooms[schedule.roomIndexOf(unit)].capacity;
        if (missing <= 0) return 0;
        return missing * schedule.timeslots[schedule.timeslotIndexOf(unit)].durationInMinutes();
    }

    std::int64_t total_ = 0;
```

A leave must subtract exactly what the join added; an `int64` sum of terms that read
only the facts does, and `full_assert` (step 3) checks it. `reset()` runs when a solve
starts and once more for the final score (after every score under `full_assert`), so
what it prepares is paid once. `Breakdown::roomOverflowCost` stays: it is the full
recompute the tests of step 3 compare the component with, and step 5 needs it.

A global at weight 0 is still sent every change, because what a global keeps may be read
by more than its own `cost()`. Here nothing else reads `total_` and `reset()` rebuilds
it, so the component declares `idle_at_weight_zero()`: picked at weight 0, it is sent no
change, its `cost()` is not asked for a score, and it is reset before
`constraint_costs()` reports its total.

Measured on the conference of step 6, in thousand steps per second:

| Picked | Step 2: projects the breakdown | Step 4: keeps its own total |
| --- | --- | --- |
| the 37 defaults and the constraint at weight 1 | 14.9 | 14.7 |
| the constraint alone, at weight 1 | 12.3 | 9,016 |

Alone, the constraint no longer needs the breakdown and runs 700 times faster. Next to one
projection it gains nothing, because the breakdown is recomputed anyway; the
difference in the first row is inside the spread of its three runs (14.3 to 14.9).
Both forms end on the same score for the same seed and steps, as they must.

So this form pays only in a pick list that holds no projection. For a constraint picked
together with the defaults, the form of step 2 is as cheap as this assembly gets, and
it has one definition of the raw cost instead of two. What makes such a constraint cheap
per move is step 5.

## 5. Put it on the default path

Do this when the constraint should be on in every solve (`kDefaultWeight > 0`). Then
`cs.solve(schedule)` must score it, and `cs.solve(schedule)` scores with the
monolithic calculators: 60 times the steps per second of the assembly, and the only
path that runs parallel starts and restarts.

`cs_plugin_test` compares the default assembly with `ScoreCalculator::score()`, and
`cs_test` compares the incremental calculator with it. So from the moment the weight
is not 0 one of the two fails, until all three agree.

**1. The weight.** In the component: `static constexpr double kDefaultWeight = 1.0;`.
It must be a whole number, because the monolithic calculators add in `int64`:
`kSoftWeight<>` and `kHardWeight<>` in `common.hpp` do not compile otherwise, nor
when the constraint is on another level than the one it is summed into.

**2. The full recompute**, `cpp/src/score_calculator.cpp`. The term is there since
step 2; give it its weight at the end of `Breakdown::softPenaltyTotal()` (a hard constraint
goes into `hardTotal()`, a reward into `softRewardTotal()`):

```cpp
           kSoftWeight<SpeakerMakespanCost> * speakerMakespanCost +
           kSoftWeight<RoomOverflowCost> * roomOverflowCost;
```

**3. The incremental calculator**, `cpp/src/incremental_calculator.cpp`. Read the
weight off the component with the others at the top, and add the term to
`addUnaryContribution()`, which `insert()` and `retract()` call for the talk that
moves:

```cpp
constexpr std::int64_t kOverflowWeight = kSoftWeight<RoomOverflowCost>;
```

```cpp
    std::int64_t softUnits = kSoftTagWeight * (softSlotCnt_[slotAt] + softRoomCnt_[roomAt]) * dur;
    // roomOverflowCost: the favorites that do not fit the room.
    const std::int64_t overflow =
            schedule_->talks[talkIndex].favoriteCount - schedule_->rooms[room].capacity;
    if (overflow > 0) softUnits += kOverflowWeight * overflow * dur;
```

That is all for a constraint of one talk. The default path ran at 809 thousand steps per
second with the 38th constraint in it; five runs without it gave 814 to 833 thousand.

A constraint over pairs is more work. `pairTotals()` has to answer what one talk adds
against all the others without visiting them, from counters per timeslot that
`applyAggregates()` keeps (`contentConflictCost` reads `contentCnt_`); only the
speaker and prerequisite constraints walk short lists of talks. A constraint that counts per
timeslot or per speaker keeps a counter and a running total, as
`crowdControlRiskCost` (`applyCrowdDelta()`) and `speakerMakespanCost`
(`makespanUnitsOf()`) do.

**4. What else moves.** With the weight at 1 and the three calculators in agreement,
two things still failed: the row of step 3 in `verifyPartialAssemblies()` and in
`EXPECTED_INFO`, which states the weight 0 and "off by default", and
`test_weight_none_means_the_component_default`, whose list of every default with its
weight lacks the constraint. No hand-computed score failed, because no fixture gives a talk
more favorites than its room seats; a constraint that the fixtures violate changes those
scores too. Beyond the tests:

- `verifyConstraints()` in `tests/conference_scheduling/cpp/cs_test.cpp` works out
  `Breakdown` members and default scores by hand; a default constraint gets its case there;
- the tables and the counts ("37", "17 hard + 20 soft") in [README.md](README.md),
  [python.md](python.md), `constraints.py`, `__init__.py`, `_core.pyi`,
  `score_calculator.hpp`, `constraints.hpp` and `common.hpp`;
- `printBreakdown()` in `benchmarks/conference-scheduling/cpp/cs_quality_bench.cpp`
  lists every `Breakdown` member; add the row, or step 6 does not show the constraint;
- the CP-SAT plugin of step 3 now counts in every comparison of
  [`benchmarks/conference-scheduling`](../../../benchmarks/conference-scheduling): its
  published results change, and so does `CHANGELOG.md`.

## 6. Measure

**Steps per second.** A seeded solve with a step limit on each path. The demo has 15
talks, too few to show what a recompute over every pair costs, so the script builds a
conference of three days in the demo's shape:

```python
import random
import time

import optimaru.conference_scheduling as cs
from optimaru.conference_scheduling.constraints import get_constraints

DAY = ((615, 735, 0), (615, 660, 1), (690, 735, 1), (780, 900, 0), (930, 975, 1), (990, 1035, 1))


def conference(days=3, seed=1):
    """A seeded conference in the demo's shape. Three days: 70 talks, 18 timeslots, 8 rooms."""
    rng = random.Random(seed)
    timeslots = [
        cs.Timeslot(f"T{day * 6 + i + 1}", day * 1440 + start, day * 1440 + end, [kind])
        for day in range(days)
        for i, (start, end, kind) in enumerate(DAY)
    ]
    rooms = [cs.Room(f"R{i + 1}", f"Room {i + 1}", seats, [1]) for i, seats in enumerate((60, 240, 630, 70, 120, 300))]
    rooms += [cs.Room(f"R{i + 7}", f"Lab {i + 1}", 490, [0]) for i in range(2)]
    labs = days * 4
    count = labs + round(0.8 * days * 24)
    speakers = [cs.Speaker(str(k + 1), f"Speaker {k + 1}") for k in range(round(0.8 * count))]
    talks = [
        cs.Talk(
            code=f"S{i + 1:03d}",
            talk_type=0 if i < labs else 1,
            speakers=rng.sample(range(len(speakers)), 2 if rng.random() < 0.4 else 1),
            theme_track_tags=[rng.choice(("Cloud", "AI", "Optimization"))],
            sector_tags=[rng.choice(("Orange", "Blue", "Green"))],
            audience_types=[rng.choice(("Analysts", "Managers", "Programmers"))],
            audience_level=rng.randint(1, 3),
            content_tags=[rng.choice(("Constraints", "Metaheuristics", "Kubernetes", "Optimization"))],
            favorite_count=rng.randint(0, 1000),
            crowd_control_risk=int(rng.random() < 0.08),
        )
        for i in range(count)
    ]
    return cs.ConferenceSchedule("Conference", ["Lab", "Breakout"], timeslots, rooms, speakers, talks)


def solve(steps, picked):
    schedule = conference()
    config = cs.SolverConfig()
    config.step_count_limit = steps
    config.spent_limit_seconds = 600
    config.random_seed = 1
    start = time.perf_counter()
    cs.solve(schedule, config=config, constraints=picked)
    return time.perf_counter() - start, schedule.score


for label, picked, steps in (("default path", None, 2_000_000), ("assembly", get_constraints(), 60_000)):
    start_up, _ = solve(1, picked)  # the tables and the construction heuristic
    seconds, score = solve(steps, picked)
    print(f"{label:14s} {steps / (seconds - start_up) / 1e3:7.1f} thousand steps/s  {score}")
print(solve(60_000, None)[1], solve(60_000, get_constraints())[1])
```

The last line prints the same score twice: same seed, same steps, same scoring. If
the two differ after a change, the two paths no longer score alike. (With a step
limit the default path makes one run from one start, like the assembly.)

| Measured on that conference, median of three runs | Steps per second |
| --- | --- |
| default path, 37 constraints | 833 thousand |
| assembly, 37 constraints | 13.6 thousand |
| assembly, 37 + the constraint at weight 1 (step 2, step 4) | 14.9 thousand, 14.7 thousand |
| default path, 38 constraints (step 5) | 809 thousand |

A step of the assembly costs more on some schedules than on others (the pair loop,
and the ruin-and-recreate moves, which score many candidates in one step), and
another objective visits other schedules. That is why the constraint at weight 1 looks
faster than the 37 alone. Compare two forms of one constraint on the same picks, as step 4
does.

**Quality at a fixed time.** A constraint changes what the search can reach, so compare
scores at the same budget over several seeds, before and after:

```sh
cmake --build build --target cs_quality_bench
./build/conference_scheduling/cs_quality_bench scaled:60 1 10     # instance, seed, seconds
```

`cs_quality_bench` runs the default path only, so it measures a constraint after step 5.
It prints the final hard and soft score, every non-zero `Breakdown` member and a
moves-per-second estimate; `scaled:N` is a conference of N talks in 14 timeslots and
9 rooms. Build the tree before and after the change into two build directories and
run the two binaries in turn on the same instances and seeds.
[Quality benchmark and solver defaults](README.md#quality-benchmark-and-solver-defaults)
shows what such a comparison reports. Against CP-SAT:
`uv run conference-scheduling-compare --seconds 10 --workers 8` in
`benchmarks/conference-scheduling/cpsat`
([README](../../../benchmarks/conference-scheduling/README.md)).

## What each step touched

| File | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- |
| `cpp/include/cs/score_calculator.hpp`, `cpp/src/score_calculator.cpp` | member, term | | | the weighted total |
| `cpp/include/cs/constraints/<subject>_cost.hpp` | new | | own total | weight |
| `cpp/include/cs/constraints.hpp` | include, `emplace` | | | |
| `constraints.py` | spec, instance, `_DEFAULTS`, `__all__` | | | counts in the text |
| `tests/conference_scheduling/cpp/cs_plugin_test.cpp` | | case, row, counts | | the row's weight |
| `tests/conference_scheduling/test_constraint_specs.py` | | lists | | the row, the explicit defaults |
| `docs/use-cases/conference-scheduling/README.md`, `python.md` | | table row | | defaults, counts |
| `cpp/src/incremental_calculator.cpp` | | | | the term |
| `tests/conference_scheduling/cpp/cs_test.cpp` | | | | a case by hand |
| `benchmarks/conference-scheduling/cpp/cs_quality_bench.cpp` | | | | breakdown row |
| external-solver, `benchmarks/conference-scheduling` | | the plugin | | results |

Moves and the construction heuristic are a different subject: see
[Extending](README.md#extending) in the guide.
