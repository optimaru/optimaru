"""bed_allocation.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py` and the C++
tests (one hand-calculated positive and zero case per constraint). This file covers the
selection path that picks "which constraints are on", the catalog (`constraint_info()`),
the three knobs and the medium level.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import bed_allocation as ba
from bed_allocation import constraints as specs
from bed_allocation.constraints import (
    BedConflictCost,
    Constraint,
    PreferredMaxRoomCapacityCost,
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
    """A spec with a wrong name (e.g. a renamed component) must not be silently ignored."""
    raw, _ = _raw()

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])


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
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
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
    """level="soft" moves unassigned_stays_cost to soft in the solver (it was a no-op before 0.2.0).

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
    for pick, message in ((negative, "weight must be finite and >= 0"), (flat, "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            ba.solve(build_plan(), time_limit_seconds=0.1, constraints=[pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
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


# 0.2.0 migration: the pre-0.2.0 catalog names.
OLD_NAMES = [
    "same_bed_in_same_night",
    "female_in_male_room",
    "male_in_female_room",
    "department_minimum_age",
    "department_maximum_age",
    "required_patient_equipment",
    "assign_every_patient_to_a_bed",
    "preferred_maximum_room_capacity",
    "department_specialty",
    "department_specialty_not_first_priority",
    "preferred_patient_equipment",
    "different_gender_in_same_gender_room_in_same_night",
]


@pytest.mark.parametrize("old", OLD_NAMES)
def test_old_names_are_gone(old):
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        ba.solve(build_plan(), time_limit_seconds=0.05, constraints=[pick])
    with pytest.raises(ValueError, match="unknown constraint"):
        score(ba.constraint_costs(build_plan()), [pick])
