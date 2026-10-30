"""Starting a solve from an earlier solution."""

import json
import subprocess
import sys

import pytest

import task_assigning as ta
from task_assigning.constraints import get_constraints
from task_assigning.models import (
    CustomerModel,
    EmployeeModel,
    TaskAssigningSolutionModel,
    TaskModel,
    TaskTypeModel,
    with_initial_solution,
)

PATHS = [pytest.param(lambda: None, id="default"), pytest.param(get_constraints, id="constraints")]
TASKS = ["t1", "t2", "t3", "t4", "t5", "t6"]
# Every task to the slower employee, in reverse: nothing construction would pick.
NOT_GREEDY = {"slow": ["t6", "t5", "t4", "t3", "t2", "t1"], "fast": []}


def problem() -> TaskAssigningSolutionModel:
    return TaskAssigningSolutionModel(
        task_types=[TaskTypeModel(code="T", base_duration=10)],
        customers=[CustomerModel(id="c")],
        tasks=[TaskModel(id=task_id, task_type_code="T", customer_id="c") for task_id in TASKS],
        employees=[
            EmployeeModel(id="fast", customer_to_affinity={"c": "HIGH"}),
            EmployeeModel(id="slow", customer_to_affinity={"c": "LOW"}),
        ],
    )


def seed_of(tasks: dict[str, list[str]]) -> TaskAssigningSolutionModel:
    solution = problem()
    for employee in solution.employees:
        employee.tasks = tasks.get(employee.id, [])
    return solution


def solve(model, constraints=None, seconds: float = 0) -> TaskAssigningSolutionModel:
    """A zero time limit runs construction and no search step."""
    return ta.solve(model, time_limit_seconds=seconds, constraints=constraints)


def assignment(solution: TaskAssigningSolutionModel) -> dict[str, list[str]]:
    return {employee.id: employee.tasks for employee in solution.employees}


def score(solution: TaskAssigningSolutionModel) -> tuple[int, ...]:
    return solution.score_hard, solution.score_soft0, solution.score_soft1, solution.score_soft2


@pytest.mark.parametrize("constraints", PATHS)
def test_a_complete_seed_comes_back_unchanged(constraints):
    assert assignment(solve(problem(), constraints())) != NOT_GREEDY  # so it cannot have been constructed

    seeded = with_initial_solution(problem(), seed_of(NOT_GREEDY))

    assert assignment(solve(seeded, constraints())) == NOT_GREEDY


@pytest.mark.parametrize("as_seed", [lambda s: s, lambda s: json.loads(json.dumps(s.model_dump(mode="json")))])
def test_a_solved_result_feeds_back_as_model_and_as_json(as_seed):
    solved = solve(problem(), seconds=0.1)

    seeded = with_initial_solution(problem(), as_seed(solved))

    assert assignment(solve(seeded)) == assignment(solved)


@pytest.mark.parametrize("constraints", PATHS)
def test_a_partial_seed_keeps_its_part_and_fills_the_rest(constraints):
    seeded = with_initial_solution(problem(), seed_of({"slow": ["t6", "t2"]}))

    solution = solve(seeded, constraints())

    tasks = assignment(solution)
    assert sorted(tasks["fast"] + tasks["slow"]) == TASKS
    assert [task for task in tasks["slow"] if task in ("t6", "t2")] == ["t6", "t2"]
    assert solution.score_soft0 == 0  # nothing left unassigned


def test_the_seed_replaces_what_the_problem_had():
    request = with_initial_solution(problem(), seed_of({"fast": ["t1"], "slow": ["t2"]}))

    seeded = with_initial_solution(request, seed_of({"fast": ["t3"]}))

    assert assignment(seeded) == {"fast": ["t3"], "slow": []}


def test_an_employee_the_solution_leaves_out_starts_empty():
    request = with_initial_solution(problem(), seed_of({"fast": ["t1"], "slow": ["t2"]}))
    solution = seed_of({"fast": ["t3"]}).model_dump(mode="json")
    solution["employees"] = [employee for employee in solution["employees"] if employee["id"] == "fast"]

    assert assignment(with_initial_solution(request, solution)) == {"fast": ["t3"], "slow": []}


def test_an_unknown_task_is_refused():
    solution = seed_of({"fast": ["nope"]}).model_dump()
    solution["tasks"].append(solution["tasks"][0] | {"id": "nope"})

    with pytest.raises(ValueError, match="'fast' has unknown task 'nope'"):
        with_initial_solution(problem(), solution)


def test_an_unknown_employee_is_refused():
    solution = seed_of({}).model_dump()
    solution["employees"].append(EmployeeModel(id="nope").model_dump())

    with pytest.raises(ValueError, match="unknown employee .*nope"):
        with_initial_solution(problem(), solution)


@pytest.mark.parametrize("tasks", [{"fast": ["t1", "t1"]}, {"fast": ["t1"], "slow": ["t1"]}])
def test_a_task_assigned_twice_is_refused(tasks):
    with pytest.raises(ValueError, match="'t1' is assigned to more than one employee"):
        with_initial_solution(problem(), seed_of(tasks))


def test_a_task_assigned_twice_is_refused_as_a_dict_too():
    solution = seed_of({}).model_dump()
    solution["employees"][0]["tasks"] = ["t1", "t1"]

    with pytest.raises(ValueError, match="'t1' is assigned to more than one employee"):
        with_initial_solution(problem(), solution)


def test_a_dict_that_is_no_solution_is_refused():
    with pytest.raises(ValueError):
        with_initial_solution(problem(), {"employees": "nope"})


def test_the_input_is_not_changed():
    request = problem()
    solution = seed_of(NOT_GREEDY)
    before = request.model_dump(), solution.model_dump()

    seeded = with_initial_solution(request, solution)
    seeded.employees[0].tasks.append("t1")
    seeded.tasks[0].priority = "MAJOR"

    assert (request.model_dump(), solution.model_dump()) == before


@pytest.mark.parametrize("constraints", PATHS)
def test_a_short_search_is_not_worse_than_its_seed(constraints):
    start = solve(with_initial_solution(problem(), seed_of(NOT_GREEDY)), constraints())

    improved = solve(with_initial_solution(problem(), seed_of(NOT_GREEDY)), constraints(), seconds=0.2)

    assert score(improved) >= score(start)


def test_the_package_exports_it():
    assert "with_initial_solution" in ta.__all__

    seeded = ta.with_initial_solution(problem(), seed_of(NOT_GREEDY))

    assert assignment(solve(seeded)) == NOT_GREEDY


def test_the_package_still_imports_without_pydantic():
    code = "import sys; sys.modules['pydantic'] = None; import task_assigning; task_assigning.Solver"

    subprocess.run([sys.executable, "-c", code], check=True)
