from datetime import date
from itertools import product

import pytest

import maintenance_scheduling as domain
from maintenance_scheduling.constraints import (
    AfterIdealEndDateCost,
    BeforeIdealEndDateCost,
    CrewConflictCost,
    Level,
    MaxEndDateCost,
    MinStartDateCost,
    TagConflictCost,
    get_constraints,
    score,
)
from maintenance_scheduling.models import CrewModel, JobModel, MaintenanceScheduleModel
from maintenance_scheduling_cpsat import catalog, solve
from maintenance_scheduling import calculate_score


def test_avoids_crew_overlap() -> None:
    problem = MaintenanceScheduleModel(
        from_date=date(2026, 1, 5),
        to_date=date(2026, 1, 12),
        crews=[CrewModel(id="crew", name="Crew")],
        jobs=[
            JobModel(id="a", name="A", duration_in_days=2),
            JobModel(id="b", name="B", duration_in_days=2),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert (
        solved.jobs[0].end_date <= solved.jobs[1].start_date
        or solved.jobs[1].end_date <= solved.jobs[0].start_date
    )
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> MaintenanceScheduleModel:
    return MaintenanceScheduleModel(
        from_date=date(2026, 1, 5),
        to_date=date(2026, 1, 12),
        crews=[CrewModel(id="c0", name="Crew 0"), CrewModel(id="c1", name="Crew 1")],
        jobs=[
            JobModel(
                id="a",
                name="A",
                duration_in_days=3,
                min_start_date=date(2026, 1, 7),
                max_end_date=date(2026, 1, 7),
                ideal_end_date=date(2026, 1, 6),
                tags=["downtown"],
            ),
            JobModel(
                id="b",
                name="B",
                duration_in_days=1,
                ideal_end_date=date(2026, 1, 9),
                tags=["downtown", "crane"],
            ),
        ],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_fixed_jobs(constraint) -> None:
    problem = _parity_problem()
    fixed = [(0, 0), (0, 0)]
    candidate = problem.model_copy(deep=True)
    for job, (crew, _day) in zip(candidate.jobs, fixed, strict=True):
        job.crew_id = candidate.crews[crew].id
        job.start_date = candidate.from_date
    expected = -sum(
        domain.constraint_costs(candidate.to_core())[constraint.name]["costs"]
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_options=fixed,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info().

    Before 0.2.0 the plugins restated after_ideal_end_date (1'000'000) and
    tag_conflict (1'000) as ClassVars; now both solvers read the C++ constants."""
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
        assert entry["kind"] == "either"  # every maintenance constraint is flippable
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }
    assert {
        name: (row.default_level, row.default_weight) for name, row in engine.items()
    } == {
        "min_start_date_cost": ("hard", 1.0),
        "max_end_date_cost": ("hard", 1.0),
        "before_ideal_end_date_cost": ("soft", 1.0),
        "after_ideal_end_date_cost": ("soft", 1_000_000.0),
        "crew_conflict_cost": ("hard", 1.0),
        "tag_conflict_cost": ("soft", 1_000.0),
    }


def test_default_solve_scores_like_the_engine() -> None:
    """The default selection: CP-SAT's reported score is the engine's score of the same
    solution (the after_ideal_end_date_cost 1'000'000 weight comes from C++ on both)."""
    problem = _parity_problem()
    solved = solve(problem, time_limit_seconds=5)
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)
    engine = score(domain.constraint_costs(solved.to_core()))
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))


def test_demoting_max_end_date_scores_like_the_engine() -> None:
    """level="soft" lands on soft in both solvers.

    Job a may not start before Wed and must end by Wed, which its 3 days cannot meet,
    so max_end_date_cost (or min_start_date_cost) is violated in every solution.
    Demoted to soft, the hard level only keeps what the other hard rules cost, and
    CP-SAT's reported score equals the engine's score of the same solution under the
    same selection."""
    problem = _parity_problem()
    selection = [
        MaxEndDateCost(level="soft") if spec.name == "max_end_date_cost" else spec
        for spec in get_constraints()
    ]
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    raw = domain.constraint_costs(solved.to_core())
    engine = score(raw, selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert solved.score_hard == -(
        sum(raw["min_start_date_cost"]["costs"])
        + sum(raw["crew_conflict_cost"]["costs"])
    )


def test_reweighting_after_ideal_end_date_scores_like_the_engine() -> None:
    """An explicit weight replaces the engine default on both paths."""
    problem = _parity_problem()
    selection = [
        AfterIdealEndDateCost(weight=7)
        if spec.name == "after_ideal_end_date_cost"
        else spec
        for spec in get_constraints()
    ]
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------


def _three_days() -> MaintenanceScheduleModel:
    """Mon 5 - Wed 7 Jan 2026 (3 start dates) x 2 crews = 6 cells, two jobs: 36 assignments.

    a (2 days) may start Tue at the earliest, must end by Wed and ideally ends on Wed;
    b (1 day) ideally ends on Thu; both carry ``downtown``.
    """
    return MaintenanceScheduleModel(
        from_date=date(2026, 1, 5),
        to_date=date(2026, 1, 8),
        crews=[CrewModel(id="c0", name="Crew 0"), CrewModel(id="c1", name="Crew 1")],
        jobs=[
            JobModel(
                id="a",
                name="A",
                duration_in_days=2,
                min_start_date=date(2026, 1, 6),
                max_end_date=date(2026, 1, 7),
                ideal_end_date=date(2026, 1, 7),
                tags=["downtown"],
            ),
            JobModel(
                id="b",
                name="B",
                duration_in_days=1,
                ideal_end_date=date(2026, 1, 8),
                tags=["downtown", "crane"],
            ),
        ],
    )


SELECTIONS = {
    "repeat": [
        CrewConflictCost(),
        CrewConflictCost(weight=3, offset=1),
        TagConflictCost(),
    ],
    "repeat-literal": [
        MinStartDateCost(offset=1, clip=False),
        MinStartDateCost(weight=2, offset=-1),
        AfterIdealEndDateCost(),
        AfterIdealEndDateCost(weight=5, offset=1, clip=False),
    ],
    "hard-offsets": [
        MaxEndDateCost(offset=1, clip=False),
        CrewConflictCost(weight=2, offset=2, clip=False),
        TagConflictCost(offset=-1),
        BeforeIdealEndDateCost(weight=3, offset=1, clip=False),
    ],
    "identical-twice": [
        TagConflictCost(),
        TagConflictCost(),
        CrewConflictCost(),
    ],
    # Each pick on its own level: a soft literal max end next to the hard default, the
    # tag conflict on hard beyond 1 next to its soft default, crew conflict moved to soft.
    "levels": [
        MaxEndDateCost(),
        MaxEndDateCost(level="soft", weight=3, offset=1, clip=False),
        TagConflictCost(),
        TagConflictCost(level=Level.HARD, offset=1),
        CrewConflictCost(level="soft", weight=2),
        AfterIdealEndDateCost(level="hard", weight=2, offset=-1),
    ],
}

_OPTIONS = list(product(range(2), range(3)))  # (crew, start date index)


def _pinned(problem, fixed):
    dates = [date(2026, 1, 5), date(2026, 1, 6), date(2026, 1, 7)]
    candidate = problem.model_copy(deep=True)
    for job, (crew, day) in zip(candidate.jobs, fixed, strict=True):
        job.crew_id = candidate.crews[crew].id
        job.start_date = dates[day]
    return candidate.to_core()


def _engine_of(problem, fixed, selection):
    """The Python score() mirror of the pinned assignment under `selection`."""
    core = _pinned(problem, fixed)
    return tuple(score(domain.constraint_costs(core, selection), selection))


def _engine_solve(problem, fixed, selection):
    """The engine's own score of the pinned assignment: a solve with no time keeps it."""
    core = _pinned(problem, fixed)
    assignment = core.assignment()
    domain.solve(core, time_limit_seconds=0, random_seed=1, constraints=selection)
    assert core.assignment() == assignment
    return core.score.hard, core.score.soft


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_every_assignment(selection) -> None:
    """Pin each of the 36 assignments: the CP-SAT objective's own levels are the engine's
    score under the same repeated / shifted picks (its own and the score() mirror's),
    exactly (every cell is a term, an empty one included); the reported score re-prices
    the default costs (each name once) to the same value."""
    problem = _three_days()
    for fixed in product(_OPTIONS, repeat=2):
        proxy: dict[str, int] = {}
        solved = solve(
            problem,
            time_limit_seconds=5,
            constraints=selection,
            _fixed_options=list(fixed),
            _proxy_score=proxy,
        )
        engine = _engine_of(problem, fixed, selection)
        assert _engine_solve(problem, fixed, selection) == engine, fixed
        assert (proxy["hard"], proxy["soft"]) == engine, fixed
        assert (solved.score_hard, solved.score_soft) == engine, fixed


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection) -> None:
    problem = _three_days()
    best = max(
        _engine_of(problem, fixed, selection) for fixed in product(_OPTIONS, repeat=2)
    )
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == best


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, level, offset, clip} dicts pick the same instances as specs."""
    specs = [
        TagConflictCost(),
        TagConflictCost(weight=3, offset=1, clip=False),
        TagConflictCost(level="hard", offset=1),
    ]
    dicts = [
        {"name": "tag_conflict_cost"},
        {"name": "tag_conflict_cost", "weight": 3, "offset": 1, "clip": False},
        {"name": "tag_conflict_cost", "level": "hard", "offset": 1},
    ]
    problem = _three_days()
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(
            _three_days(),
            time_limit_seconds=1,
            constraints=[MinStartDateCost(offset=0.5)],
        )


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_like_the_engine(level) -> None:
    """The domain has no medium level: CP-SAT refuses the pick as the engine does, a repeat too."""
    message = "constraint 'tag_conflict_cost': this use case has no medium level"
    for selection in (
        [TagConflictCost(level=level)],
        [TagConflictCost(), TagConflictCost(level=level)],
    ):
        with pytest.raises(ValueError, match=message):
            solve(_three_days(), time_limit_seconds=1, constraints=selection)
        with pytest.raises(ValueError, match=message):
            domain.solve(
                _three_days().to_core(), time_limit_seconds=0, constraints=selection
            )
