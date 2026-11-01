from itertools import product

import pytest

import tournament_scheduling as domain
from tournament_scheduling.constraints import (
    AssignmentCountImbalanceCost,
    ConfrontationCountImbalanceCost,
    Level,
    TeamConflictCost,
    TeamUnavailabilityCost,
    get_constraints,
    score,
)
from tournament_scheduling.models import (
    DayModel,
    TeamAssignmentModel,
    TeamModel,
    TournamentScheduleModel,
    UnavailabilityPenaltyModel,
)
from tournament_scheduling_cpsat import solve
from tournament_scheduling import calculate_score


def test_team_loads_are_balanced_without_day_conflicts() -> None:
    problem = TournamentScheduleModel(
        teams=[TeamModel(name="a"), TeamModel(name="b")],
        days=[DayModel(date_index=0), DayModel(date_index=1)],
        team_assignments=[
            TeamAssignmentModel(id=0, day=0),
            TeamAssignmentModel(id=1, day=1),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert {assignment.team for assignment in solved.team_assignments} == {0, 1}
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> TournamentScheduleModel:
    return TournamentScheduleModel(
        teams=[TeamModel(name="a"), TeamModel(name="b"), TeamModel(name="c")],
        days=[DayModel(date_index=0), DayModel(date_index=1)],
        unavailability_penalties=[UnavailabilityPenaltyModel(team=0, day=0)],
        team_assignments=[
            TeamAssignmentModel(id=0, day=0, index_in_day=0),
            TeamAssignmentModel(id=1, day=0, index_in_day=1),
            TeamAssignmentModel(id=2, day=1, index_in_day=0),
            TeamAssignmentModel(id=3, day=1, index_in_day=1),
        ],
    )


def _brute_force_score(problem, constraint):
    best = None
    for teams in product(
        range(len(problem.teams)), repeat=len(problem.team_assignments)
    ):
        candidate = problem.model_copy(deep=True)
        for assignment, team in zip(candidate.team_assignments, teams, strict=True):
            assignment.team = team
        value = score(domain.constraint_costs(candidate.to_core()), [constraint])
        best = max(best, value) if best is not None else value
    return best


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_brute_force_independently(constraint) -> None:
    problem = _parity_problem()
    expected = _brute_force_score(problem, constraint)

    solved = solve(problem, time_limit_seconds=2, constraints=[constraint])

    assert (solved.score_hard, solved.score_soft) == pytest.approx(expected)


# --- 0.2.0: defaults from the engine, the knobs, and where parity holds ------


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """Every plugin reads its default level and weight from constraint_info()."""
    from tournament_scheduling_cpsat import catalog

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
        assert entry["kind"] == "either"  # every tournament constraint is flippable
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }
    levels = {
        entry["name"]: (entry["default_level"], entry["default_weight"])
        for entry in entries
    }
    # The medium weight used to be 1 here but 1e6 (the fold) in the engine.
    assert levels["assignment_count_imbalance_cost"] == ("medium", 1)


def test_each_pick_lands_on_the_level_it_names_on_cpsat_too() -> None:
    """level="soft" lands on soft from a hard OR a medium default (not on medium),
    level="hard" on hard and level="medium" on medium, a repeat on its own level."""
    from ortools.sat.python import cp_model

    from tournament_scheduling_cpsat.constraints import REGISTRY

    selection = [
        TeamConflictCost(level="soft"),
        TeamConflictCost(level=Level.MEDIUM),
        TeamUnavailabilityCost(level="medium"),
        AssignmentCountImbalanceCost(level="soft"),
        ConfrontationCountImbalanceCost(level="hard"),
    ]
    objective = REGISTRY.objective(
        cp_model.CpModel(), ("hard", "medium", "soft"), selection
    )
    levels = {
        name: objective.level_of(spec, config)
        for name, (spec, config) in objective.selected.items()
    }
    assert levels == {
        "team_conflict_cost": "soft",
        "team_conflict_cost#2": "medium",
        "team_unavailability_cost": "medium",
        "assignment_count_imbalance_cost": "soft",
        "confrontation_count_imbalance_cost": "hard",
    }
    defaults = REGISTRY.objective(cp_model.CpModel(), ("hard", "medium", "soft"), None)
    assert (
        defaults.level_of(*defaults.selected["assignment_count_imbalance_cost"])
        == "medium"
    )


@pytest.mark.parametrize(
    "selection",
    [
        [
            spec
            if spec.name != "assignment_count_imbalance_cost"
            else type(spec)(level="soft")
            for spec in get_constraints()
        ],
        [TeamConflictCost(), AssignmentCountImbalanceCost(level="hard")],
        [TeamConflictCost(), AssignmentCountImbalanceCost(weight=3)],
        [TeamUnavailabilityCost(level="soft"), ConfrontationCountImbalanceCost()],
        [TeamConflictCost(), ConfrontationCountImbalanceCost(level=Level.MEDIUM)],
        [TeamConflictCost(level="medium"), TeamUnavailabilityCost()],
    ],
    ids=[
        "medium-demoted-to-soft",
        "medium-promoted",
        "medium-reweighted",
        "hard-demoted-to-soft",
        "soft-to-medium",
        "hard-to-medium",
    ],
)
def test_knobs_score_like_the_engine(selection) -> None:
    """The reported score of a CP-SAT solve under a selection is the engine's score of
    that solution under the same selection (level="soft" soft, level="hard" hard, an
    explicit medium weight in medium units)."""
    problem = _parity_problem()

    solved = solve(problem, time_limit_seconds=2, constraints=selection)

    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))


def test_default_score_folds_medium_like_the_engine() -> None:
    """With the defaults the returned score is the engine's calculate_score (medium x 1e6)."""
    problem = _pinned(_UNEVEN)

    solved = solve(problem, time_limit_seconds=2)

    expected = domain.calculate_score(problem.to_core())
    assert expected.soft < -1e6  # the medium level is non-zero here
    assert (solved.score_hard, solved.score_soft) == (expected.hard, expected.soft)


# Fully pinned layouts as (day, team) per slot on 3 teams x 4 days.
# Loads {1, 4, 2}: ssd 14/3, unfairness 2.16025; pair counts {1, 1, 2}: ssd 2/3, 0.816497.
_UNEVEN = [(0, 0), (0, 1), (0, 2), (1, 1), (1, 2), (2, 1), (3, 1)]
# Loads {2, 2, 2}: 0; pair counts {2, 1, 1}: ssd 2/3, 0.816497.
_EVEN = [(0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (2, 2)]
# T0 twice on D0 next to T1, T1 and T2 on D1: the engine counts (0,1) twice.
_DOUBLE_BOOKED = [(0, 0), (0, 0), (0, 1), (1, 1), (1, 2)]


def _pinned(layout, penalties=()) -> TournamentScheduleModel:
    return TournamentScheduleModel(
        teams=[TeamModel(name=name) for name in ("a", "b", "c")],
        days=[DayModel(date_index=day) for day in range(4)],
        unavailability_penalties=[
            UnavailabilityPenaltyModel(team=team, day=day) for team, day in penalties
        ],
        team_assignments=[
            TeamAssignmentModel(id=index, day=day, team=team, pinned=True)
            for index, (day, team) in enumerate(layout)
        ],
    )


def _cpsat_term(problem: TournamentScheduleModel, constraint) -> int:
    """The CP-SAT objective value of one constraint alone on a fully pinned problem."""
    from ortools.sat.python import cp_model

    from tournament_scheduling_cpsat import build_model

    _, objective, _ = build_model(problem, [constraint])
    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = 10
    solver.parameters.num_search_workers = 1
    assert objective.solve(solver) == cp_model.OPTIMAL
    totals = objective.score(solver)
    return -sum(totals.values())


def _engine_raw(problem: TournamentScheduleModel, name: str) -> float:
    return sum(domain.constraint_costs(problem.to_core())[name]["costs"])


@pytest.mark.parametrize(
    ("constraint", "layout", "penalties"),
    [
        (TeamConflictCost(), _DOUBLE_BOOKED, ()),
        (TeamConflictCost(), [(0, 0), (0, 0), (0, 0), (1, 1)], ()),
        (TeamConflictCost(), _UNEVEN, ()),
        (TeamUnavailabilityCost(), _UNEVEN, [(1, 1), (1, 1), (0, 0), (2, 3)]),
        (TeamUnavailabilityCost(), _UNEVEN, [(0, 1)]),
    ],
)
def test_the_hard_constraints_are_raw_identical(constraint, layout, penalties) -> None:
    """team_conflict_cost / team_unavailability_cost add exactly the engine's raw cost."""
    problem = _pinned(layout, penalties)
    assert _cpsat_term(problem, constraint) == _engine_raw(problem, constraint.name)


def test_the_imbalance_surrogates_are_not_raw_identical() -> None:
    """The two imbalance terms minimize scale * ssd, not round6(sqrt(ssd)).

    On 3 loaded items the scale is lcm(1, 2, 3) = 6, so loads {1, 4, 2}
    (ssd 14/3) cost 28 in CP-SAT and 2.16025 in the engine, and pair counts
    {1, 1, 2} (ssd 2/3) cost 4 and 0.816497. Documented, not a parity claim."""
    problem = _pinned(_UNEVEN)
    assert _engine_raw(problem, "assignment_count_imbalance_cost") == pytest.approx(
        2.16025, abs=1e-12
    )
    assert _cpsat_term(problem, AssignmentCountImbalanceCost()) == 28
    assert _engine_raw(problem, "confrontation_count_imbalance_cost") == pytest.approx(
        0.816497, abs=1e-12
    )
    assert _cpsat_term(problem, ConfrontationCountImbalanceCost()) == 4


def test_a_single_imbalance_surrogate_orders_feasible_layouts_like_the_engine() -> None:
    """Alone, the surrogate is monotone in the engine's raw cost on hard-feasible
    layouts (every team at most once a day): same order, so the same optimum."""
    layouts = [_UNEVEN, _EVEN, [(0, 0), (0, 1), (1, 1), (2, 1), (3, 1)]]
    for constraint in (
        AssignmentCountImbalanceCost(),
        ConfrontationCountImbalanceCost(),
    ):
        pairs = sorted(
            (
                _engine_raw(_pinned(layout), constraint.name),
                _cpsat_term(_pinned(layout), constraint),
            )
            for layout in layouts
        )
        surrogate = [cpsat for _, cpsat in pairs]
        assert surrogate == sorted(surrogate), constraint.name


def test_confrontations_count_once_per_shared_day_on_cpsat() -> None:
    """While a team plays twice a day the confrontation surrogate counts the pair once
    per shared day, the engine once per same-day assignment pair: here the engine
    sees pair counts {2, 1} (0.707107) and CP-SAT {1, 1} (0)."""
    problem = _pinned(_DOUBLE_BOOKED)
    assert _engine_raw(problem, "confrontation_count_imbalance_cost") == pytest.approx(
        0.707107, abs=1e-12
    )
    assert _cpsat_term(problem, ConfrontationCountImbalanceCost()) == 0


def test_confrontation_alone_can_miss_the_engine_optimum() -> None:
    """Documented non-parity: with team_conflict_cost off, the multiplicity gap moves
    the optimum. T0 plays twice on D0 next to T1 and T2; T1 plays on D1, where one slot
    is free. The engine's best team for it is T2 (pair counts {2, 2, 2}, raw 0); CP-SAT
    counts one confrontation per shared day and prefers T1 (surrogate 0, engine raw
    0.816497). The reported score is still the engine's raw cost of that answer."""
    pinned = [(0, 0), (0, 0), (0, 1), (0, 2), (1, 1)]
    constraint = ConfrontationCountImbalanceCost()

    def problem_with(free_team):
        problem = _pinned(pinned)
        return TournamentScheduleModel(
            teams=problem.teams,
            days=problem.days,
            team_assignments=[
                *problem.team_assignments,
                TeamAssignmentModel(id=len(pinned), day=1, team=free_team),
            ],
        )

    engine = {
        team: _engine_raw(problem_with(team), constraint.name) for team in range(3)
    }
    assert engine == pytest.approx({0: 1.41421, 1: 0.816497, 2: 0.0}, abs=1e-12)

    solved = solve(problem_with(None), time_limit_seconds=2, constraints=[constraint])

    assert solved.team_assignments[-1].team == 1
    assert (solved.score_hard, solved.score_soft) == pytest.approx((0, -engine[1]))
    assert -solved.score_soft > min(engine.values())


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------

#: Fully pinned layouts with unavailability facts: T0 twice on D0, T2 on D3. T1 has no
#: fact, so the scoped team_unavailability_cost evaluates (and offsets charge) T0 and T2.
_FACTS = [(0, 0), (0, 0), (2, 3)]
_LAYOUTS = [_UNEVEN, _EVEN, _DOUBLE_BOOKED, [(0, 0), (0, 0), (0, 0), (1, 1)]]

HARD_SELECTIONS = {
    "repeat": [
        TeamConflictCost(),
        TeamConflictCost(weight=2),
        TeamUnavailabilityCost(),
        TeamUnavailabilityCost(),
    ],
    "offsets": [
        TeamConflictCost(offset=1),
        TeamConflictCost(offset=1, clip=False),
        TeamUnavailabilityCost(offset=-1),
        TeamUnavailabilityCost(weight=2, offset=3, clip=False),
    ],
    "soft-offsets": [
        TeamConflictCost(level="soft", offset=2, clip=False),
        TeamUnavailabilityCost(level="soft", offset=1),
        TeamUnavailabilityCost(level="soft", offset=-2, clip=False, weight=3),
    ],
    # Each pick on its own level: the same-day pairs on hard and (literal) on medium,
    # the facts hit on medium beyond one per team and (literal) on soft.
    "levels": [
        TeamConflictCost(),
        TeamConflictCost(level=Level.MEDIUM, offset=1, clip=False),
        TeamUnavailabilityCost(level="medium", weight=2, offset=1),
        TeamUnavailabilityCost(level="soft", offset=3, clip=False),
    ],
}


def _engine_solve(core, selection) -> tuple[int, float]:
    """The engine's own score of the fully pinned `core` under `selection` (a solve with
    no time keeps every assignment)."""
    kept = core.assignment()
    config = domain.SolverConfig()
    config.spent_limit_seconds = 0
    domain.Solver(config).solve_with_constraints(core, selection)
    assert core.assignment() == kept
    return core.score.hard, core.score.soft


@pytest.mark.parametrize(
    "selection", list(HARD_SELECTIONS.values()), ids=list(HARD_SELECTIONS)
)
def test_the_hard_terms_equal_the_engine_with_repeats_and_offsets(selection) -> None:
    """Pin each layout: the CP-SAT objective of the two raw-identical plugins equals the
    engine's score under the same repeated / shifted picks (its own and the score()
    mirror's), exactly (one term per team, the scoped unavailability constraint only for
    the teams with a fact), the medium level folded at MEDIUM_SCALE."""
    from ortools.sat.python import cp_model

    from tournament_scheduling_cpsat import build_model

    for layout in _LAYOUTS:
        problem = _pinned(layout, _FACTS)
        _, objective, _ = build_model(problem, selection)
        solver = cp_model.CpSolver()
        solver.parameters.num_search_workers = 1
        assert objective.solve(solver) == cp_model.OPTIMAL
        levels = objective.score(solver)
        core = problem.to_core()
        engine = score(domain.constraint_costs(core, selection), selection)
        assert _engine_solve(core, selection) == tuple(engine), layout
        folded = levels["medium"] * domain.MEDIUM_SCALE + levels["soft"]
        assert (levels["hard"], folded) == tuple(engine), layout
        assert (
            objective.score_costs(domain.constraint_costs(problem.to_core())) == levels
        )


def test_repeated_imbalance_surrogates_solve_and_report_the_engine_score() -> None:
    """A repeated imbalance pick adds a second surrogate; the reported score is the
    engine's, under the same repeated selection."""
    selection = [
        *get_constraints(),
        AssignmentCountImbalanceCost(weight=2),
        ConfrontationCountImbalanceCost(level="soft", weight=3),
    ]
    solved = solve(_parity_problem(), time_limit_seconds=5, constraints=selection)
    engine = score(domain.constraint_costs(solved.to_core(), selection), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))


def test_an_imbalance_offset_is_refused_by_the_surrogate() -> None:
    """The surrogates have no raw-identical total to shift: CP-SAT refuses an offset there."""
    for pick in (
        AssignmentCountImbalanceCost(offset=1),
        ConfrontationCountImbalanceCost(offset=1, clip=False),
    ):
        with pytest.raises(
            ValueError, match="offset is not supported by this CP-SAT model"
        ):
            solve(_parity_problem(), time_limit_seconds=1, constraints=[pick])


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, offset, clip} dicts pick the same instances as specs."""
    specs = [
        TeamConflictCost(),
        TeamUnavailabilityCost(),
        TeamUnavailabilityCost(weight=3, offset=1, clip=False),
        AssignmentCountImbalanceCost(),
    ]
    dicts = [
        {"name": "team_conflict_cost"},
        {"name": "team_unavailability_cost"},
        {"name": "team_unavailability_cost", "weight": 3, "offset": 1, "clip": False},
        {"name": "assignment_count_imbalance_cost"},
    ]
    problem = _parity_problem()
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(
            _parity_problem(),
            time_limit_seconds=1,
            constraints=[TeamConflictCost(offset=0.5)],
        )
