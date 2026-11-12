"""optimaru.bin_packing.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the catalog, the five knobs (the level included) and repeated
picks are covered here.
"""

import random
from types import SimpleNamespace

import pytest

import optimaru.bin_packing as bp
from optimaru._constraint import instance_keys
from optimaru.bin_packing.constraints import (
    BinCapacityCost,
    ColorConflictCost,
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
PER_BIN = {"bin_capacity_cost", "color_conflict_cost", "item_conflict_cost"}  # one raw cost per bin


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


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    for spec in (bin_capacity_cost, color_conflict_cost, item_conflict_cost, used_bins_cost):
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
    assert [spec.name for spec in get_constraints()] == NAMES


def test_every_default_instance_is_exported():
    import optimaru.bin_packing.constraints as module

    for name in NAMES:
        assert name in module.__all__
        assert getattr(module, name).name == name


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


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint."""
    raw = bp.constraint_costs(build_problem())
    assert list(raw) == NAMES
    for row in bp.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"]) == (
            row.default_weight,
            row.default_level,
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
    for name in PER_BIN:
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
            # The literal formula with an offset is refused on hard; its copies sit on soft.
            BinCapacityCost(offset=1),
            BinCapacityCost(level="soft", offset=0.5, clip=False, weight=3),
            ColorConflictCost(offset=-1),
            ItemConflictCost(offset=1),
            ItemConflictCost(level="soft", offset=1, clip=False),
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
    """The unit granularity: per bin for colours/conflicts, on the count for used bins."""
    problem = bp.BinPackingProblem(
        100,
        [bp.Item(f"R{k}", 1, color="red") for k in range(3)] + [bp.Item(f"B{k}", 1, color="blue") for k in range(2)],
        conflicts=[("R0", "R1"), ("R1", "R2")],
    )
    problem.set_assignment([0, 0, 0, 0, 0])
    raw = bp.constraint_costs(problem)
    assert score(raw, [ColorConflictCost(exponent=2.0)]).hard == -9  # not 2**2 + 1**2 = 5
    assert score(raw, [ItemConflictCost(exponent=2.0)]).hard == -4  # not 1 + 1 = 2
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


def test_naming_a_constraint_twice_scores_both_picks():
    """A constraint may be picked several times: each pick is scored on its own.

    The later pick does not win in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [UsedBinsCost(), UsedBinsCost(weight=10)]
    problem = _zero_budget_solve(_hand(), picks)
    assert problem.score == bp.Score(0, -(1 + 10) * 2)
    assert get_constraints([UsedBinsCost(), UsedBinsCost()]) == [UsedBinsCost(), UsedBinsCost()]


# --- repeated picks and the offset / clip knobs ------------------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


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
        # The three per-bin constraints are hard by default, which refuses the literal
        # formula with an offset, so its numbers are checked on soft (not rounded).
        (BinCapacityCost(offset=1), (-2, 0.0)),
        (BinCapacityCost(level="soft", offset=1, clip=False), (0, -(2 - 4.0))),  # the four other bins are rewarded
        (BinCapacityCost(offset=5), (0, 0.0)),
        (BinCapacityCost(level="soft", offset=5, clip=False), (0, -(-2 - 4 * 5.0))),
        (ItemConflictCost(offset=-2), (-3, 0.0)),  # a negative offset adds to every non-zero bin
        (ItemConflictCost(level="soft", offset=-2, clip=False), (0, -(3 + 4 * 2.0))),  # ...or to every bin
        (ColorConflictCost(offset=0.5, weight=3), (-2, 0.0)),  # hard rounds per bin: 1.5 -> 2, 0 -> 0
        (ColorConflictCost(level="soft", offset=0.5, clip=False, weight=3), (0, -(1.5 - 4 * 1.5))),  # per bin
        (
            ColorConflictCost(level="soft", offset=2, exponent=2, clip=False),
            (0, -(-1 - 4 * 4.0)),
        ),  # (1 - 2)^2, 4 x (0 - 2)^2, signs kept
        # The global shapes its one count.
        (UsedBinsCost(offset=1, exponent=2), (0, -1.0)),
        (UsedBinsCost(offset=5, exponent=3, clip=False), (0, 27.0)),  # (2 - 5)^3: a reward below 5 bins
        (UsedBinsCost(offset=-1), (0, -3.0)),
        (UsedBinsCost(level="hard", offset=0.5, weight=3), (-5, 0.0)),  # 4.5 -> 5
        (UsedBinsCost(offset=0.5, clip=False, weight=3), (0, -4.5)),  # the literal one stays on soft
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


def test_offset_zero_reproduces_the_plain_scores():
    """offset 0 is the plain shaping in both modes, so a selection scores the same with or without it."""
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
    """Random assignments, random pick lists with repeats, offsets of both signs and both clip modes.

    A list with a literal offset on hard (the pick's level, or the default) is refused by
    both paths with the same message, naming its first such pick; moved to soft, the same
    picks score alike."""
    rng = random.Random(11)
    classes = (BinCapacityCost, ColorConflictCost, ItemConflictCost, UsedBinsCost)
    default_level = {row.name: row.default_level for row in bp.constraint_info()}
    refused_lists = 0
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

        def literal_on_hard(spec):
            return not spec.clip and spec.offset != 0 and (spec.level or default_level[spec.name]) == "hard"

        refused = next((spec for spec in specs if literal_on_hard(spec)), None)
        if refused is not None:
            refused_lists += 1
            message = HARD_REFUSAL.format(q=repr(refused.name))
            with pytest.raises(ValueError) as engine_refusal:
                bp.calculate_score(problem, specs)
            with pytest.raises(ValueError) as mirror_refusal:
                score(bp.constraint_costs(problem), specs)
            assert str(engine_refusal.value) == str(mirror_refusal.value) == message
            specs = [
                type(spec)(weight=spec.weight, level="soft", exponent=spec.exponent, offset=spec.offset, clip=spec.clip)
                if literal_on_hard(spec)
                else spec
                for spec in specs
            ]
        engine = bp.calculate_score(problem, specs)
        python = score(bp.constraint_costs(problem), specs)
        assert python.hard == engine.hard
        assert python.soft == pytest.approx(engine.soft)
        assert score(bp.constraint_costs(problem, specs), specs) == python
    assert 0 < refused_lists < 60


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
            BinCapacityCost(level="soft", offset=1, clip=False, weight=2),
            ColorConflictCost(offset=-1),
            ColorConflictCost(level="soft", offset=0.5, clip=False, weight=3),
            ItemConflictCost(offset=1, exponent=2),
            ItemConflictCost(level="soft", offset=1, clip=False, exponent=2),
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
    """ScoreChecks::Full re-checks the incremental score against a full recompute after every move."""
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


# --- clip=False with an offset stays off the hard score -------------------------------


@pytest.mark.parametrize("offset", [3.0, -3.0])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: BinCapacityCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: BinCapacityCost(level="hard", offset=offset, clip=False),
        lambda offset: ItemConflictCost(offset=offset, clip=False),
        lambda offset: UsedBinsCost(level=Level.HARD, offset=offset, clip=False),  # a global moved to hard
    ],
    ids=["capacity-default-hard", "capacity-level-hard", "item-conflict-default-hard", "used-bins-moved-to-hard"],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """A positive offset would reward every bin below it, an empty one included, so the
    four bins with room would cancel bin 0's overload and an infeasible packing could
    score hard 0 (or above); a negative one would charge every bin. The engine (solve,
    Solver, calculate_score, constraint_costs) and the score() mirror refuse it with the
    same message, alone and as a repeat; so does the CP-SAT reference (its own tests)."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    picks = [*get_constraints(), spec]  # a repeat next to the defaults
    assert _refusal(lambda: bp.solve(build_problem(), config=_config(0.1), constraints=[spec])) == message
    assert _refusal(lambda: _zero_budget_solve(_hand(), [spec])) == message
    assert _refusal(lambda: _zero_budget_solve(_hand(), picks)) == message
    assert _refusal(lambda: bp.calculate_score(_hand(), [spec])) == message
    assert _refusal(lambda: bp.constraint_costs(_hand(), [spec])) == message
    assert _refusal(lambda: score(bp.constraint_costs(_hand()), [spec])) == message
    assert _refusal(lambda: score(bp.constraint_costs(_hand()), picks)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """Capacity [3, 0, 0, 0, 0] minus the offset on each of the five bins, on soft, on every path."""
    pick = BinCapacityCost(level="soft", offset=offset, clip=False)
    expected = (0, -(3 - 5 * offset))
    assert score(bp.constraint_costs(_hand()), [pick]) == expected
    raw = bp.constraint_costs(_hand(), [pick])
    assert (raw["bin_capacity_cost"]["level"], raw["bin_capacity_cost"]["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    assert _engine([pick]) == expected
    # Next to the hard default, which keeps "hard 0 = no violation": -3 hard.
    assert _engine([BinCapacityCost(), pick]) == (-3, expected[1])


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        (BinCapacityCost(offset=3.0), (0, 0.0)),  # clip=True on hard: a threshold
        (BinCapacityCost(offset=-3.0), (-6, 0.0)),  # 3 + 3 on bin 0 only
        (BinCapacityCost(clip=False), (-3, 0.0)),  # clip=False with offset 0: the plain shaping
        (BinCapacityCost(offset=0.0, clip=False), (-3, 0.0)),
        (UsedBinsCost(level="hard", offset=-3.0), (-5, 0.0)),
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    assert score(bp.constraint_costs(_hand()), [pick]) == expected
    assert _engine([pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(name="bin_capacity_cost", weight=None, level="soft", exponent=1.0, offset=3.0, clip=False)
    assert _engine([moved]) == (0, 12.0)
    kept = SimpleNamespace(name="bin_capacity_cost", weight=None, level=None, exponent=1.0, offset=3.0, clip=False)
    message = HARD_REFUSAL.format(q="'bin_capacity_cost'")
    assert _refusal(lambda: _engine([kept])) == message
    assert _refusal(lambda: score(bp.constraint_costs(_hand()), [kept])) == message
