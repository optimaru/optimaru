"""order_picking.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the five knobs (the level included) and repeated picks are
covered here.
Per-constraint values are covered by `test_order_picking.py` and the C++ tests.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import order_picking as op
from order_picking._constraint import instance_keys
from order_picking.constraints import (
    Constraint,
    DistanceFromLastPickTaskToPathOriginCost,
    DistanceFromPreviousPickTaskCost,
    Level,
    OrderSplitByTrolleyCost,
    TrolleyCapacityCost,
    distance_from_last_pick_task_to_path_origin_cost,
    distance_from_previous_pick_task_cost,
    get_constraints,
    order_split_by_trolley_cost,
    score,
    trolley_capacity_cost,
)


def build_solution():
    return op.build_demo_data()


def solve(constraints=None):
    solution = build_solution()
    op.solve(solution, time_limit_seconds=0.5, random_seed=1, constraints=constraints)
    return solution.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    available = op.list_constraints()
    assert available == [
        "trolley_capacity_cost",
        "distance_from_previous_pick_task_cost",
        "distance_from_last_pick_task_to_path_origin_cost",
        "order_split_by_trolley_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == op.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring.

    Whether both paths reach the same **solution** cannot be checked: the
    order_consolidate_moves operator does not run on the plugin path (it reads
    the monolithic calculator's live volume table — cpp/include/op/solver.hpp)."""
    solution = build_solution()
    op.solve(
        solution,
        time_limit_seconds=0.5,
        random_seed=1,
        constraints=get_constraints(),
    )
    recomputed = op.calculate_score(solution)
    assert solution.score.hard == recomputed.hard
    assert solution.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the split term disappears entirely."""
    baseline = solve()
    assert baseline.soft < 0

    distance_only = solve(
        [
            trolley_capacity_cost,
            distance_from_previous_pick_task_cost,
            distance_from_last_pick_task_to_path_origin_cost,
        ]
    )
    # The split penalty (weight 1000) is gone, so soft is much less negative.
    assert distance_only.soft > baseline.soft


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the split weight grows soft only; hard stays put."""
    picks = [trolley_capacity_cost]
    base = solve([*picks, order_split_by_trolley_cost])
    heavier = solve([*picks, OrderSplitByTrolleyCost(weight=100_000)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level.

    Every pick has to ride somewhere, so (order, trolley) pairs can never be 0 —
    raising the level always lands on hard."""
    soft_level = solve([trolley_capacity_cost, order_split_by_trolley_cost])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([trolley_capacity_cost, OrderSplitByTrolleyCost(level="hard")])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "order_split_by_trolley_costs"  # trailing s
        weight = None
        level = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        op.solve(build_solution(), time_limit_seconds=0.2, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    twice = [TrolleyCapacityCost(), OrderSplitByTrolleyCost(), OrderSplitByTrolleyCost(weight=10)]
    assert _zero_budget(twice) == (-1, -(1000 + 10) * 3.0)
    assert get_constraints(twice) == twice
    assert get_constraints([TrolleyCapacityCost(), TrolleyCapacityCost()]) == [
        TrolleyCapacityCost(),
        TrolleyCapacityCost(),
    ]


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    for spec in get_constraints():
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
    assert get_constraints() == [
        trolley_capacity_cost,
        distance_from_previous_pick_task_cost,
        distance_from_last_pick_task_to_path_origin_cost,
        order_split_by_trolley_cost,
    ]


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = op.constraint_info()
    assert [row.name for row in rows] == op.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
        ("soft", 1000.0, True, False, True),
    ]


def test_constraint_info_rows_are_plain_values():
    row = op.constraint_info()[0]
    assert isinstance(row, op.ConstraintInfo)
    assert row._asdict() == {
        "name": "trolley_capacity_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='trolley_capacity_cost'")
    assert pickle.loads(pickle.dumps(op.constraint_info())) == op.constraint_info()
    assert op.ConstraintInfo.__module__ == op._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint."""
    raw = op.constraint_costs(build_solution())
    for row in op.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"], entry["global"]) == (True, False, False)
        assert (entry["name"], entry["label"]) == (row.name, "")  # one instance per name, no label
        assert "medium_fold" not in entry  # no medium level in this domain


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights 1 / 1 / 1 / 1000."""
    explicit = [
        TrolleyCapacityCost(weight=1),
        DistanceFromPreviousPickTaskCost(weight=1),
        DistanceFromLastPickTaskToPathOriginCost(weight=1),
        OrderSplitByTrolleyCost(weight=1000),
    ]
    raw, solution = _raw()
    engine = op.calculate_score(solution)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (engine.hard, engine.soft)


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
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        OrderSplitByTrolleyCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""

    class Negative:
        name = "order_split_by_trolley_cost"
        weight = -2.0
        level = None
        exponent = 1.0

    class Flat:
        name = "order_split_by_trolley_cost"
        weight = None
        level = None
        exponent = 0.0

    class Unbounded:
        name = "order_split_by_trolley_cost"
        weight = None
        level = None
        exponent = 1.0
        offset = float("inf")

    for pick, message in (
        (Negative(), "weight must be finite and >= 0"),
        (Flat(), "exponent must be finite and > 0"),
        (Unbounded(), "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            op.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        OrderSplitByTrolleyCost(offset=float("nan"))


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
    pick = SimpleNamespace(name="order_split_by_trolley_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'order_split_by_trolley_cost': {re.escape(message)}"):
        op.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        OrderSplitByTrolleyCost(**knobs)


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    for old in (
        "required_number_of_buckets",
        "minimize_distance_from_previous_pick_task",
        "minimize_distance_from_last_pick_task_to_path_origin",
        "minimize_order_split_by_trolley",
    ):

        class Old:
            name = old
            weight = None
            level = None
            exponent = 1.0

        with pytest.raises(ValueError, match="unknown constraint"):
            op.solve(build_solution(), time_limit_seconds=0.1, constraints=[Old()])
    for gone in ("ConstraintTotals", "calculate_totals"):
        assert not hasattr(op, gone)
        assert not hasattr(op._core, gone)


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw(constraints=None):
    """Raw per-constraint costs of one solved solution."""
    solution = build_solution()
    op.solve(solution, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return op.constraint_costs(solution), solution


def test_score_reproduces_the_engine_score_exactly():
    """Python scoring must match the engine's hard/soft exactly.

    This domain has only two levels, so the comparison needs no folding. This
    assertion pins down, in this package, that `score()` reproduces the engine's
    applyRaw arithmetic (weights, shaping, per-unit hard rounding)."""
    raw, solution = _raw()
    engine = op.calculate_score(solution)
    python = score(raw)
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        [TrolleyCapacityCost(level="soft"), *get_constraints()[1:]],
        [*get_constraints()[:3], OrderSplitByTrolleyCost(weight=10, exponent=2.0)],
        [TrolleyCapacityCost(exponent=2.0), DistanceFromPreviousPickTaskCost(level="hard", weight=0.5)],
        [*get_constraints(), DistanceFromPreviousPickTaskCost(weight=0.5, exponent=2.0)],
        [
            TrolleyCapacityCost(),
            TrolleyCapacityCost(offset=1, clip=False, weight=2),
            DistanceFromPreviousPickTaskCost(offset=100),
            DistanceFromPreviousPickTaskCost(offset=-5, clip=False, weight=0.25),
            DistanceFromLastPickTaskToPathOriginCost(offset=30, clip=False),
            OrderSplitByTrolleyCost(),
            OrderSplitByTrolleyCost(weight=200, offset=2, exponent=2),
        ],
    ],
    ids=["demoted", "squared-split", "shaped", "repeat", "repeat-and-offsets"],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score.

    exponent applies per trolley, so reproducing it needs the per-unit raw costs."""
    raw, solution = _raw(specs)
    python = score(raw, specs)
    assert python.hard == solution.score.hard
    assert python.soft == pytest.approx(solution.score.soft)
    picked = op.constraint_costs(solution, specs)
    assert len(picked) == len(specs)
    assert score(picked, specs) == python  # the picks' own costs, repeats keyed name#k


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
    base = score(raw, [order_split_by_trolley_cost])
    ten = score(raw, [OrderSplitByTrolleyCost(weight=10)])
    default_weight = raw[order_split_by_trolley_cost.name]["weight"]
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


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def _hand_solution():
    """Two orders, three trolleys parked at A1L0: t0 (2 buckets of 10) picks order 0 at
    A1L2 (6) then order 1 at A1L5 (7); t1 (1 bucket of 10) picks order 0 at A1L5 (15,
    two buckets); t2 stays empty.

    Raw per trolley: walk [5, 5, 0], walk back [5, 5, 0], orders [2, 1, 0], buckets
    short [0, 1, 0].
    """
    origin = op.WarehouseLocation(0, 0, op.Side.LEFT, 0)
    trolleys = [op.Trolley(f"t{i}", buckets, 10, origin) for i, buckets in enumerate((2, 1, 1))]
    tasks = [
        op.PickTask("a", 0, 6, op.WarehouseLocation(0, 0, op.Side.LEFT, 2)),
        op.PickTask("b", 1, 7, op.WarehouseLocation(0, 0, op.Side.LEFT, 5)),
        op.PickTask("c", 0, 15, op.WarehouseLocation(0, 0, op.Side.LEFT, 5)),
    ]
    solution = op.OrderPickingSolution([op.Order("o0"), op.Order("o1")], trolleys, tasks)
    solution.trolleys[0].pick_task_indices = [0, 1]
    solution.trolleys[1].pick_task_indices = [2]
    return solution


def _zero_budget(picks):
    """The engine's score of the hand solution under `picks`: a solve with no time keeps it."""
    solution = op.solve(_hand_solution(), time_limit_seconds=0, random_seed=1, constraints=picks)
    assert [list(trolley.pick_task_indices) for trolley in solution.trolleys] == [[0, 1], [2], []]
    return solution.score.hard, solution.score.soft


REPEAT = [DistanceFromPreviousPickTaskCost(), DistanceFromPreviousPickTaskCost(weight=0.5, exponent=2)]


def test_a_repeated_pick_by_hand():
    """walk plus 0.5 x walk^2 per trolley: 10 + 0.5 x (25 + 25) = 35."""
    raw = op.constraint_costs(_hand_solution(), REPEAT)
    assert list(raw) == ["distance_from_previous_pick_task_cost", "distance_from_previous_pick_task_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [5.0, 5.0, 0.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "distance_from_previous_pick_task_cost"
    assert "units" not in first  # every trolley is evaluated, an empty one included
    expected = (0, -35.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(op.constraint_costs(_hand_solution()), REPEAT) == expected
    assert _zero_budget(REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [OrderSplitByTrolleyCost(), OrderSplitByTrolleyCost()]
    assert score(op.constraint_costs(_hand_solution()), picks) == (0, -6000.0)
    assert _zero_budget(picks) == (0, -6000.0)
    assert list(op.constraint_costs(_hand_solution(), picks)) == [
        "order_split_by_trolley_cost",
        "order_split_by_trolley_cost#2",
    ]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        OrderSplitByTrolleyCost(weight=1),
        TrolleyCapacityCost(),
        OrderSplitByTrolleyCost(weight=2),
        DistanceFromPreviousPickTaskCost(),
        OrderSplitByTrolleyCost(weight=3),
    ]
    raw = op.constraint_costs(_hand_solution(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "trolley_capacity_cost": 1.0,
        "distance_from_previous_pick_task_cost": 1.0,
        "order_split_by_trolley_cost": 1.0,
        "order_split_by_trolley_cost#2": 2.0,
        "order_split_by_trolley_cost#3": 3.0,
    }
    assert [key for key in raw if key.startswith("order")] == [
        "order_split_by_trolley_cost",
        "order_split_by_trolley_cost#2",
        "order_split_by_trolley_cost#3",
    ]
    expected = (-1, -(10 + 6 * 3))
    assert score(raw, picks) == expected
    assert _zero_budget(picks) == expected


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Per trolley; t2 is empty (raw 0 everywhere).
        (DistanceFromPreviousPickTaskCost(offset=3), (0, -4.0)),
        (DistanceFromPreviousPickTaskCost(offset=3, clip=False), (0, -1.0)),  # 2 + 2 - 3: t2 is rewarded
        (DistanceFromPreviousPickTaskCost(offset=10), (0, 0.0)),
        (DistanceFromPreviousPickTaskCost(offset=4, exponent=2, clip=False), (0, -(1 + 1 - 16))),
        (OrderSplitByTrolleyCost(offset=-1), (0, -5000.0)),  # a negative offset adds to every non-zero trolley
        (OrderSplitByTrolleyCost(offset=-1, clip=False), (0, -6000.0)),  # ...or to every trolley
        (OrderSplitByTrolleyCost(offset=1, exponent=3), (0, -1000.0)),  # (2 - 1)^3 on t0 only
        (TrolleyCapacityCost(offset=3, clip=False), (8, 0.0)),  # -3 -2 -3: a hard reward below 3
        (TrolleyCapacityCost(offset=1.5, clip=False), (5, 0.0)),  # -1.5 -> -2, -0.5 -> -1: per trolley
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(op.constraint_costs(_hand_solution()), [pick]) == expected
    raw = op.constraint_costs(_hand_solution(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _zero_budget([pick]) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [TrolleyCapacityCost(), OrderSplitByTrolleyCost(), DistanceFromPreviousPickTaskCost(exponent=2)]
    shifted = [
        TrolleyCapacityCost(offset=0.0, clip=False),
        OrderSplitByTrolleyCost(offset=0),
        DistanceFromPreviousPickTaskCost(exponent=2, offset=0.0, clip=False),
    ]
    raw = op.constraint_costs(_hand_solution())
    assert score(raw, shifted) == score(raw, plain) == (-1, -(3000 + 50))
    assert _zero_budget(shifted) == _zero_budget(plain) == score(raw, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), DistanceFromPreviousPickTaskCost(weight=0.5, exponent=2)],
        [
            TrolleyCapacityCost(),
            TrolleyCapacityCost(offset=1, clip=False, weight=2),
            DistanceFromPreviousPickTaskCost(),
            DistanceFromPreviousPickTaskCost(offset=40, exponent=2, weight=0.01, clip=False),
            DistanceFromLastPickTaskToPathOriginCost(offset=-3),
            OrderSplitByTrolleyCost(offset=1),
            OrderSplitByTrolleyCost(weight=300, offset=-2, clip=False),
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    solution = build_solution()
    config = op.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 3000
    config.spent_limit_seconds = 30
    op.Solver(config).solve_with_constraints(solution, picks)
    raw = op.constraint_costs(solution, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == solution.score.hard
    assert python.soft == pytest.approx(solution.score.soft)
    assert score(op.constraint_costs(solution), picks) == python  # the default costs score the repeats too


# --- Levels: hard and soft, each pick (a repeat included) on the level it names ------


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        ([OrderSplitByTrolleyCost(level="hard")], (-3000, 0.0)),
        ([TrolleyCapacityCost(level=Level.SOFT, weight=5)], (0, -5.0)),
        ([DistanceFromPreviousPickTaskCost(level=Level.HARD, weight=2)], (-20, 0.0)),
        ([TrolleyCapacityCost(level="hard")], (-1, 0.0)),  # the default level, named
        # A repeat carries its own level and shaping: the soft walk, the hard meters beyond
        # 3 per trolley (2 + 2), and hard 2 x (x - 6) per trolley: 2 x (-1 - 1 - 6) = -16.
        (
            [
                DistanceFromPreviousPickTaskCost(),
                DistanceFromPreviousPickTaskCost(level="hard", offset=3),
                DistanceFromPreviousPickTaskCost(level="hard", weight=2, offset=6, clip=False),
            ],
            (12, -10.0),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(op.constraint_costs(_hand_solution()), picks) == expected
    raw = op.constraint_costs(_hand_solution(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert _zero_budget(picks) == expected


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_without_a_medium_level(level):
    """The domain has no medium level: every path refuses the pick before scoring, a repeat too."""
    message = "constraint 'order_split_by_trolley_cost': this use case has no medium level"
    pick = OrderSplitByTrolleyCost(level=level)  # the spec cannot know the domain
    for picks in ([pick], [OrderSplitByTrolleyCost(), pick]):
        with pytest.raises(ValueError, match=message):
            _zero_budget(picks)
        with pytest.raises(ValueError, match=message):
            op.constraint_costs(_hand_solution(), picks)
        with pytest.raises(ValueError, match=message):
            score(op.constraint_costs(_hand_solution()), picks)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        OrderSplitByTrolleyCost(level=False)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        OrderSplitByTrolleyCost(level="firm")
    pick = SimpleNamespace(name="order_split_by_trolley_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        _zero_budget([OrderSplitByTrolleyCost(), pick])


def test_the_knob_was_renamed():
    with pytest.raises(TypeError):
        OrderSplitByTrolleyCost(hard=True)
