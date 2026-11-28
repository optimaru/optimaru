"""optimaru.box_sizing.constraints — an API for picking the cost function declaratively."""

import pytest

import optimaru.box_sizing as bs
from optimaru.box_sizing.constraints import (
    BoxCost,
    BoxTypeCost,
    BoxTypeLimitCost,
    BoxWeightCost,
    ParcelFitCost,
    VoidVolumeCost,
    box_cost,
    box_type_cost,
    box_type_limit_cost,
    box_weight_cost,
    get_constraints,
    parcel_fit_cost,
    score,
    void_volume_cost,
)

from .._picks import only, replace_default, without

NAMES = [
    "parcel_fit_cost",
    "box_weight_cost",
    "void_volume_cost",
    "box_cost",
    "box_type_limit_cost",
    "box_type_cost",
]


def by_hand():
    """Two boxes and four parcels, every cost worked out by hand.

    B0 300 x 200 x 100 (6,000,000) carries 1,000 and costs 2.5; B1 400 x 300 x 200
    (24,000,000) is required. In B0: `long` (330 x 120 x 210, x2) exceeds it by
    20 + 10 + 30 = 60 and is larger than the box (void 0); `heavy` (300 x 100 x 100, x4,
    1,300 g) is 300 over the weight and leaves 4 x 3,000,000 of air. In B1: `fits`
    (400 x 300 x 100, x3) leaves 3 x 12,000,000. `loose` has no box.
    """
    problem = bs.BoxSizingProblem(
        [bs.Box("B0", 300, 200, 100, max_weight=1000, cost=2.5), bs.Box("B1", 400, 300, 200, required=True)],
        [
            bs.Parcel("long", 330, 120, 210, quantity=2),
            bs.Parcel("heavy", 300, 100, 100, weight=1300, quantity=4),
            bs.Parcel("fits", 400, 300, 100, quantity=3),
            bs.Parcel("loose", 100, 100, 100, quantity=9),
        ],
        max_box_types=1,
    )
    problem.set_assignment([0, 0, 1, -1])
    return problem


def solved(constraints=None, **arguments):
    problem = bs.generate_demo_data(parcel_count=40, **arguments).to_core()
    bs.solve(problem, time_limit_seconds=0.2, random_seed=1, constraints=constraints)
    return problem


def test_list_constraints_matches_the_spec_names():
    assert bs.list_constraints() == NAMES
    assert [spec.name for spec in get_constraints()] == NAMES


def test_module_level_instances_keep_every_default():
    instances = (parcel_fit_cost, box_weight_cost, void_volume_cost, box_cost, box_type_limit_cost, box_type_cost)
    for spec in instances:
        assert (spec.weight, spec.level, spec.exponent, spec.offset, spec.clip) == (None, None, 1.0, 0.0, True)
    assert get_constraints() == list(instances)


def test_constraint_info_reports_the_cpp_defaults():
    rows = bs.constraint_info()
    assert [row.name for row in rows] == NAMES
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("soft", 0.0, True, False, False),
    ]


def test_constraint_info_matches_the_raw_cost_metadata():
    raw = bs.constraint_costs(by_hand())
    for row in bs.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"]) == (row.default_weight, row.default_level)
        assert entry["global"] == (row.name in ("box_type_limit_cost", "box_type_cost"))
        assert (entry["level_flippable"], entry["reward"], entry["name"]) == (True, False, row.name)


def test_constraint_costs_by_hand():
    raw = bs.constraint_costs(by_hand())
    assert raw["parcel_fit_cost"]["costs"] == [60.0, 0.0]
    assert raw["box_weight_cost"]["costs"] == [300.0, 0.0]
    assert raw["void_volume_cost"]["costs"] == [12_000_000.0, 36_000_000.0]
    assert raw["box_cost"]["costs"] == [15.0, 0.0]  # 6 shipments x 2.5; B1 costs nothing
    assert raw["box_type_limit_cost"]["costs"] == [1.0]  # B0 and the required B1 against 1
    assert raw["box_type_cost"]["costs"] == [2.0]
    assert score(raw) == (-(60 + 300 + 1), -(48_000_000 + 15))
    engine = bs.calculate_score(by_hand())
    assert (engine.hard, engine.soft) == (-361, -48_000_015.0)


def test_the_summary_getters_agree_with_the_costs():
    problem = by_hand()
    assert problem.void_volume() == 48_000_000
    assert problem.box_types_used() == 2
    assert problem.parcels_per_box() == [6, 3]
    # `long` is larger than its box: it counts as the box's volume, as it counts no air.
    shipped = 2 * 6_000_000 + 4 * 3_000_000 + 3 * 12_000_000
    boxes = 6 * 6_000_000 + 3 * 24_000_000
    assert problem.fill_ratio() == pytest.approx(shipped / boxes)
    assert problem.void_volume() == boxes - shipped


def test_the_fill_ratio_never_passes_one():
    problem = bs.BoxSizingProblem([bs.Box("S", 10, 10, 10)], [bs.Parcel("big", 30, 30, 30, quantity=4, box=0)])
    assert bs.calculate_score(problem).hard == -60
    assert (problem.fill_ratio(), problem.void_volume()) == (1.0, 0)
    model = bs.models.BoxSizingProblemModel.from_core(problem)
    assert (model.fill_ratio, model.void_volume, model.score_hard) == (1.0, 0, -60)


def test_the_clean_cases_cost_nothing():
    problem = bs.BoxSizingProblem(
        [bs.Box("B0", 300, 200, 100, max_weight=1000), bs.Box("B1", 400, 300, 200)],
        [bs.Parcel("exact", 100, 300, 200, weight=1000, quantity=5)],  # turned, it fills B0
        max_box_types=1,
    )
    problem.set_assignment([0])
    raw = bs.constraint_costs(problem)
    for name in NAMES[:5]:
        assert set(raw[name]["costs"]) == {0.0}, name
    assert raw["box_type_cost"]["costs"] == [1.0]
    assert bs.calculate_score(problem).soft == 0.0


def test_default_solve_equals_explicit_default_constraints():
    plain, explicit = solved(), solved(get_constraints())
    assert (plain.score.hard, plain.score.soft) == (explicit.score.hard, explicit.score.soft)
    assert plain.assignment() == explicit.assignment()


def test_weight_none_means_the_component_default():
    explicit = [
        ParcelFitCost(weight=1),
        BoxWeightCost(weight=1),
        VoidVolumeCost(weight=1),
        BoxCost(weight=1),
        BoxTypeLimitCost(weight=1),
        BoxTypeCost(weight=0),
    ]
    raw = bs.constraint_costs(by_hand())
    assert score(raw) == score(raw, explicit)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [parcel_fit_cost, void_volume_cost],
        [BoxCost(weight=1000) if spec.name == "box_cost" else spec for spec in get_constraints()],
        [BoxTypeCost(weight=3_000_000) if spec.name == "box_type_cost" else spec for spec in get_constraints()],
        [*get_constraints(), VoidVolumeCost(weight=1e-9, exponent=2.0, offset=1_000_000.0)],
        [*get_constraints(), BoxTypeCost(weight=1000, exponent=2.0), BoxTypeCost(weight=500, offset=2.0)],
        [
            BoxWeightCost(level="soft", weight=5) if spec.name == "box_weight_cost" else spec
            for spec in get_constraints()
        ],
        [parcel_fit_cost, box_weight_cost, VoidVolumeCost(offset=2_000_000.0, clip=False), box_type_limit_cost],
        # A soft constraint moved to the hard level: the prices of the construction and the
        # moves read its weight only, and the solve still ends on a score both sides agree on.
        [VoidVolumeCost(level="hard") if spec.name == "void_volume_cost" else spec for spec in get_constraints()],
        [BoxTypeCost(level="hard", weight=3) if spec.name == "box_type_cost" else spec for spec in get_constraints()],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution under the specs it was solved with must equal the engine's score."""
    problem = solved(specs)
    python = score(bs.constraint_costs(problem, specs), specs)
    assert python.hard == problem.score.hard
    assert python.soft == pytest.approx(problem.score.soft)
    engine = bs.calculate_score(problem, specs)
    assert (engine.hard, engine.soft) == (problem.score.hard, pytest.approx(problem.score.soft))


def test_score_is_additive_over_the_picked_constraints():
    raw = bs.constraint_costs(by_hand())
    full = score(raw)
    parts = [score(raw, [spec]) for spec in get_constraints()]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_picking_a_subset_drops_the_other_terms():
    raw = bs.constraint_costs(by_hand())
    assert score(raw, only(get_constraints(), "parcel_fit_cost")) == (-60, 0.0)
    assert score(raw, only(get_constraints(), "box_cost")) == (0, -15.0)
    assert score(raw, without(get_constraints(), "void_volume_cost")) == (-361, -15.0)


def test_a_level_can_be_flipped():
    picks = replace_default(get_constraints(), "box_weight_cost", BoxWeightCost(level="soft", weight=2))
    assert score(bs.constraint_costs(by_hand(), picks), picks) == (-61, -(48_000_015 + 600))
    picks = replace_default(get_constraints(), "void_volume_cost", VoidVolumeCost(level="hard"))
    assert score(bs.constraint_costs(by_hand(), picks), picks) == (-(361 + 48_000_000), -15.0)


def test_naming_a_constraint_twice_scores_both_picks():
    picks = [box_cost, BoxCost(weight=10)]
    raw = bs.constraint_costs(by_hand(), picks)
    assert list(raw) == ["box_cost", "box_cost#2"]
    assert score(raw, picks) == (0, -(15 + 150.0))
    engine = bs.calculate_score(by_hand(), picks)
    assert (engine.hard, engine.soft) == (0, -165.0)


def test_box_type_cost_is_opt_in_and_its_weight_is_the_cost_of_a_size():
    raw = bs.constraint_costs(by_hand())
    assert raw["box_type_cost"]["weight"] == 0.0
    assert score(raw, [box_type_cost]) == (0, 0.0)
    assert score(raw, [BoxTypeCost(weight=50)]) == (0, -100.0)
    # The exponent shapes the count: 2 sizes squared.
    assert score(raw, [BoxTypeCost(weight=50, exponent=2)]) == (0, -200.0)


def test_a_fixed_cost_per_size_makes_the_solve_stock_fewer_sizes():
    def sizes(weight):
        picks = replace_default(get_constraints(), "box_type_cost", BoxTypeCost(weight=weight))
        problem = solved(picks, max_box_types=None)
        assert problem.score.hard == 0
        return problem.box_types_used(), problem.void_volume()

    free, some, dear = sizes(0), sizes(20_000_000), sizes(2_000_000_000)
    assert free[0] > some[0] > dear[0] >= 1
    assert free[1] < some[1] < dear[1]


def test_the_construction_follows_the_picked_weights():
    """With box_cost a thousand times heavier, the zero-budget result (the construction alone)
    already avoids the dear boxes: the greedy prices the sizes with the picked weights."""
    data = bs.generate_demo_data(parcel_count=40).model_dump()
    for index, box in enumerate(data["boxes"]):
        box["cost"] = 1000.0 if index % 2 else 0.0
    model = bs.models.BoxSizingProblemModel.model_validate(data)
    picks = replace_default(get_constraints(), "box_cost", BoxCost(weight=1000))
    plain = bs.solve(model, time_limit_seconds=0)
    weighted = bs.solve(model, time_limit_seconds=0, constraints=picks)

    def dear_shipments(solved_model):
        return sum(box.parcels for box in solved_model.boxes if box.cost)

    assert dear_shipments(weighted) < dear_shipments(plain)


def test_the_literal_offset_is_refused_on_hard():
    pick = ParcelFitCost(offset=5.0, clip=False)
    with pytest.raises(ValueError, match="hard 0 must mean no violation"):
        bs.calculate_score(by_hand(), [pick])
    with pytest.raises(ValueError, match="hard 0 must mean no violation"):
        bs.solve(by_hand(), time_limit_seconds=0, constraints=[pick])
    soft = ParcelFitCost(level="soft", offset=5.0, clip=False)
    assert bs.calculate_score(by_hand(), [soft]).soft == -(55.0 - 5.0)


def test_full_assert_solves_with_repeats_and_offsets():
    picks = [
        *get_constraints(),
        VoidVolumeCost(weight=1e-9, exponent=2.0, offset=1_000_000.0),
        BoxTypeCost(weight=1_000_000),
    ]
    problem = bs.generate_demo_data(parcel_count=30).to_core()
    config = bs.SolverConfig(stop={"time_limit_seconds": 60, "step_limit": 2000})
    bs.solve(problem, config=config, full_assert=True, constraints=picks)
    python = score(bs.constraint_costs(problem, picks), picks)
    assert (python.hard, python.soft) == (problem.score.hard, pytest.approx(problem.score.soft))
