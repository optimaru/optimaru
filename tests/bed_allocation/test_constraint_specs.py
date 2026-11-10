"""optimaru.bed_allocation.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py` and the C++
tests (one hand-calculated positive and zero case per constraint). This file covers the
selection path that picks "which constraints are on", the catalog (`constraint_info()`),
the five knobs, repeated picks and the medium level.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import optimaru.bed_allocation as ba
from optimaru._constraint import instance_keys
from optimaru.bed_allocation import constraints as specs
from optimaru.bed_allocation.constraints import (
    BedConflictCost,
    Constraint,
    DepartmentSpecialtyCost,
    Level,
    MixedGenderRoomCost,
    PreferredMaxRoomCapacityCost,
    PreferredPatientEquipmentCost,
    UnassignedStaysCost,
    bed_conflict_cost,
    get_constraints,
    preferred_max_room_capacity_cost,
    score,
    unassigned_stays_cost,
)

MEDIUM_UNIT = 1_000_000_000  # the domain's medium fold (into soft)


def make_stay(index, bed=-1):
    return ba.Stay(
        f"s{index}",
        f"patient-{index}",
        ba.Gender.MALE,
        30,
        0,
        5,  # 6 nights
        "default",
        patient_preferred_maximum_room_capacity=1,
        bed=bed,
    )


def build_plan():
    """Only capacity-2 rooms, so a preferred-capacity (1) violation always remains, and
    more stays (4) than beds (3), so a "no bed" medium cost always remains."""
    return ba.BedPlan(
        [ba.Department("d0", "Dept", {"default": 1})],
        [ba.Room("r0", "r0", capacity=2), ba.Room("r1", "r1", capacity=2)],
        [ba.Bed("b0", 0), ba.Bed("b1", 0), ba.Bed("b2", 1)],
        [make_stay(i) for i in range(4)],
    )


def build_crowded_plan():
    """Two overlapping stays for one bed — forces a choice between sharing (hard) and
    unassigned (medium). Preferred capacity is left empty to remove soft noise."""
    return ba.BedPlan(
        [ba.Department("d0", "Dept", {"default": 1})],
        [ba.Room("r0", "r0", capacity=1)],
        [ba.Bed("b0", 0)],
        [ba.Stay(f"s{i}", f"patient-{i}", ba.Gender.MALE, 30, 0, 5, "default") for i in range(2)],
    )


def solve(constraints=None):
    plan = build_plan()
    ba.solve(plan, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return plan.score


def solve_crowded(constraints=None):
    plan = build_crowded_plan()
    ba.solve(plan, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return plan.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, global constraints after — the global
    # mixed_gender_room_cost goes last.
    available = ba.list_constraints()
    assert available == [
        "bed_conflict_cost",
        "female_in_male_room_cost",
        "male_in_female_room_cost",
        "department_min_age_cost",
        "department_max_age_cost",
        "required_patient_equipment_cost",
        "unassigned_stays_cost",
        "preferred_max_room_capacity_cost",
        "department_specialty_cost",
        "department_specialty_priority_cost",
        "preferred_patient_equipment_cost",
        "mixed_gender_room_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ba.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.medium == explicit.medium
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the medium/soft terms disappear entirely."""
    baseline = solve()
    assert baseline.medium < 0 and baseline.soft < 0

    hard_only = solve([bed_conflict_cost])
    assert hard_only.medium == 0
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the preferred-capacity weight grows soft only; hard stays put.

    `unassigned_stays_cost` must be on too — otherwise unassigned is free, the
    solver leaves everyone out of a bed and the soft term itself vanishes."""
    picks = [bed_conflict_cost, unassigned_stays_cost]
    base = solve([*picks, preferred_max_room_capacity_cost])
    heavier = solve([*picks, PreferredMaxRoomCapacityCost(weight=80)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft flips the solver's choice.

    Two overlapping stays for one bed: sharing the bed is bed_conflict_cost,
    leaving one out is unassigned_stays_cost. With the former hard, unassigned
    is cheaper (levels take priority); demoted to soft, sharing becomes cheaper."""
    plan_picks = [unassigned_stays_cost]

    hard_default = solve_crowded([bed_conflict_cost, *plan_picks])
    assert hard_default.medium < 0  # leaves one out instead of sharing
    assert hard_default.soft == 0

    flipped = solve_crowded([BedConflictCost(level="soft"), *plan_picks])
    assert flipped.medium == 0  # sharing is cheaper now
    assert flipped.soft < 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "bed_conflict_costs"  # trailing s
        weight = None
        level = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        ba.solve(build_plan(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    for spec in get_constraints():
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0), spec.name
        assert getattr(specs, spec.name) is spec  # the instance is named after the constraint


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        for attribute in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(type(spec), attribute).strip(), f"{spec.name}.{attribute}"


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    plan = build_plan()
    ba.solve(plan, time_limit_seconds=0.3, random_seed=1)
    return ba.constraint_costs(plan), plan


def test_score_reproduces_the_engine_hard_level():
    """The hard level of the Python score at default specs must match the engine."""
    raw, solution = _raw()
    assert score(raw).hard == ba.calculate_score(solution).hard


def test_score_is_additive_over_the_picked_constraints():
    """The total score is the sum of per-constraint scores — unlisted terms simply drop out.

    Hard rounding happens per unit contribution, so splitting by constraint and
    summing gives the same value. "How much does this term add to the score"
    can be read without rescoring."""
    raw, _ = _raw()
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_reweighting_is_linear_without_rescoring():
    """Changing only the weight scales that term linearly — no rescoring."""
    raw, _ = _raw()
    base = score(raw, [bed_conflict_cost])
    ten = score(raw, [BedConflictCost(weight=10)])
    default_weight = raw[bed_conflict_cost.name]["weight"]
    factor = 10 / default_weight
    if base.hard:
        assert ten.hard == pytest.approx(base.hard * factor, rel=1e-6)
    else:
        assert ten.soft == pytest.approx(base.soft * factor, rel=1e-9)


def test_score_rejects_an_unknown_constraint_name():
    """A spec with a wrong name (e.g. a misspelled component) must not be silently ignored."""
    raw, _ = _raw()

    class Misnamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Misnamed()])


# --- The catalog: constraint_info() -------------------------------------------

# (name, default level, default weight), in list_constraints() order.
EXPECTED_INFO = [
    ("bed_conflict_cost", "hard", 1000.0),
    ("female_in_male_room_cost", "hard", 50.0),
    ("male_in_female_room_cost", "hard", 50.0),
    ("department_min_age_cost", "hard", 100.0),
    ("department_max_age_cost", "hard", 100.0),
    ("required_patient_equipment_cost", "hard", 50.0),
    ("unassigned_stays_cost", "medium", 1.0),
    ("preferred_max_room_capacity_cost", "soft", 8.0),
    ("department_specialty_cost", "soft", 10.0),
    ("department_specialty_priority_cost", "soft", 10.0),
    ("preferred_patient_equipment_cost", "soft", 50.0),
    ("mixed_gender_room_cost", "hard", 1000.0),
]


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = ba.constraint_info()
    assert [row.name for row in rows] == ba.list_constraints()
    assert [(row.name, row.default_level, row.default_weight) for row in rows] == EXPECTED_INFO
    # Every constraint is flippable, none is a reward, and all twelve are on by default.
    assert all(row.level_flippable and not row.reward and row.default_enabled for row in rows)


def test_constraint_info_rows_are_plain_values():
    row = ba.constraint_info()[6]
    assert isinstance(row, ba.ConstraintInfo)
    assert row._asdict() == {
        "name": "unassigned_stays_cost",
        "default_level": "medium",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='unassigned_stays_cost'")
    assert pickle.loads(pickle.dumps(ba.constraint_info())) == ba.constraint_info()
    assert ba.ConstraintInfo.__module__ == ba._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults and carries the domain's medium fold on every entry."""
    raw = ba.constraint_costs(build_plan())
    assert list(raw) == ba.list_constraints()
    for row in ba.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"]) == (
            row.default_weight,
            row.default_level,
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] == (row.name == "mixed_gender_room_cost")
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert entry["medium_fold"] == {"kind": "into_soft", "unit": float(MEDIUM_UNIT)}


# --- The knobs ------------------------------------------------------------------


def _engine(score_):
    """The engine-level (hard, soft) of a HardMediumSoft plan score (medium folded into soft)."""
    return score_.hard, score_.medium * MEDIUM_UNIT + score_.soft


def _one_stay_without_a_bed():
    """build_plan() with s0..s2 in the three beds and s3 (6 nights) on the "no bed" sentinel:
    6 medium nights, and every stay prefers a single room (6 x 8 soft each in a real bed)."""
    plan = build_plan()
    plan.set_assignment([0, 1, 2, plan.no_bed_value()])
    return plan


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit C++ default weights."""
    explicit = [type(spec)(weight=weight) for spec, (_, _, weight) in zip(get_constraints(), EXPECTED_INFO)]
    plan = _one_stay_without_a_bed()
    raw = ba.constraint_costs(plan)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == _engine(ba.calculate_score(plan))


def test_the_medium_level_folds_into_soft():
    plan = _one_stay_without_a_bed()
    raw = ba.constraint_costs(plan)
    assert raw["unassigned_stays_cost"]["costs"] == [0.0, 0.0, 0.0, 6.0]  # three beds, then the sentinel
    monolithic = ba.calculate_score(plan)
    assert (monolithic.hard, monolithic.medium, monolithic.soft) == (0, -6, -144)
    assert score(raw) == (0, monolithic.soft - 6 * MEDIUM_UNIT)
    others = [spec for spec in get_constraints() if spec.name != "unassigned_stays_cost"]
    assert score(raw, [*others, UnassignedStaysCost(level="hard")]) == (-6, monolithic.soft)
    assert score(raw, [*others, UnassignedStaysCost(level="soft")]) == (0, monolithic.soft - 6)
    assert score(raw, [*others, UnassignedStaysCost(weight=3)]) == (0, monolithic.soft - 18 * MEDIUM_UNIT)


def test_a_demoted_medium_constraint_is_soft_in_the_engine_too():
    """level="soft" moves unassigned_stays_cost to soft in the solver.

    Two overlapping 6-night stays for one bed: sharing costs 6000 hard, so one stay goes
    without a bed, and its nights now cost soft, not medium."""
    picks = [bed_conflict_cost, UnassignedStaysCost(level="soft")]
    plan = build_crowded_plan()
    ba.solve(plan, time_limit_seconds=0.4, random_seed=1, constraints=picks)
    assert (plan.score.hard, plan.score.medium, plan.score.soft) == (0, 0, -6)
    assert score(ba.constraint_costs(plan), picks) == (plan.score.hard, plan.score.soft)


def test_a_promoted_medium_constraint_is_one_hard_unit_per_night():
    """level="hard": unassigned nights cost 1 hard each (not 10^9), still cheaper than sharing a bed."""
    picks = [bed_conflict_cost, UnassignedStaysCost(level="hard")]
    plan = build_crowded_plan()
    ba.solve(plan, time_limit_seconds=0.4, random_seed=1, constraints=picks)
    assert (plan.score.hard, plan.score.medium, plan.score.soft) == (-6, 0, 0)
    assert score(ba.constraint_costs(plan), picks) == (plan.score.hard, plan.score.soft)


# --- on_new_best ------------------------------------------------------------------

#: A soft total of 10^9 or more: one stay (6 nights) without a bed is 6 medium, and each
#: of the other three spends 6 nights in a room above its preferred capacity at 10^8 a
#: night, so soft is -1.8 * 10^9. Decoding the folded engine score (medium * 10^9 + soft)
#: would report that as medium -7, soft -8 * 10^8.
_HEAVY_SOFT = [bed_conflict_cost, unassigned_stays_cost, PreferredMaxRoomCapacityCost(weight=10**8)]


@pytest.mark.parametrize("constraints", [None, _HEAVY_SOFT], ids=["default", "heavy-soft"])
def test_on_new_best_reports_the_levels_of_the_final_score(constraints):
    """on_new_best and the final plan.score use the same three levels on both solve paths,
    so the last reported best is exactly the returned score."""
    events = []
    plan = build_plan()
    ba.solve(
        plan,
        time_limit_seconds=0.4,
        random_seed=1,
        constraints=constraints,
        on_new_best=lambda step, s: events.append((s.hard, s.medium, s.soft)),
    )
    final = (plan.score.hard, plan.score.medium, plan.score.soft)
    assert events
    assert events[-1] == final
    if constraints is not None:
        assert final == (0, -6, -18 * 10**8)
        assert score(ba.constraint_costs(plan), constraints) == (0, -6 * MEDIUM_UNIT - 18 * 10**8)


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"hard": 1}, TypeError),
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        BedConflictCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="bed_conflict_cost", weight=-2.0, level=None, exponent=1.0)
    flat = SimpleNamespace(name="bed_conflict_cost", weight=None, level=None, exponent=0.0)
    unbounded = SimpleNamespace(name="bed_conflict_cost", weight=None, level=None, exponent=1.0, offset=float("inf"))
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
        (unbounded, "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            ba.solve(build_plan(), time_limit_seconds=0.1, constraints=[pick])
        with pytest.raises(ValueError, match=message):
            ba.constraint_costs(build_plan(), [pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        BedConflictCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="unassigned_stays_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'unassigned_stays_cost': {re.escape(message)}"):
        ba.solve(build_plan(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        UnassignedStaysCost(**knobs)


# --- repeated picks and the offset / clip knobs ------------------------------------
#
# The hand instance is _hand_plan(): s0 in b0, s1 in b1 (6 nights each, both rooms above
# the preferred capacity 1), b2 empty, s2 and s3 (12 nights) on the "no bed" sentinel.
# Raw per unit (b0, b1, b2, sentinel): preferred_max_room_capacity_cost [6, 6, 0, 0],
# unassigned_stays_cost [0, 0, 0, 12], every other unit constraint [0, 0, 0, 0] and the
# global mixed_gender_room_cost 0. The per-bed constraints evaluate the real beds b0..b2
# (an empty one included), never the sentinel; unassigned_stays_cost evaluates the
# sentinel only.


def _hand_plan():
    plan = build_plan()
    plan.set_assignment([0, 1, plan.no_bed_value(), plan.no_bed_value()])
    return plan


def _zero_budget(plan, picks):
    """The engine's score of `plan` under `picks`: a solve with no time keeps the assignment."""
    before = plan.assignment()
    config = ba.SolverConfig()
    config.spent_limit_seconds = 0
    ba.Solver(config).solve_with_constraints(plan, picks)
    assert plan.assignment() == before
    return _engine(plan.score)


REPEAT = [PreferredMaxRoomCapacityCost(), PreferredMaxRoomCapacityCost(weight=0.5, exponent=2)]


def test_naming_a_constraint_twice_scores_both_picks():
    """A constraint may be picked several times: each pick is scored on its own.

    The later pick does not win in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [BedConflictCost(), UnassignedStaysCost(), PreferredMaxRoomCapacityCost(weight=10), *REPEAT[:1]]
    assert _zero_budget(_hand_plan(), picks) == (0, -12 * MEDIUM_UNIT - (10 + 8) * 12)
    assert get_constraints([BedConflictCost(), BedConflictCost()]) == [BedConflictCost(), BedConflictCost()]
    plan = build_plan()
    ba.solve(plan, time_limit_seconds=0.2, random_seed=1, constraints=[*picks, BedConflictCost(weight=1)])
    assert score(ba.constraint_costs(plan), [*picks, BedConflictCost(weight=1)]) == _engine(plan.score)


def test_a_repeated_pick_by_hand():
    """8 x nights plus 0.5 x nights^2 per bed: 8 x 12 + 0.5 x 2 x 6^2 = 132."""
    raw = ba.constraint_costs(_hand_plan(), REPEAT)
    assert list(raw) == ["preferred_max_room_capacity_cost", "preferred_max_room_capacity_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [6.0, 6.0, 0.0, 0.0]
    assert first["units"] == second["units"] == [0, 1, 2]  # the real beds, not the sentinel
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (8.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "preferred_max_room_capacity_cost"
    assert second["default_weight"] == 8.0
    assert second["medium_fold"] == {"kind": "into_soft", "unit": float(MEDIUM_UNIT)}
    expected = (0, -(8 * 12 + 0.5 * 2 * 6**2))
    assert expected == (0, -132.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(ba.constraint_costs(_hand_plan()), REPEAT) == expected
    assert _zero_budget(_hand_plan(), REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [PreferredMaxRoomCapacityCost(), PreferredMaxRoomCapacityCost()]
    assert score(ba.constraint_costs(_hand_plan()), picks) == (0, -192.0)
    assert _zero_budget(_hand_plan(), picks) == (0, -192.0)
    assert list(ba.constraint_costs(_hand_plan(), picks)) == [
        "preferred_max_room_capacity_cost",
        "preferred_max_room_capacity_cost#2",
    ]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        PreferredMaxRoomCapacityCost(weight=1),
        MixedGenderRoomCost(),
        PreferredMaxRoomCapacityCost(weight=2),
        UnassignedStaysCost(),
        PreferredMaxRoomCapacityCost(weight=3),
    ]
    raw = ba.constraint_costs(_hand_plan(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "unassigned_stays_cost": 1.0,
        "preferred_max_room_capacity_cost": 1.0,
        "preferred_max_room_capacity_cost#2": 2.0,
        "preferred_max_room_capacity_cost#3": 3.0,
        "mixed_gender_room_cost": 1000.0,
    }
    assert [key for key in raw if key.startswith("preferred")] == [
        "preferred_max_room_capacity_cost",
        "preferred_max_room_capacity_cost#2",
        "preferred_max_room_capacity_cost#3",
    ]
    expected = (0, -12 * MEDIUM_UNIT - 6 * 12)
    assert score(raw, picks) == expected
    assert _zero_budget(_hand_plan(), picks) == expected


def test_a_repeated_medium_pick_adds_up():
    """Both instances stay on the medium level (folded at 10^9); a promoted repeat goes hard."""
    picks = [UnassignedStaysCost(), UnassignedStaysCost(weight=2)]
    assert score(ba.constraint_costs(_hand_plan()), picks) == (0, -36 * MEDIUM_UNIT)
    plan = _hand_plan()
    _zero_budget(plan, picks)
    assert (plan.score.hard, plan.score.medium, plan.score.soft) == (0, -36, 0)
    mixed = [UnassignedStaysCost(), UnassignedStaysCost(level="hard"), UnassignedStaysCost(level="soft", offset=2)]
    assert score(ba.constraint_costs(_hand_plan(), mixed), mixed) == (-12, -12 * MEDIUM_UNIT - 10)
    plan = _hand_plan()
    _zero_budget(plan, mixed)
    assert (plan.score.hard, plan.score.medium, plan.score.soft) == (-12, -12, -10)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Per real bed: b0, b1 at 6 nights, b2 empty (raw 0, evaluated, so charged in literal mode).
        (PreferredMaxRoomCapacityCost(offset=2), (0, -8 * 2 * 4)),
        (PreferredMaxRoomCapacityCost(offset=2, clip=False), (0, -8 * (2 * 4 - 2))),  # the empty bed is rewarded
        (PreferredMaxRoomCapacityCost(offset=10), (0, 0.0)),
        (PreferredMaxRoomCapacityCost(offset=10, clip=False), (0, -8 * (2 * -4 - 10))),
        (PreferredMaxRoomCapacityCost(offset=3, exponent=2), (0, -8 * 2 * 3**2)),
        (PreferredMaxRoomCapacityCost(offset=7, exponent=3, clip=False), (0, -8 * (2 * -1 - 7**3))),
        (PreferredMaxRoomCapacityCost(offset=-1), (0, -8 * 2 * 7)),  # a negative offset adds to every non-zero bed
        (PreferredMaxRoomCapacityCost(offset=-1, clip=False), (0, -8 * (2 * 7 + 1))),  # ...or to every bed
        # The literal formula with an offset is refused on hard (bed_conflict_cost's default);
        # on soft it charges every real bed: 3 beds at (0 - 1) x 1000, a reward, not rounded.
        (BedConflictCost(level="soft", offset=1, clip=False), (0, 3000.0)),
        (BedConflictCost(level="soft", weight=4, offset=0.5, clip=False), (0, 6.0)),  # -2 on each of the 3 beds
        (BedConflictCost(weight=3, offset=0.5), (0, 0.0)),  # the threshold stays on hard: no conflict, 0
        # The scoped medium constraint evaluates the sentinel only: (12 - 20), not 4 x (0 - 20).
        (UnassignedStaysCost(offset=2), (0, -10 * MEDIUM_UNIT)),
        (UnassignedStaysCost(offset=20, clip=False), (0, 8 * MEDIUM_UNIT)),
        (UnassignedStaysCost(offset=20, clip=False, level="soft"), (0, 8.0)),
        (UnassignedStaysCost(offset=10, level="hard"), (-2, 0.0)),  # hard takes the threshold: 12 - 10
        (UnassignedStaysCost(offset=-1, level="soft"), (0, -13.0)),
        # The global's single total (0 here) is shaped once: (0 - 2) x 1000, not per unit, on
        # soft (its default hard refuses the literal formula with an offset).
        (MixedGenderRoomCost(level="soft", offset=2, clip=False), (0, 2000.0)),
        (MixedGenderRoomCost(offset=2), (0, 0.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(ba.constraint_costs(_hand_plan()), [pick]) == expected
    raw = ba.constraint_costs(_hand_plan(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _zero_budget(_hand_plan(), [pick]) == expected


def _mixed_room():
    """One SAME_GENDER room with three beds: a man (6 nights) and two women (6 and 3 nights).

    The global mixed_gender_room_cost totals the opposite-gender pairs: 6 + 3 = 9 nights."""
    stays = [
        ba.Stay("m", "m", ba.Gender.MALE, 30, 0, 5, "default", bed=0),
        ba.Stay("f1", "f1", ba.Gender.FEMALE, 30, 0, 5, "default", bed=1),
        ba.Stay("f2", "f2", ba.Gender.FEMALE, 30, 3, 5, "default", bed=2),
    ]
    return ba.BedPlan(
        [ba.Department("d0", "Dept", {"default": 1})],
        [ba.Room("r0", "r0", capacity=3, gender_limitation=ba.GenderLimitation.SAME_GENDER)],
        [ba.Bed("b0", 0), ba.Bed("b1", 0, 1), ba.Bed("b2", 0, 2)],
        stays,
    )


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        (MixedGenderRoomCost(), (-9000, 0.0)),
        (MixedGenderRoomCost(offset=4, exponent=2), (-25000, 0.0)),  # (9 - 4)^2, the total shaped once
        (MixedGenderRoomCost(level="soft", offset=12, exponent=3, clip=False), (0, 27000.0)),  # (9 - 12)^3 = -27
        (MixedGenderRoomCost(weight=1, offset=-1, level="soft"), (0, -10.0)),
    ],
)
def test_the_global_total_is_shaped_once(pick, expected):
    raw = ba.constraint_costs(_mixed_room(), [pick])
    assert raw["mixed_gender_room_cost"]["costs"] == [9.0]
    assert raw["mixed_gender_room_cost"]["global"] is True
    assert "units" not in raw["mixed_gender_room_cost"]
    assert score(raw, [pick]) == expected
    assert _zero_budget(_mixed_room(), [pick]) == expected


def test_the_scopes_report_their_units():
    """Every per-bed entry lists the real beds as its "units", unassigned_stays_cost the sentinel,
    and the global none. On a solution with every stay in a bed (b2 shared), a literal offset
    charges exactly those units: -3 medium on the sentinel, 3 x 50 on the beds (on soft: the
    hard default refuses the literal formula with an offset)."""
    plan = build_plan()
    plan.set_assignment([0, 1, 2, 2])  # nobody without a bed
    raw = ba.constraint_costs(plan)
    for key, entry in raw.items():
        if key == "mixed_gender_room_cost":
            assert "units" not in entry
        elif key == "unassigned_stays_cost":
            assert entry["units"] == [plan.no_bed_value()] == [3]
        else:
            assert entry["units"] == [0, 1, 2], key
        assert len(entry["costs"]) == (1 if entry["global"] else 4)
    assert raw["unassigned_stays_cost"]["costs"] == [0.0, 0.0, 0.0, 0.0]
    assert raw["male_in_female_room_cost"]["costs"] == [0.0, 0.0, 0.0, 0.0]
    for pick, expected in (
        (UnassignedStaysCost(offset=3, clip=False), (0, 3 * MEDIUM_UNIT)),
        (specs.MaleInFemaleRoomCost(level="soft", offset=1, clip=False), (0, 150.0)),  # not 4 x 50
    ):
        assert score(raw, [pick]) == expected
        copy = build_plan()
        copy.set_assignment([0, 1, 2, 2])
        assert _zero_budget(copy, [pick]) == expected


def test_offset_zero_reproduces_the_plain_scores():
    """offset 0 is the plain shaping in both modes, so a selection scores the same with or without it."""
    plain = [*get_constraints()[:-1], MixedGenderRoomCost(exponent=2)]
    shifted = [type(spec)(exponent=spec.exponent, offset=0.0, clip=False) for spec in plain]
    raw, solved = _raw()
    assert score(raw, shifted) == score(raw, plain)
    engines = []
    for picks in (plain, shifted):
        copy = build_plan()
        copy.set_assignment(solved.assignment())
        engines.append(_zero_budget(copy, picks))
    assert engines[0] == engines[1] == score(raw, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


SELECTIONS = {
    "repeat": [*get_constraints(), *REPEAT, UnassignedStaysCost(weight=2, exponent=2)],
    "repeat-and-offsets": [
        BedConflictCost(),
        BedConflictCost(level="soft", weight=1, offset=1, clip=False),  # hard refuses the literal formula
        BedConflictCost(weight=1, offset=1),
        UnassignedStaysCost(),
        UnassignedStaysCost(offset=4, exponent=2),
        UnassignedStaysCost(offset=-2, clip=False, level="soft", weight=3),
        PreferredMaxRoomCapacityCost(offset=3),
        PreferredMaxRoomCapacityCost(offset=5, clip=False, weight=0.25),
        DepartmentSpecialtyCost(offset=-1),
        MixedGenderRoomCost(),
        MixedGenderRoomCost(offset=2, clip=False, level="soft", weight=1),
    ],
}


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_score_matches_the_engine_for_repeats_and_offsets(picks):
    raw, solved = _raw()
    copy = build_plan()
    copy.set_assignment(solved.assignment())
    engine = _zero_budget(copy, picks)
    python = score(ba.constraint_costs(solved, picks), picks)
    assert python.hard == engine[0]
    assert python.soft == pytest.approx(engine[1])
    assert score(raw, picks) == python  # the default costs score the repeats too


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """ScoreChecks::Full re-checks the incremental score against a full recompute after every move."""
    from optimaru.bed_allocation.demo import generate_demo_data

    plan = generate_demo_data().to_core()
    config = ba.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 3000
    config.spent_limit_seconds = 30
    ba.Solver(config).solve_with_constraints(plan, picks)
    raw = ba.constraint_costs(plan, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    engine = _engine(plan.score)
    assert python.hard == engine[0]
    assert python.soft == pytest.approx(engine[1])
    assert score(ba.constraint_costs(plan), picks) == python


# --- Levels: each pick, a repeat included, on the level it names --------------------


def test_a_soft_constraint_can_move_to_medium():
    """On medium at its default weight 8, a night above the preferred capacity outranks a
    night without a bed (1 medium), so the solver leaves all four stays without a bed."""
    picks = [bed_conflict_cost, unassigned_stays_cost]
    soft_level = solve([*picks, preferred_max_room_capacity_cost])
    medium_level = solve([*picks, PreferredMaxRoomCapacityCost(level=Level.MEDIUM)])
    assert (soft_level.hard, soft_level.medium, soft_level.soft) == (0, -6, -144)
    assert (medium_level.hard, medium_level.medium, medium_level.soft) == (0, -24, 0)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        BedConflictCost(level=True)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft'"):
        BedConflictCost(level="firm")
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft'"):
        BedConflictCost(level="HARD")


def test_core_refuses_an_unknown_level_like_the_python_base():
    pick = SimpleNamespace(name="bed_conflict_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        ba.solve(build_plan(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        ba.constraint_costs(build_plan(), [pick])


def test_a_pick_without_level_is_refused():
    """A duck-typed pick without `level` is refused on every path instead of running at the default level."""
    pick = SimpleNamespace(name="bed_conflict_cost", weight=None, exponent=1.0)
    with pytest.raises(AttributeError, match="level"):
        ba.solve(build_plan(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(AttributeError, match="level"):
        ba.constraint_costs(build_plan(), [pick])
    with pytest.raises(AttributeError, match="level"):
        score(ba.constraint_costs(build_plan()), [pick])


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        # _hand_plan(): preferred capacity [6, 6, 0, 0] nights, unassigned [0, 0, 0, 12].
        ([PreferredMaxRoomCapacityCost(level="medium")], (0, -96 * MEDIUM_UNIT)),  # weight 8 in medium units
        ([PreferredMaxRoomCapacityCost(level=Level.HARD)], (-96, 0.0)),
        ([PreferredMaxRoomCapacityCost(level=Level.MEDIUM, weight=1)], (0, -12 * MEDIUM_UNIT)),
        ([PreferredMaxRoomCapacityCost(level="soft")], (0, -96.0)),
        ([UnassignedStaysCost(level=Level.SOFT, weight=2)], (0, -24.0)),
        ([UnassignedStaysCost(level="hard", offset=1)], (-11, 0.0)),
        ([UnassignedStaysCost(level="medium", exponent=2, offset=10)], (0, -4 * MEDIUM_UNIT)),
        # A repeat carries its own level and shaping: soft nights, medium beyond 4 nights per
        # bed (2 + 2), hard 2 x nights beyond 5 per bed (1 + 1), and soft 2 x (x - 7) per real
        # bed: 2 x (-1 - 1 - 7) = -18, a reward (hard refuses the literal formula with an offset).
        (
            [
                PreferredMaxRoomCapacityCost(),
                PreferredMaxRoomCapacityCost(level="medium", offset=4),
                PreferredMaxRoomCapacityCost(level="hard", weight=2, offset=5),
                PreferredMaxRoomCapacityCost(level="soft", weight=2, offset=7, clip=False),
            ],
            (-4, -32 * MEDIUM_UNIT - 96 + 18),
        ),
        # The medium constraint repeated onto all three levels.
        (
            [
                UnassignedStaysCost(),
                UnassignedStaysCost(level=Level.HARD, weight=3, offset=2),
                UnassignedStaysCost(level=Level.SOFT, offset=20, clip=False),
            ],
            (-30, -12 * MEDIUM_UNIT + 8),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(ba.constraint_costs(_hand_plan()), picks) == expected
    raw = ba.constraint_costs(_hand_plan(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert _zero_budget(_hand_plan(), picks) == expected


def test_a_repeat_on_medium_reports_its_level_unfolded():
    """plan.score reports each level on its own: the repeat's medium term is medium."""
    picks = [
        PreferredPatientEquipmentCost(),
        PreferredMaxRoomCapacityCost(),
        PreferredMaxRoomCapacityCost(level="medium"),
    ]
    plan = _hand_plan()
    _zero_budget(plan, picks)
    assert (plan.score.hard, plan.score.medium, plan.score.soft) == (0, -96, -96)


def test_a_repeat_is_checked_on_its_own_level():
    """The second pick's level is read and checked like the first's, on both paths."""
    pick = SimpleNamespace(name="bed_conflict_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="constraint 'bed_conflict_cost': level must be 'hard', 'medium' or 'soft'"):
        _zero_budget(_hand_plan(), [BedConflictCost(), pick])
    with pytest.raises(ValueError, match="constraint 'bed_conflict_cost': level must be 'hard', 'medium' or 'soft'"):
        score(ba.constraint_costs(_hand_plan()), [BedConflictCost(), pick])


# --- clip=False with an offset stays off the hard score -------------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


@pytest.mark.parametrize("offset", [2.5, -2.5])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: BedConflictCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: BedConflictCost(level="hard", offset=offset, clip=False),
        lambda offset: MixedGenderRoomCost(offset=offset, clip=False),  # the global, hard by default
        lambda offset: PreferredMaxRoomCapacityCost(level="hard", offset=offset, clip=False),
        lambda offset: UnassignedStaysCost(level=Level.HARD, offset=offset, clip=False),
    ],
    ids=[
        "bed-conflict-default-hard",
        "bed-conflict-level-hard",
        "mixed-gender-default-hard",
        "preferred-capacity-moved-to-hard",
        "unassigned-moved-to-hard",
    ],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """A positive offset would reward the beds below it, so one bed's slack would cancel
    another bed's conflict and an infeasible plan could score hard 0; a negative one would
    charge every bed, an empty one included. The engine (solve, solve_with_constraints,
    constraint_costs) and the score() mirror refuse it with the same message; the CP-SAT
    reference's test_solve.py checks the same words."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    repeated = [type(spec)(), spec]  # a refused repeat next to its accepted first instance
    assert _refusal(lambda: ba.solve(build_plan(), time_limit_seconds=0.1, constraints=[spec])) == message
    assert _refusal(lambda: _zero_budget(_hand_plan(), repeated)) == message
    assert _refusal(lambda: ba.constraint_costs(_hand_plan(), [spec])) == message
    assert _refusal(lambda: ba.constraint_costs(_hand_plan(), repeated)) == message
    assert _refusal(lambda: score(ba.constraint_costs(_hand_plan()), [spec])) == message
    assert _refusal(lambda: score(ba.constraint_costs(_hand_plan()), repeated)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """(0 - offset) x 1000 on each of the three real beds, on soft, on every path; never the
    "no bed" sentinel. Medium folds into soft here, so it takes the literal formula too."""
    pick = BedConflictCost(level="soft", offset=offset, clip=False)
    expected = (0, 3 * 1000 * offset)
    assert score(ba.constraint_costs(_hand_plan()), [pick]) == expected
    raw = ba.constraint_costs(_hand_plan(), [pick])
    assert (raw["bed_conflict_cost"]["level"], raw["bed_conflict_cost"]["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    assert _zero_budget(_hand_plan(), [pick]) == expected
    plan = build_plan()
    ba.solve(plan, time_limit_seconds=0.2, random_seed=1, constraints=[BedConflictCost(), pick])
    assert score(ba.constraint_costs(plan), [BedConflictCost(), pick]) == _engine(plan.score)
    medium = BedConflictCost(level="medium", offset=offset, clip=False)
    assert score(ba.constraint_costs(_hand_plan()), [medium]) == (0, expected[1] * MEDIUM_UNIT)
    assert _zero_budget(_hand_plan(), [medium]) == (0, expected[1] * MEDIUM_UNIT)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # _mixed_room(): 9 mixed-gender nights, the global's one total.
        (MixedGenderRoomCost(offset=4), (-5000, 0.0)),  # clip=True on hard: a threshold
        (MixedGenderRoomCost(offset=-1), (-10000, 0.0)),  # a negative threshold adds 1 to the non-zero total
        (MixedGenderRoomCost(clip=False), (-9000, 0.0)),  # clip=False with offset 0: the plain shaping
        (MixedGenderRoomCost(offset=0.0, clip=False), (-9000, 0.0)),
        (BedConflictCost(offset=-1), (0, 0.0)),  # no conflict: a threshold adds nothing to a raw 0
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    assert score(ba.constraint_costs(_mixed_room()), [pick]) == expected
    assert _zero_budget(_mixed_room(), [pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(name="bed_conflict_cost", weight=None, level="soft", exponent=1.0, offset=1.0, clip=False)
    assert _zero_budget(_hand_plan(), [moved]) == (0, 3000.0)
    kept = SimpleNamespace(name="bed_conflict_cost", weight=None, level=None, exponent=1.0, offset=1.0, clip=False)
    message = HARD_REFUSAL.format(q="'bed_conflict_cost'")
    assert _refusal(lambda: _zero_budget(_hand_plan(), [kept])) == message
    assert _refusal(lambda: ba.constraint_costs(_hand_plan(), [kept])) == message
    assert _refusal(lambda: score(ba.constraint_costs(_hand_plan()), [kept])) == message
