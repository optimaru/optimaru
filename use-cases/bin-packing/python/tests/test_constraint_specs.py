"""bin_packing.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the catalog, the five knobs (the level included) and repeated
picks are covered here.
"""

import pickle
import random
import re
from types import SimpleNamespace

import pytest

import bin_packing as bp
from bin_packing._constraint import instance_keys
from bin_packing.constraints import (
    BinCapacityCost,
    ColorConflictCost,
    Constraint,
    ItemConflictCost,
    Level,
    UsedBinsCost,
    bin_capacity_cost,
    color_conflict_cost,
    get_constraints,
    item_conflict_cost,
    score,
    used_bins_cost,
)

NAMES = ["bin_capacity_cost", "color_conflict_cost", "item_conflict_cost", "used_bins_cost"]


def build_problem() -> bp.BinPackingProblem:
    """Two reds and a conflict pair that would rather share a bin: the constraints on decide."""
    return bp.BinPackingProblem(
        10,
        [
            bp.Item("A", 5, color="red"),
            bp.Item("B", 5, color="red"),
            bp.Item("C", 4),
            bp.Item("D", 3),
            bp.Item("E", 3),
        ],
        conflicts=[("C", "D")],
    )


def _config(seconds: float = 0.3) -> bp.SolverConfig:
    config = bp.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = 1
    return config


def solve(constraints=None) -> bp.Score:
    problem = build_problem()
    bp.solve(problem, config=_config(), constraints=constraints)
    return problem.score


def _solved(constraints=None) -> bp.BinPackingProblem:
    problem = build_problem()
    bp.solve(problem, config=_config(), constraints=constraints)
    return problem


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    assert bp.list_constraints() == NAMES
    for spec in get_constraints():
        assert spec.name in bp.list_constraints()


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == bp.list_constraints()
    assert get_constraints() == [bin_capacity_cost, color_conflict_cost, item_conflict_cost, used_bins_cost]


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain == explicit
    assert plain == bp.Score(0, -3.0)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the soft term disappears entirely."""
    hard_only = solve([bin_capacity_cost, color_conflict_cost, item_conflict_cost])
    assert hard_only == bp.Score(0, 0.0)


def test_reweighting_one_constraint_scales_only_that_term():
    base = solve()
    heavier = solve([bin_capacity_cost, color_conflict_cost, item_conflict_cost, UsedBinsCost(weight=10)])
    assert heavier.hard == base.hard
    assert heavier.soft == 10 * base.soft


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft empties the hard level."""
    problem = build_problem()
    problem.set_assignment([0, 0, 0, 0, 0])  # 20 into a capacity-10 bin, two reds, C-D together
    assert bp.calculate_score(problem).hard == -(10 + 1 + 1)
    demoted = [
        BinCapacityCost(level="soft"),
        ColorConflictCost(level="soft"),
        ItemConflictCost(level="soft"),
        used_bins_cost,
    ]
    assert bp.calculate_score(problem, demoted) == bp.Score(0, -(10 + 1 + 1 + 1))
    assert bp.calculate_score(problem, [UsedBinsCost(level="hard")]) == bp.Score(-1, 0.0)


def test_calculate_score_with_constraints_stores_the_score():
    problem = build_problem()
    problem.set_assignment([0, 1, 2, 3, 4])
    assert bp.calculate_score(problem, [UsedBinsCost(weight=2)]) == bp.Score(0, -10.0)
    assert problem.score == bp.Score(0, -10.0)
    assert bp.calculate_score(problem) == bp.Score(0, -5.0)


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""
    typo = SimpleNamespace(name="used_bin_cost", weight=None, level=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        solve([typo])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        bp.solve(build_problem(), config=_config(0.1), constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    for spec in (bin_capacity_cost, color_conflict_cost, item_conflict_cost, used_bins_cost):
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
    assert [spec.name for spec in get_constraints()] == NAMES


def test_every_default_instance_is_exported():
    import bin_packing.constraints as module

    for name in NAMES:
        assert name in module.__all__
        assert getattr(module, name).name == name


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = bp.constraint_info()
    assert [row.name for row in rows] == bp.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
    ]


def test_constraint_info_rows_are_plain_values():
    row = bp.constraint_info()[0]
    assert isinstance(row, bp.ConstraintInfo)
    assert row._asdict() == {
        "name": "bin_capacity_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='bin_capacity_cost'")
    assert pickle.loads(pickle.dumps(bp.constraint_info())) == bp.constraint_info()
    assert bp.ConstraintInfo.__module__ == bp._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint."""
    raw = bp.constraint_costs(build_problem())
    assert list(raw) == NAMES
    for row in bp.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] is (row.name == "used_bins_cost")
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert "medium_fold" not in entry  # no medium level in this domain


def test_constraint_costs_shapes():
    """Three per-bin constraints (one entry per bin 0..n-1) and the global bin count."""
    problem = build_problem()
    problem.set_assignment([0, 0, 1, 1, 4])
    raw = bp.constraint_costs(problem)
    for name in NAMES[:3]:
        assert len(raw[name]["costs"]) == problem.item_count()
    assert raw["used_bins_cost"]["costs"] == [3]
    assert raw["color_conflict_cost"]["costs"] == [1, 0, 0, 0, 0]
    assert raw["item_conflict_cost"]["costs"] == [0, 1, 0, 0, 0]
    assert raw["bin_capacity_cost"]["costs"] == [0, 0, 0, 0, 0]


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights (1 each)."""
    explicit = [
        BinCapacityCost(weight=1),
        ColorConflictCost(weight=1),
        ItemConflictCost(weight=1),
        UsedBinsCost(weight=1),
    ]
    problem = _solved()
    raw = bp.constraint_costs(problem)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (problem.score.hard, problem.score.soft)


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": float("inf")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"exponent": -1.0}, ValueError),
        ({"hard": 1}, TypeError),
        ({"offset": float("nan")}, ValueError),
        ({"offset": "1"}, TypeError),
        ({"clip": 0}, TypeError),
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        UsedBinsCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="used_bins_cost", weight=-2.0, level=None, exponent=1.0)
    flat = SimpleNamespace(name="used_bins_cost", weight=None, level=None, exponent=0.0)
    unbounded = SimpleNamespace(name="used_bins_cost", weight=None, level=None, exponent=1.0, offset=float("inf"))
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
        (unbounded, "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            bp.solve(build_problem(), config=_config(0.1), constraints=[pick])
        with pytest.raises(ValueError, match=message):
            bp.calculate_score(build_problem(), [pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
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
    pick = SimpleNamespace(name="used_bins_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'used_bins_cost': {re.escape(message)}"):
        bp.solve(build_problem(), config=_config(0.1), constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        UsedBinsCost(**knobs)


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    for old in ("bin_capacity", "color_compatibility", "item_conflicts", "minimize_bins"):
        pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            solve([pick])
        with pytest.raises(ValueError, match="unknown constraint"):
            score(bp.constraint_costs(build_problem()), [pick])
    import bin_packing.constraints as module

    for old in ("BinCapacity", "ColorCompatibility", "ItemConflicts", "MinimizeBins", "minimize_bins"):
        assert not hasattr(module, old)


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def test_score_equals_the_engine_for_the_default_assembly():
    problem = _solved()
    engine = bp.calculate_score(problem)
    python = score(bp.constraint_costs(problem))
    assert python == (engine.hard, engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [bin_capacity_cost],
        [bin_capacity_cost, color_conflict_cost, item_conflict_cost, UsedBinsCost(weight=60)],
        [BinCapacityCost(level="soft"), color_conflict_cost, item_conflict_cost, used_bins_cost],
        [BinCapacityCost(exponent=2.0), ColorConflictCost(exponent=3.0), item_conflict_cost, used_bins_cost],
        [BinCapacityCost(weight=0.5), UsedBinsCost(level="hard", weight=0.5, exponent=2.0)],
        [*get_constraints(), UsedBinsCost(weight=100, offset=1), BinCapacityCost(exponent=2)],
        [
            BinCapacityCost(offset=1),
            BinCapacityCost(offset=0.5, clip=False, weight=3),
            ColorConflictCost(offset=-1),
            ItemConflictCost(offset=1, clip=False),
            UsedBinsCost(),
            UsedBinsCost(offset=2, exponent=2, weight=0.5, clip=False),
        ],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score."""
    problem = _solved(specs)
    python = score(bp.constraint_costs(problem), specs)
    assert python.hard == problem.score.hard
    assert python.soft == pytest.approx(problem.score.soft)


def test_python_score_equals_cpp_calculate_score_for_random_specs():
    """For random assignments and random specs (exponents included): Python == C++."""
    rng = random.Random(3)
    classes = (BinCapacityCost, ColorConflictCost, ItemConflictCost, UsedBinsCost)
    for _ in range(60):
        problem = build_problem()
        problem.set_assignment([rng.randrange(problem.item_count()) for _ in range(problem.item_count())])
        picked = rng.sample(classes, rng.randrange(1, len(classes) + 1))
        specs = [
            cls(
                weight=rng.choice([None, 0, 0.5, 1, 3]),
                level=rng.choice([None, "hard", "soft"]),
                exponent=rng.choice([1.0, 0.5, 2.0, 3.0]),
            )
            for cls in picked
        ]
        engine = bp.calculate_score(problem, specs)
        python = score(bp.constraint_costs(problem), specs)
        assert python.hard == engine.hard
        assert python.soft == pytest.approx(engine.soft)


def test_exponent_shapes_per_bin_and_the_bin_count():
    """The 0.2.0 unit granularity: per bin for colours/conflicts, on the count for used bins."""
    problem = bp.BinPackingProblem(
        100,
        [bp.Item(f"R{k}", 1, color="red") for k in range(3)] + [bp.Item(f"B{k}", 1, color="blue") for k in range(2)],
        conflicts=[("R0", "R1"), ("R1", "R2")],
    )
    problem.set_assignment([0, 0, 0, 0, 0])
    raw = bp.constraint_costs(problem)
    assert score(raw, [ColorConflictCost(exponent=2.0)]).hard == -9  # 0.1.x: 2**2 + 1**2 = 5
    assert score(raw, [ItemConflictCost(exponent=2.0)]).hard == -4  # 0.1.x: 1 + 1 = 2
    problem.set_assignment([0, 1, 2, 3, 4])
    assert bp.calculate_score(problem, [UsedBinsCost(exponent=2.0)]) == bp.Score(0, -25.0)


def test_score_is_additive_over_the_picked_constraints():
    problem = _solved()
    raw = bp.constraint_costs(problem)
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_rejects_an_unknown_constraint_name():
    raw = bp.constraint_costs(_solved())

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [UsedBinsCost(), UsedBinsCost(weight=10)]
    problem = _zero_budget_solve(_hand(), picks)
    assert problem.score == bp.Score(0, -(1 + 10) * 2)
    assert get_constraints([UsedBinsCost(), UsedBinsCost()]) == [UsedBinsCost(), UsedBinsCost()]


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def _hand() -> bp.BinPackingProblem:
    """build_problem() placed by hand, n = 5 bins: bin 0 = A5 B5 E3 (13 on 10, two reds),
    bin 1 = C4 D3 (the conflict pair). Raw: capacity [3, 0, 0, 0, 0], colour
    [1, 0, 0, 0, 0], conflicts [0, 1, 0, 0, 0], 2 bins used."""
    problem = build_problem()
    problem.set_assignment([0, 0, 1, 1, 0])
    return problem


def _zero_budget_solve(problem, picks):
    """Score a complete assignment under ``picks`` on the engine: a warm start, no step taken."""
    config = bp.SolverConfig()
    config.spent_limit_seconds = 0
    assignment = problem.assignment()
    problem.warm_start = True  # otherwise the solve ignores the input bins
    bp.Solver(config).solve_with_constraints(problem, picks)
    assert problem.assignment() == assignment
    return problem


def _engine(picks):
    """Both engine paths: calculate_score with the picks and a zero-budget solve."""
    calculated = bp.calculate_score(_hand(), picks)
    solved = _zero_budget_solve(_hand(), picks).score
    assert (calculated.hard, calculated.soft) == (solved.hard, solved.soft)
    return (calculated.hard, calculated.soft)


REPEAT = [UsedBinsCost(), UsedBinsCost(weight=100, offset=1)]


def test_the_hand_instance():
    raw = bp.constraint_costs(_hand())
    assert {name: entry["costs"] for name, entry in raw.items()} == {
        "bin_capacity_cost": [3, 0, 0, 0, 0],
        "color_conflict_cost": [1, 0, 0, 0, 0],
        "item_conflict_cost": [0, 1, 0, 0, 0],
        "used_bins_cost": [2],
    }
    assert score(raw) == _engine(None) == (-5, -2.0)


def test_a_repeated_pick_by_hand():
    """2 bins, plus 100 x (2 - 1) for the bins beyond the first: -102."""
    raw = bp.constraint_costs(_hand(), REPEAT)
    assert list(raw) == ["used_bins_cost", "used_bins_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [2.0]
    assert (first["weight"], first["offset"], second["weight"], second["offset"]) == (1.0, 0.0, 100.0, 1.0)
    assert first["name"] == second["name"] == "used_bins_cost"
    assert second["default_weight"] == 1.0
    expected = (0, -(2 + 100 * 1))
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(bp.constraint_costs(_hand()), REPEAT) == expected
    assert _engine(REPEAT) == expected


def test_a_repeated_unit_pick_by_hand():
    """capacity per bin plus 2 x its square: -(3 + 2 x 9) on bin 0."""
    picks = [BinCapacityCost(), BinCapacityCost(weight=2, exponent=2)]
    raw = bp.constraint_costs(_hand(), picks)
    assert list(raw) == ["bin_capacity_cost", "bin_capacity_cost#2"]
    assert raw["bin_capacity_cost"]["costs"] == raw["bin_capacity_cost#2"]["costs"]
    assert score(raw, picks) == (-21, 0.0)
    assert score(bp.constraint_costs(_hand()), picks) == (-21, 0.0)
    assert _engine(picks) == (-21, 0.0)


def test_identical_picks_count_twice():
    picks = [ItemConflictCost(), ItemConflictCost()]
    assert score(bp.constraint_costs(_hand()), picks) == (-2, 0.0)
    assert _engine(picks) == (-2, 0.0)
    assert list(bp.constraint_costs(_hand(), picks)) == ["item_conflict_cost", "item_conflict_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        UsedBinsCost(weight=1),
        BinCapacityCost(),
        UsedBinsCost(weight=2),
        ColorConflictCost(),
        UsedBinsCost(weight=3),
    ]
    raw = bp.constraint_costs(_hand(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "bin_capacity_cost": 1.0,
        "color_conflict_cost": 1.0,
        "used_bins_cost": 1.0,
        "used_bins_cost#2": 2.0,
        "used_bins_cost#3": 3.0,
    }
    assert score(raw, picks) == (-(3 + 1), -(6 * 2))
    assert _engine(picks) == score(raw, picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Per-bin constraints, shaped per bin 0..4; an empty bin (raw 0) counts in literal mode.
        (BinCapacityCost(offset=1), (-2, 0.0)),
        (BinCapacityCost(offset=1, clip=False), (-(2 - 4), 0.0)),  # the four other bins are rewarded
        (BinCapacityCost(offset=5), (0, 0.0)),
        (BinCapacityCost(offset=5, clip=False), (-(-2 - 4 * 5), 0.0)),
        (ItemConflictCost(offset=-2), (-3, 0.0)),  # a negative offset adds to every non-zero bin
        (ItemConflictCost(offset=-2, clip=False), (-(3 + 4 * 2), 0.0)),  # ...or to every bin
        (ColorConflictCost(offset=0.5, clip=False, weight=3), (6, 0.0)),  # 1.5 -> 2, 4 x (-1.5 -> -2): per bin
        (
            ColorConflictCost(offset=2, exponent=2, clip=False),
            (-(-1 - 4 * 4), 0.0),
        ),  # (1 - 2)^2, 4 x (0 - 2)^2, signs kept
        # The global shapes its one count.
        (UsedBinsCost(offset=1, exponent=2), (0, -1.0)),
        (UsedBinsCost(offset=5, exponent=3, clip=False), (0, 27.0)),  # (2 - 5)^3: a reward below 5 bins
        (UsedBinsCost(offset=-1), (0, -3.0)),
        (UsedBinsCost(level="hard", offset=0.5, clip=False, weight=3), (-5, 0.0)),  # 4.5 -> 5
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(bp.constraint_costs(_hand()), [pick]) == expected
    raw = bp.constraint_costs(_hand(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert "units" not in entry  # every bin is evaluated: no scoped constraint here
    assert score(raw, [pick]) == expected
    assert _engine([pick]) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [BinCapacityCost(exponent=2), ColorConflictCost(), ItemConflictCost(), UsedBinsCost(exponent=2)]
    shifted = [
        BinCapacityCost(exponent=2, offset=0.0, clip=False),
        ColorConflictCost(offset=0),
        ItemConflictCost(clip=False),
        UsedBinsCost(exponent=2, offset=0.0, clip=False),
    ]
    problem = _solved()
    raw = bp.constraint_costs(problem)
    assert score(raw, shifted) == score(raw, plain)
    assert bp.calculate_score(problem, shifted) == bp.calculate_score(problem, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


def test_python_score_equals_cpp_calculate_score_for_random_repeats_and_offsets():
    """Random assignments, random pick lists with repeats, offsets of both signs and both clip modes."""
    rng = random.Random(11)
    classes = (BinCapacityCost, ColorConflictCost, ItemConflictCost, UsedBinsCost)
    for _ in range(60):
        problem = build_problem()
        problem.set_assignment([rng.randrange(problem.item_count()) for _ in range(problem.item_count())])
        specs = [
            rng.choice(classes)(
                weight=rng.choice([None, 0, 0.5, 1, 3]),
                level=rng.choice([None, "hard", "soft"]),
                exponent=rng.choice([1.0, 0.5, 2.0, 3.0]),
                offset=rng.choice([0, 0.5, 1, 2, -1, -0.25]),
                clip=rng.choice([True, False]),
            )
            for _ in range(rng.randrange(1, 7))
        ]
        engine = bp.calculate_score(problem, specs)
        python = score(bp.constraint_costs(problem), specs)
        assert python.hard == engine.hard
        assert python.soft == pytest.approx(engine.soft)
        assert score(bp.constraint_costs(problem, specs), specs) == python


def _random_problem(seed: int, items: int = 30) -> bp.BinPackingProblem:
    rng = random.Random(seed)
    pairs = [tuple(rng.sample(range(items), 2)) for _ in range(10)]
    return bp.BinPackingProblem(
        20,
        [bp.Item(f"i{k}", rng.randint(1, 12), color=rng.choice([None, "red", "blue", "green"])) for k in range(items)],
        conflicts=[(f"i{a}", f"i{b}") for a, b in pairs],
    )


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), *REPEAT, BinCapacityCost(weight=2, exponent=2)],
        [
            BinCapacityCost(),
            BinCapacityCost(offset=1, clip=False, weight=2),
            ColorConflictCost(offset=-1),
            ColorConflictCost(offset=0.5, clip=False, weight=3),
            ItemConflictCost(offset=1, clip=False, exponent=2),
            UsedBinsCost(),
            UsedBinsCost(offset=8, exponent=2, weight=0.25),
        ],
        [
            *get_constraints(),
            BinCapacityCost(level="soft", weight=1.5, offset=1, clip=False),
            ItemConflictCost(level=Level.SOFT, weight=2, offset=-1),
            UsedBinsCost(level="hard", offset=8),
        ],
    ],
    ids=["repeat", "repeat-and-offsets", "repeats-on-other-levels"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    problem = _random_problem(5)
    config = bp.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 5000
    config.spent_limit_seconds = 30
    bp.Solver(config).solve_with_constraints(problem, picks)
    raw = bp.constraint_costs(problem, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == problem.score.hard
    assert python.soft == pytest.approx(problem.score.soft)
    assert score(bp.constraint_costs(problem), picks) == python  # the default costs score the repeats too
    engine = bp.calculate_score(problem, picks)
    assert engine.hard == python.hard
    assert engine.soft == pytest.approx(python.soft)


# --- Levels: hard and soft, each pick (a repeat included) on the level it names ------


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        ([UsedBinsCost(level="hard")], (-2, 0.0)),
        ([BinCapacityCost(level=Level.SOFT, weight=2)], (0, -6.0)),
        ([ItemConflictCost(level="soft", offset=1, clip=False)], (0, 4.0)),  # (0 - 1) on four bins, 0 on one
        ([ColorConflictCost(level="hard")], (-1, 0.0)),  # the default level, named
        # A repeat carries its own level and shaping. Hard: the capacity [3, 0, 0, 0, 0] and
        # the bins beyond the first (2 - 1). Soft: the 2 bins and 3 x (x - 1) per bin on
        # the capacity, 3 x (2 - 1 - 1 - 1 - 1) = -6: -2 + 6.
        (
            [
                UsedBinsCost(),
                UsedBinsCost(level="hard", offset=1),
                BinCapacityCost(),
                BinCapacityCost(level=Level.SOFT, weight=3, offset=1, clip=False),
            ],
            (-4, 4.0),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(bp.constraint_costs(_hand()), picks) == expected
    raw = bp.constraint_costs(_hand(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert _engine(picks) == expected


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_without_a_medium_level(level):
    """The domain has no medium level: every path refuses the pick before scoring, a repeat too."""
    message = "constraint 'used_bins_cost': this use case has no medium level"
    pick = UsedBinsCost(level=level)  # the spec cannot know the domain
    for picks in ([pick], [UsedBinsCost(), pick]):
        with pytest.raises(ValueError, match=message):
            _zero_budget_solve(_hand(), picks)
        with pytest.raises(ValueError, match=message):
            bp.calculate_score(_hand(), picks)
        with pytest.raises(ValueError, match=message):
            bp.solve(build_problem(), config=_config(0.1), constraints=picks)
        with pytest.raises(ValueError, match=message):
            bp.constraint_costs(_hand(), picks)
        with pytest.raises(ValueError, match=message):
            score(bp.constraint_costs(_hand()), picks)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        UsedBinsCost(level=True)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        UsedBinsCost(level="firm")
    pick = SimpleNamespace(name="used_bins_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        _engine([UsedBinsCost(), pick])


def test_the_knob_was_renamed():
    """0.3.0 replaced hard=True / hard=False by level="hard" / level="soft"; hard= is unknown."""
    with pytest.raises(TypeError):
        UsedBinsCost(hard=True)
    with pytest.raises(TypeError):
        BinCapacityCost(weight=None, hard=False)
