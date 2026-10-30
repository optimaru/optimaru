import pytest

from semiconductor_scheduling import (
    SolverConfig,
    _construct,
    _operation_rows,
    _seed_state,
    calculate_score,
    constraint_costs,
    solve,
    with_initial_solution,
)
from semiconductor_scheduling.models import (
    AssignmentModel,
    LotModel,
    MachineModel,
    MachineOptionModel,
    OperationModel,
    SemiconductorScheduleModel,
)


def _no_search() -> SolverConfig:
    return SolverConfig(max_iterations=0)


def _option(machine_id: str, duration: int, preference_cost: int = 0) -> MachineOptionModel:
    return MachineOptionModel(machine_id=machine_id, duration=duration, preference_cost=preference_cost)


def _assignment(lot_id: str, operation_id: str, machine_id: str, start: int, end: int, **fields) -> dict:
    return dict(lot_id=lot_id, operation_id=operation_id, machine_id=machine_id, start=start, end=end, **fields)


def _queue_problem(**fields) -> SemiconductorScheduleModel:
    """L1 must start B within one time unit of finishing A, and L2 holds M2 first."""
    return SemiconductorScheduleModel(
        machines=[MachineModel(id="M1", group="A"), MachineModel(id="M2", group="B")],
        lots=[
            LotModel(
                id="L1",
                product="P",
                due_time=50,
                operations=[
                    OperationModel(id="A", family="F", max_queue_time=1, eligible_machines=[_option("M1", 2)]),
                    OperationModel(id="B", family="F", eligible_machines=[_option("M2", 2)]),
                ],
            ),
            LotModel(
                id="L2",
                product="P",
                due_time=50,
                operations=[OperationModel(id="X", family="F", eligible_machines=[_option("M2", 10)])],
            ),
        ],
        **fields,
    )


QUEUE_SEED = [
    _assignment("L2", "X", "M2", 0, 10),
    _assignment("L1", "A", "M1", 7, 9),
    _assignment("L1", "B", "M2", 10, 12),
]


def _choice_problem(**fields) -> SemiconductorScheduleModel:
    """Two independent one-step lots; M1 is the preferred tool for both."""
    return SemiconductorScheduleModel(
        machines=[MachineModel(id="M1", group="A"), MachineModel(id="M2", group="A")],
        lots=[
            LotModel(
                id=lot_id,
                product="P",
                due_time=50,
                operations=[
                    OperationModel(id="O", family="F", eligible_machines=[_option("M1", 3), _option("M2", 3, 5)])
                ],
            )
            for lot_id in ("L1", "L2")
        ],
        **fields,
    )


def _batch_problem() -> SemiconductorScheduleModel:
    return SemiconductorScheduleModel(
        machines=[MachineModel(id="F1", group="DIFF", batch_capacity=2)],
        lots=[
            LotModel(
                id=lot_id,
                product="P",
                due_time=50,
                operations=[OperationModel(id="O", family="X", eligible_machines=[_option("F1", 5)])],
            )
            for lot_id in ("L1", "L2")
        ],
    )


def _decoded(seeded: SemiconductorScheduleModel) -> list[AssignmentModel]:
    rows = _operation_rows(seeded)
    return _construct(seeded, *_seed_state(seeded, rows, [0] * len(rows)))


def test_complete_seed_comes_back_unchanged_with_zero_budget():
    plain = _queue_problem()
    solve(plain, config=_no_search())
    assert {(a.operation_id, a.start) for a in plain.assignments} == {("A", 0), ("B", 2), ("X", 4)}

    seeded = with_initial_solution(_queue_problem(), {"assignments": QUEUE_SEED})
    solve(seeded, config=_no_search())
    assert seeded.assignments == [AssignmentModel(**row) for row in QUEUE_SEED]
    assert seeded.score_hard == 0
    assert seeded.makespan == 12


def test_decoder_turns_a_feasible_seed_into_a_worse_schedule():
    # This is why solve scores a complete seed directly: decoding left-shifts A to 0-2, and B then waits 8.
    seeded = with_initial_solution(_queue_problem(), {"assignments": QUEUE_SEED})
    assert sum(constraint_costs(seeded)["maximum_queue_time"]) == 0
    decoded = seeded.model_copy(update={"assignments": _decoded(seeded)})
    assert sum(constraint_costs(decoded)["maximum_queue_time"]) == 7


def test_seed_the_decoder_cannot_keep_is_never_lost_to_the_search():
    for random_seed in range(5):
        seeded = with_initial_solution(_queue_problem(), {"assignments": QUEUE_SEED})
        seed_score = calculate_score(seeded.model_copy(deep=True))
        config = SolverConfig(max_iterations=200, stagnation_limit=20, random_seed=random_seed)
        assert tuple(solve(seeded, config=config)) >= tuple(seed_score)


def test_decoder_merges_runs_that_the_seed_keeps_apart_but_solve_returns_the_seed():
    runs = [_assignment("L1", "O", "F1", 0, 5, batch_id="RUN1"), _assignment("L2", "O", "F1", 5, 10, batch_id="RUN2")]
    seeded = with_initial_solution(_batch_problem(), {"assignments": runs})
    assert {(item.start, item.end) for item in _decoded(seeded)} == {(0, 5)}
    solve(seeded, config=_no_search())
    assert seeded.assignments == [AssignmentModel(**row) for row in runs]


def test_seed_with_idle_time_stays_as_given_until_the_search_runs():
    idle = [_assignment("L1", "O", "M1", 4, 7), _assignment("L2", "O", "M1", 7, 10)]
    seeded = with_initial_solution(_choice_problem(), {"assignments": idle})
    seed_score = calculate_score(seeded.model_copy(deep=True))
    solve(seeded, config=_no_search())
    assert seeded.assignments == [AssignmentModel(**row) for row in idle]

    searched = with_initial_solution(_choice_problem(), {"assignments": idle})
    assert tuple(solve(searched, config=SolverConfig(max_iterations=100, random_seed=1))) > tuple(seed_score)


@pytest.mark.parametrize("as_dict", [False, True])
def test_solved_result_feeds_back(as_dict):
    solved = _queue_problem()
    solve(solved, time_limit_seconds=0.05, random_seed=3)
    solution = solved.model_dump(mode="json") if as_dict else solved
    seeded = with_initial_solution(_queue_problem(), solution)
    assert seeded.warm_start is True
    solve(seeded, config=_no_search())
    assert seeded.assignments == solved.assignments
    assert (seeded.score_hard, seeded.score_soft) == (solved.score_hard, solved.score_soft)


def test_partial_seed_keeps_its_machine_and_order_and_constructs_the_rest():
    seeded = with_initial_solution(_choice_problem(), {"assignments": [_assignment("L2", "O", "M2", 8, 11)]})
    solve(seeded, config=_no_search())
    placed = {item.lot_id: (item.machine_id, item.start) for item in seeded.assignments}
    assert placed == {"L1": ("M1", 0), "L2": ("M2", 0)}
    assert seeded.score_hard == 0


def test_partial_seed_ranks_the_left_out_operations_last():
    seeded = with_initial_solution(_queue_problem(), {"assignments": [_assignment("L2", "X", "M2", 3, 13)]})
    solve(seeded, config=_no_search())
    placed = {item.operation_id: item.start for item in seeded.assignments}
    assert placed == {"X": 0, "A": 0, "B": 10}


def test_seed_against_a_pin_is_scored_as_given_and_the_search_restores_the_pin():
    pin = AssignmentModel(lot_id="L1", operation_id="O", machine_id="M1", start=0, end=3)
    seed = [_assignment("L1", "O", "M1", 5, 8), _assignment("L2", "O", "M1", 0, 3)]
    seeded = with_initial_solution(_choice_problem(pinned_assignments=[pin]), {"assignments": seed})
    solve(seeded, config=_no_search())
    assert constraint_costs(seeded)["pinned_assignments"] == [1]
    searched = with_initial_solution(_choice_problem(pinned_assignments=[pin]), {"assignments": seed})
    solve(searched, config=SolverConfig(max_iterations=100, random_seed=1))
    assert constraint_costs(searched)["pinned_assignments"] == [0]


def test_left_out_pinned_operation_stays_on_its_pin():
    pin = AssignmentModel(lot_id="L1", operation_id="O", machine_id="M2", start=2, end=5)
    seeded = with_initial_solution(
        _choice_problem(pinned_assignments=[pin]), {"assignments": [_assignment("L2", "O", "M1", 0, 3)]}
    )
    solve(seeded, config=_no_search())
    assert pin in seeded.assignments


@pytest.mark.parametrize(
    "entry, message",
    [
        (_assignment("L9", "A", "M1", 0, 2), "unknown operation.*L9"),
        (_assignment("L1", "Z", "M1", 0, 2), "unknown operation.*Z"),
        (_assignment("L1", "A", "M2", 0, 2), "'L1', 'A'.*cannot run on machine 'M2'"),
        (_assignment("L1", "A", "M9", 0, 2), "'L1', 'A'.*cannot run on machine 'M9'"),
        (_assignment("L1", "A", "M1", 0, 3), "'L1', 'A'.*takes 2"),
        (_assignment("L1", "A", "M1", -1, 1), "invalid assignment.*'L1'"),
        (_assignment("L1", "A", "M1", 2, 2), "invalid assignment.*'L1'"),
        ({"lot_id": "L1"}, "invalid assignment.*'L1'"),
        ("L1", "invalid assignment"),
    ],
)
def test_invalid_entries_raise(entry, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(_queue_problem(), {"assignments": [entry]})


def test_duplicate_operation_raises():
    row = _assignment("L1", "A", "M1", 0, 2)
    with pytest.raises(ValueError, match="'L1', 'A'.*more than once"):
        with_initial_solution(_queue_problem(), {"assignments": [row, row]})


@pytest.mark.parametrize("solution", [{}, [], None, {"assignments": {}}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError, match="'assignments' list"):
        with_initial_solution(_queue_problem(), solution)


def test_hard_infeasible_result_feeds_back_unchanged():
    problem = SemiconductorScheduleModel(
        machines=[MachineModel(id="M1", group="A"), MachineModel(id="M2", group="A")],
        lots=[
            LotModel(
                id="L1",
                product="P",
                due_time=50,
                operations=[
                    OperationModel(
                        id="A", family="F", min_queue_time=5, max_queue_time=1, eligible_machines=[_option("M1", 2)]
                    ),
                    OperationModel(id="B", family="F", eligible_machines=[_option("M2", 2)]),
                ],
            )
        ],
    )
    solved = problem.model_copy(deep=True)
    solve(solved, config=SolverConfig(max_iterations=50, random_seed=1))
    assert solved.score_hard < 0
    seeded = with_initial_solution(problem, solved.model_dump(mode="json"))
    solve(seeded, config=_no_search())
    assert seeded.assignments == solved.assignments
    assert (seeded.score_hard, seeded.score_soft) == (solved.score_hard, solved.score_soft)


def test_result_on_a_repeated_machine_option_feeds_back_with_its_own_choice():
    # The scorer reads the last option of a machine and the decoder the chosen one, so this result is hard-infeasible.
    problem = SemiconductorScheduleModel(
        machines=[MachineModel(id="M1", group="A")],
        lots=[
            LotModel(
                id="L1",
                product="P",
                due_time=50,
                operations=[
                    OperationModel(id="O", family="F", eligible_machines=[_option("M1", 3), _option("M1", 5, 1)])
                ],
            )
        ],
    )
    solved = problem.model_copy(deep=True)
    solve(solved, config=_no_search())
    assert solved.score_hard < 0
    assert (solved.assignments[0].start, solved.assignments[0].end) == (0, 3)
    seeded = with_initial_solution(problem, solved)
    assert _seed_state(seeded, _operation_rows(seeded), [1])[0] == [0]
    solve(seeded, config=_no_search())
    assert seeded.assignments == solved.assignments


def test_input_problem_is_not_mutated():
    problem = _queue_problem()
    solve(problem, config=_no_search())
    before = problem.model_dump()
    seeded = with_initial_solution(problem, {"assignments": QUEUE_SEED})
    assert seeded.score_hard is None
    assert seeded.makespan is None
    solve(seeded, config=SolverConfig(max_iterations=50, random_seed=1))
    assert problem.model_dump() == before


def test_plain_solve_still_ignores_input_assignments():
    fresh = _queue_problem()
    preset = _queue_problem(assignments=[AssignmentModel(**row) for row in QUEUE_SEED])
    assert preset.warm_start is False
    solve(fresh, config=SolverConfig(max_iterations=200, random_seed=5))
    solve(preset, config=SolverConfig(max_iterations=200, random_seed=5))
    assert fresh.assignments == preset.assignments
