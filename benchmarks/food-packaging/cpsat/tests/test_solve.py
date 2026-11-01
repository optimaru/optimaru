from datetime import date, datetime, timedelta

from food_packaging.models import (
    JobModel,
    LineModel,
    OperatorModel,
    PackagingScheduleModel,
    ProductModel,
)
import pytest

import food_packaging as domain
from food_packaging import calculate_score, constraint_costs
from food_packaging.constraints import (
    AfterIdealEndTimeCost,
    MaxEndTimeCost,
    OperatorCleaningConflictCost,
    SquaredMakespanCost,
    UnassignedJobsCost,
    get_constraints,
    score,
    squared_makespan_cost,
    unassigned_jobs_cost,
)
from food_packaging_cpsat import catalog, solve

MEDIUM_UNIT = 10_000_000_000  # the engine domain's medium fold (into soft)


def test_smoke_and_plugin_catalog():
    product = ProductModel(
        id="P", name="Product", cleaning_durations={"P": timedelta(0)}
    )
    problem = PackagingScheduleModel(
        from_date=date(2026, 1, 1),
        to_date=date(2026, 1, 2),
        products=[product],
        operators=[OperatorModel(id="O")],
        lines=[LineModel(id="L", name="Line", start_date_time=datetime(2026, 1, 1))],
        jobs=[
            JobModel(
                id="J",
                name="Job",
                product_id="P",
                duration=timedelta(minutes=30),
                min_start_time=datetime(2026, 1, 1),
                ideal_end_time=datetime(2026, 1, 1, 1),
                max_end_time=datetime(2026, 1, 1, 2),
            )
        ],
    )
    result = solve(problem, time_limit_seconds=2)
    assert result.lines[0].job_ids == ["J"]
    assert result.jobs[0].end_date_time == datetime(2026, 1, 1, 0, 30)
    assert result.score_hard == 0
    assert len(catalog()) == 5
    shared = calculate_score(result.to_core())
    assert (result.score_hard, result.score_medium, result.score_soft) == (
        shared.hard,
        shared.medium,
        shared.soft,
    )


def test_large_horizon_keeps_objective_within_cp_sat_integer_domain():
    start = datetime(2026, 1, 1)
    product = ProductModel(
        id="P", name="Product", cleaning_durations={"P": timedelta(0)}
    )
    problem = PackagingScheduleModel(
        from_date=start.date(),
        to_date=(start + timedelta(minutes=200_000)).date(),
        products=[product],
        operators=[OperatorModel(id="O")],
        lines=[LineModel(id="L", name="Line", start_date_time=start)],
        jobs=[
            JobModel(
                id=f"J{index}",
                name=f"Job {index}",
                product_id="P",
                duration=timedelta(minutes=30),
                min_start_time=start,
                ideal_end_time=start + timedelta(minutes=200_000),
                max_end_time=start + timedelta(minutes=200_000),
            )
            for index in range(10)
        ],
    )

    result = solve(problem, time_limit_seconds=2)

    assert result.score_hard == 0
    assert len(result.lines[0].job_ids) == 10


def _two_line_problem() -> PackagingScheduleModel:
    """Two lines, two operators, five jobs with deadlines in reach (cleanups 40 / 60)."""
    start = datetime(2026, 1, 1)
    products = [
        ProductModel(
            id="A",
            name="A",
            cleaning_durations={"A": timedelta(0), "B": timedelta(minutes=60)},
        ),
        ProductModel(
            id="B",
            name="B",
            cleaning_durations={"A": timedelta(minutes=40), "B": timedelta(0)},
        ),
    ]

    def job(identifier, product, duration, ideal, maximum):
        return JobModel(
            id=identifier,
            name=identifier,
            product_id=product,
            duration=timedelta(minutes=duration),
            min_start_time=start,
            ideal_end_time=start + timedelta(minutes=ideal),
            max_end_time=start + timedelta(minutes=maximum),
        )

    return PackagingScheduleModel(
        from_date=start.date(),
        to_date=(start + timedelta(days=1)).date(),
        products=products,
        operators=[OperatorModel(id="O1"), OperatorModel(id="O2")],
        lines=[
            LineModel(id="L1", name="L1", start_date_time=start),
            LineModel(id="L2", name="L2", start_date_time=start),
        ],
        jobs=[
            job("J0", "B", 10, 5, 8),
            job("J1", "A", 100, 80, 100),
            job("J2", "B", 30, 20, 25),
            job("J3", "A", 100, 100, 110),
            job("J4", "A", 50, 20, 30),
        ],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_cpsat_constraint_matches_shared_raw_cost(constraint):
    problem = _two_line_problem()
    proxy: dict[str, int] = {}
    result = solve(
        problem,
        time_limit_seconds=3,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_line_orders=[[0, 1], [2, 3]],
        _fixed_operator_by_line=[0, 0],
        _proxy_score=proxy,
    )

    expected = -sum(constraint_costs(result.to_core())[constraint.name]["costs"])
    assert sum(proxy.values()) == expected


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info()."""
    engine = {row.name: row for row in domain.constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == domain.list_constraints()
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == "either"  # every food-packaging constraint is flippable
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }
    # The two medium constraints sit on the medium level at weight 1 (medium units).
    assert {
        name: (row.default_level, row.default_weight) for name, row in engine.items()
    } == {
        "max_end_time_cost": ("hard", 1.0),
        "after_ideal_end_time_cost": ("medium", 1.0),
        "squared_makespan_cost": ("soft", 1.0),
        "operator_cleaning_conflict_cost": ("hard", 1.0),
        "unassigned_jobs_cost": ("medium", 1.0),
    }


def _one_job_one_line(deadline_minutes: int = 24 * 60) -> PackagingScheduleModel:
    """One line, one 100-minute job: on the line it costs 100^2 = 10000 soft
    (squared_makespan_cost); left out it costs its 100 unassigned minutes."""
    start = datetime(2026, 1, 1)
    product = ProductModel(
        id="P", name="Product", cleaning_durations={"P": timedelta(0)}
    )
    return PackagingScheduleModel(
        from_date=start.date(),
        to_date=(start + timedelta(days=1)).date(),
        products=[product],
        operators=[OperatorModel(id="O")],
        lines=[LineModel(id="L", name="Line", start_date_time=start)],
        jobs=[
            JobModel(
                id="J",
                name="Job",
                product_id="P",
                duration=timedelta(minutes=100),
                min_start_time=start,
                ideal_end_time=start + timedelta(minutes=deadline_minutes),
                max_end_time=start + timedelta(minutes=deadline_minutes),
            )
        ],
    )


def _both_paths(selection):
    """The level triple each solver reports for the same selection."""
    problem = _one_job_one_line()
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    cpsat = (solved.score_hard, solved.score_medium, solved.score_soft)
    core = problem.to_core()
    domain.solve(core, time_limit_seconds=0.3, random_seed=1, constraints=selection)
    engine = (core.score.hard, core.score.medium, core.score.soft)
    return solved, cpsat, engine


@pytest.mark.parametrize(
    ("selection", "expected", "assigned"),
    [
        # Default (medium): the unassigned minutes dominate the soft makespan.
        ([unassigned_jobs_cost, squared_makespan_cost], (0, 0, -10000), True),
        # level="soft" demotes medium to SOFT on both paths (before 0.2.0 CP-SAT moved
        # it to medium and the engine ignored it): 100 soft beats 10000 soft.
        ([UnassignedJobsCost(level="soft"), squared_makespan_cost], (0, 0, -100), False),
        # level="hard": one hard unit per minute, not 10^10.
        (
            [UnassignedJobsCost(level="hard"), SquaredMakespanCost(level="hard")],
            (-100, 0, 0),
            False,
        ),
    ],
    ids=["default-medium", "medium-demoted", "medium-promoted"],
)
def test_medium_knobs_score_like_the_engine(selection, expected, assigned) -> None:
    solved, cpsat, engine = _both_paths(selection)
    assert cpsat == expected
    assert engine == expected
    assert (solved.lines[0].job_ids == ["J"]) is assigned
    folded = score(constraint_costs(solved.to_core()), selection)
    assert (cpsat[0], cpsat[1] * MEDIUM_UNIT + cpsat[2]) == pytest.approx(tuple(folded))


def test_demoting_a_hard_constraint_scores_like_the_engine() -> None:
    """level="soft" on a hard constraint lands on soft in both solvers too."""
    problem = _one_job_one_line(deadline_minutes=60)  # the job ends 40 minutes late
    selection = [MaxEndTimeCost(level="soft"), *get_constraints()[1:]]
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    engine = score(constraint_costs(solved.to_core()), selection)
    assert solved.score_hard == 0
    assert (
        solved.score_hard,
        solved.score_medium * MEDIUM_UNIT + solved.score_soft,
    ) == pytest.approx(tuple(engine))


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------

#: Pinned layouts of _two_line_problem(): (job order per line, operator per line). They
#: cover an empty line, pooled jobs, a shared operator (cleaning conflicts) and lateness.
LAYOUTS = [
    ([[0, 1], [2, 3]], [0, 0]),
    ([[0, 1, 2], []], [0, 1]),
    ([[4, 3], [2, 1, 0]], [1, 1]),
    ([[2, 0, 4], [3]], [0, 0]),
    ([[], []], [0, 1]),
]

SELECTIONS = {
    "repeat": [
        MaxEndTimeCost(),
        MaxEndTimeCost(weight=2),
        SquaredMakespanCost(),
        SquaredMakespanCost(),
        UnassignedJobsCost(),
        OperatorCleaningConflictCost(),
    ],
    "offsets": [
        MaxEndTimeCost(offset=5),
        MaxEndTimeCost(offset=10, clip=False),
        AfterIdealEndTimeCost(offset=-3),
        SquaredMakespanCost(offset=5000, clip=False),
        UnassignedJobsCost(offset=60, clip=False),
        OperatorCleaningConflictCost(weight=3, offset=2, clip=False),
    ],
    "hard-offsets": [
        MaxEndTimeCost(weight=2, offset=3, clip=False),
        AfterIdealEndTimeCost(level="hard", offset=20),
        UnassignedJobsCost(level="hard", offset=-1, clip=False),
        OperatorCleaningConflictCost(offset=-2),
    ],
}


def _folded(levels) -> tuple[int, float]:
    """(hard, medium, soft) -> the engine-level (hard, medium * 10^10 + soft)."""
    return levels["hard"], levels["medium"] * MEDIUM_UNIT + levels["soft"]


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_pinned_layouts(selection) -> None:
    """Pin each layout: the CP-SAT objective's per-level value is the engine's score of
    that schedule under the same repeated / shifted picks, exactly (every instance adds
    its own terms; the end-time and makespan terms are per line under an offset, the
    globals' one per total)."""
    problem = _two_line_problem()
    for orders, operators in LAYOUTS:
        proxy: dict[str, int] = {}
        result = solve(
            problem,
            time_limit_seconds=5,
            constraints=selection,
            _fixed_line_orders=orders,
            _fixed_operator_by_line=operators,
            _proxy_score=proxy,
        )
        assert [line.job_ids for line in result.lines] == [
            [problem.jobs[job].id for job in order] for order in orders
        ]
        core = result.to_core()
        engine = score(constraint_costs(core, selection), selection)
        assert _folded(proxy) == tuple(engine), orders
        assert (
            result.score_hard,
            result.score_medium * MEDIUM_UNIT + result.score_soft,
        ) == tuple(engine)


def test_repeated_and_shifted_picks_solve_like_the_engine() -> None:
    """One job, one line, the unassigned term picked twice with a literal offset of 150:
    running the job (0 unassigned minutes) rewards (0 - 150) x 2 = 300 medium, leaving it
    out only (100 - 150) x 2 = 100, so both solvers run it and pay its 100^2 soft."""
    selection = [
        SquaredMakespanCost(),
        UnassignedJobsCost(offset=150, clip=False),
        UnassignedJobsCost(offset=150, clip=False),
    ]
    solved, cpsat, engine = _both_paths(selection)
    assert cpsat == engine == (0, 300, -10000)
    assert solved.lines[0].job_ids == ["J"]


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, offset, clip} dicts pick the same instances as specs."""
    specs = [
        SquaredMakespanCost(),
        UnassignedJobsCost(),
        UnassignedJobsCost(weight=3, offset=40, clip=False),
    ]
    dicts = [
        {"name": "squared_makespan_cost"},
        {"name": "unassigned_jobs_cost"},
        {"name": "unassigned_jobs_cost", "weight": 3, "offset": 40, "clip": False},
    ]
    problem = _one_job_one_line()
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(
            _one_job_one_line(),
            time_limit_seconds=1,
            constraints=[SquaredMakespanCost(offset=0.5)],
        )
