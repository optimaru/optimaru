"""Starting a solve from an earlier solution."""

import json
import subprocess
import sys
from datetime import date, datetime, timedelta

import pytest

import food_packaging as fp
from food_packaging.constraints import get_constraints
from food_packaging.models import (
    JobModel,
    LineModel,
    OperatorModel,
    PackagingScheduleModel,
    ProductModel,
    with_initial_solution,
)

PATHS = [pytest.param(lambda: None, id="default"), pytest.param(get_constraints, id="constraints")]
START = datetime(2026, 1, 5, 6)
JOBS = ["a1", "a2", "a3", "b1", "b2", "b3"]
# Operators swapped against the order `assign_missing_operators` would use.
NOT_GREEDY = {"L1": ("op2", ["b3", "a1", "b2"]), "L2": ("op1", ["a3", "b1", "a2"])}


def no_search() -> fp.SolverConfig:
    """Stops right after construction: any score clears this floor.

    A zero time limit would not do: the default solve then returns before
    construction, which is what `test_a_zero_time_limit_skips_construction` shows.
    """
    config = fp.SolverConfig()
    config.best_score_limit = (-(10**15), -1e18)
    return config


def job(name: str, *, pinned: bool = False) -> JobModel:
    return JobModel(
        id=name,
        name=name,
        product_id=name[0],
        duration=timedelta(hours=1),
        min_start_time=START,
        ideal_end_time=START + timedelta(days=2),
        max_end_time=START + timedelta(days=4),
        pinned=pinned,
    )


def product(name: str, other: str) -> ProductModel:
    return ProductModel(
        id=name, name=name, cleaning_durations={name: timedelta(0), other: timedelta(minutes=30)}
    )


def problem(*, pinned=(), lines=None) -> PackagingScheduleModel:
    lines = lines or {}
    return PackagingScheduleModel(
        from_date=date(2026, 1, 5),
        to_date=date(2026, 1, 12),
        products=[product("a", "b"), product("b", "a")],
        operators=[OperatorModel(id="op1"), OperatorModel(id="op2")],
        lines=[
            LineModel(id=name, name=name, start_date_time=START, job_ids=lines.get(name, [])) for name in ("L1", "L2")
        ],
        jobs=[job(name, pinned=name in pinned) for name in JOBS],
    )


def seed_of(lines: dict[str, tuple[str | None, list[str]]]) -> PackagingScheduleModel:
    solution = problem()
    for line in solution.lines:
        line.operator_id, line.job_ids = lines.get(line.id, (None, []))
    return solution


def solve(model, *, constraints=None, config=None, seconds=None) -> PackagingScheduleModel:
    core = model.to_core()
    fp.solve(core, config=config, constraints=constraints, time_limit_seconds=seconds)
    return PackagingScheduleModel.from_core(core)


def lines_of(solution: PackagingScheduleModel) -> dict[str, tuple[str | None, list[str]]]:
    return {line.id: (line.operator_id, line.job_ids) for line in solution.lines}


def score(solution: PackagingScheduleModel) -> tuple[int, int, int]:
    return solution.score_hard, solution.score_medium, solution.score_soft


@pytest.mark.parametrize("constraints", PATHS)
def test_a_complete_seed_comes_back_unchanged(constraints):
    constructed = lines_of(solve(problem(), constraints=constraints(), config=no_search()))
    assert constructed != NOT_GREEDY  # so what comes back cannot have been constructed

    seeded = with_initial_solution(problem(), seed_of(NOT_GREEDY))

    assert lines_of(solve(seeded, constraints=constraints(), config=no_search())) == NOT_GREEDY


@pytest.mark.parametrize("as_seed", [lambda s: s, lambda s: json.loads(s.model_dump_json())])
def test_a_solved_result_feeds_back_as_model_and_as_json(as_seed):
    solved = solve(problem(), seconds=0.1)

    seeded = with_initial_solution(problem(), as_seed(solved))

    assert lines_of(solve(seeded, config=no_search())) == lines_of(solved)


@pytest.mark.parametrize("constraints", PATHS)
def test_a_partial_seed_keeps_its_part_and_fills_the_rest(constraints):
    seeded = with_initial_solution(problem(), seed_of({"L1": ("op2", ["b3", "a1"])}))

    solution = lines_of(solve(seeded, constraints=constraints(), config=no_search()))

    assert sorted(solution["L1"][1] + solution["L2"][1]) == JOBS
    assert [name for name in solution["L1"][1] if name in ("b3", "a1")] == ["b3", "a1"]


def test_a_zero_time_limit_skips_construction():
    """The default solve checks its deadline first, so nothing places the left-out jobs."""
    seeded = with_initial_solution(problem(), seed_of({"L1": ("op1", ["b3", "a1"]), "L2": ("op2", [])}))

    assert lines_of(solve(seeded, seconds=0))["L1"][1] == ["b3", "a1"]
    assert lines_of(solve(seeded, seconds=0))["L2"][1] == []
    # The constraints path has no such check: construction runs before its first deadline test.
    filled = lines_of(solve(seeded, constraints=get_constraints(), seconds=0))
    assert sorted(filled["L1"][1] + filled["L2"][1]) == JOBS


def test_the_seed_replaces_what_the_problem_had():
    request = with_initial_solution(problem(), seed_of({"L1": ("op1", ["a1"]), "L2": ("op2", ["a2"])}))

    seeded = with_initial_solution(request, seed_of({"L1": ("op2", ["a3"])}))

    assert lines_of(seeded) == {"L1": ("op2", ["a3"]), "L2": (None, [])}


SHARED = {"L1": ("op1", ["a1", "b1", "a3"]), "L2": ("op1", ["a2", "b2", "b3"])}  # both clean at once, one operator


def test_the_default_solve_reassigns_a_conflicting_operator_even_without_search():
    seeded = with_initial_solution(problem(), seed_of(SHARED))

    solution = lines_of(solve(seeded, config=no_search()))

    assert {line: jobs for line, (_, jobs) in solution.items()} == {line: jobs for line, (_, jobs) in SHARED.items()}
    assert {operator for operator, _ in solution.values()} == {"op1", "op2"}


def test_the_default_solve_also_reassigns_it_with_no_time():
    seeded = with_initial_solution(problem(), seed_of(SHARED))

    assert {operator for operator, _ in lines_of(solve(seeded, seconds=0)).values()} == {"op1", "op2"}


def test_the_constraints_solve_keeps_a_conflicting_operator():
    seeded = with_initial_solution(problem(), seed_of(SHARED))

    solution = solve(seeded, constraints=get_constraints(), config=no_search())

    assert lines_of(solution) == SHARED
    assert solution.score_hard < 0  # the conflict is scored, not repaired


def pinned_problem() -> PackagingScheduleModel:
    return problem(pinned=("a1", "b3"), lines={"L1": ["a1"]})  # b3 is pinned out of every line


def test_pinned_jobs_stay_where_the_problem_has_them():
    seeded = with_initial_solution(pinned_problem(), seed_of({"L1": ("op1", ["a1", "b1"]), "L2": ("op2", ["a2"])}))

    solution = lines_of(solve(seeded, config=no_search()))

    assert solution["L1"][1][0] == "a1"
    assert "b3" not in solution["L1"][1] + solution["L2"][1]


@pytest.mark.parametrize(
    "seed, message",
    [
        ({"L1": ("op1", ["b1", "a1"])}, r"L1: the jobs do not start with its pinned jobs \['a1'\]"),
        ({"L2": ("op1", ["a1"])}, r"L1: the jobs do not start with its pinned jobs \['a1'\]"),
        ({}, r"L1: the jobs do not start with its pinned jobs \['a1'\]"),
        ({"L1": ("op1", ["a1"]), "L2": ("op2", ["b3"])}, r"L2: pinned job \['b3'\] cannot move here"),
    ],
)
def test_a_seed_cannot_move_a_pinned_job(seed, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(pinned_problem(), seed_of(seed))


@pytest.mark.parametrize(
    "seed, message",
    [
        ({"L1": ("op1", ["a1", "nope"])}, "unknown job 'nope'"),
        ({"L1": ("nope", ["a1"])}, "unknown operator 'nope'"),
        ({"L1": ("op1", ["a1", "a1"])}, "'a1' appears twice"),
        ({"L1": ("op1", ["a1"]), "L2": ("op2", ["a2", "a1"])}, "'a1' appears twice"),
    ],
)
def test_a_bad_seed_is_refused(seed, message):
    with pytest.raises(ValueError, match=message):
        with_initial_solution(problem(), seed_of(seed))


def test_an_unknown_line_is_refused():
    solution = seed_of({}).model_dump()
    solution["lines"].append(solution["lines"][0] | {"id": "nope"})

    with pytest.raises(ValueError, match="unknown line .*nope"):
        with_initial_solution(problem(), solution)


def test_a_line_given_twice_is_refused():
    solution = seed_of({}).model_dump()
    solution["lines"].append(solution["lines"][0])

    with pytest.raises(ValueError, match="line L1 appears twice"):
        with_initial_solution(problem(), solution)


def test_an_adjacency_with_no_cleaning_time_is_refused():
    request = problem()
    request.products[1].cleaning_durations.pop("a")  # nothing says how to go from a to b

    with pytest.raises(ValueError, match="no cleaning time from 'a' to 'b' between 'a1' and 'b1'"):
        with_initial_solution(request, seed_of({"L1": ("op1", ["a1", "b1"])}))


def test_a_dict_that_is_no_solution_is_refused():
    with pytest.raises(ValueError):
        with_initial_solution(problem(), {"lines": "nope"})


def test_the_input_is_not_changed():
    request = problem()
    solution = seed_of(NOT_GREEDY)
    before = request.model_dump(), solution.model_dump()

    seeded = with_initial_solution(request, solution)
    seeded.lines[0].job_ids.append("a1")
    seeded.jobs[0].priority = 9

    assert (request.model_dump(), solution.model_dump()) == before


@pytest.mark.parametrize("constraints", PATHS)
def test_a_short_search_is_not_worse_than_its_seed(constraints):
    seeded = with_initial_solution(problem(), seed_of(NOT_GREEDY))
    start = solve(seeded, constraints=constraints(), config=no_search())

    improved = solve(seeded, constraints=constraints(), seconds=0.2)

    assert score(improved) >= score(start)


def test_the_package_exports_it():
    assert "with_initial_solution" in fp.__all__

    seeded = fp.with_initial_solution(problem(), seed_of(NOT_GREEDY))

    assert lines_of(seeded) == NOT_GREEDY


def test_the_package_still_imports_without_pydantic():
    code = "import sys; sys.modules['pydantic'] = None; import food_packaging; food_packaging.Solver"

    subprocess.run([sys.executable, "-c", code], check=True)
