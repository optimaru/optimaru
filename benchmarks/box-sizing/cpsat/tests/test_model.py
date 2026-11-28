"""The CP-SAT model against the engine's own evaluator and an enumeration of every set of sizes."""

import itertools

import pytest
from ortools.sat.python import cp_model

import optimaru.box_sizing as bs
from box_sizing_cpsat.model import build, cost_scale, groups, solve
from optimaru.box_sizing.models import BoxSizingProblemModel


def nested(**fields) -> BoxSizingProblemModel:
    """Three sizes that nest (volumes 1,000 / 8,000 / 27,000) and a parcel for each."""
    data = {
        "boxes": [
            {"id": "S", "length": 10, "width": 10, "height": 10},
            {"id": "M", "length": 20, "width": 20, "height": 20},
            {"id": "L", "length": 30, "width": 30, "height": 30},
        ],
        "parcels": [
            {"id": "small", "length": 10, "width": 10, "height": 10, "quantity": 10},
            {"id": "medium", "length": 20, "width": 20, "height": 15, "quantity": 5},
            {"id": "large", "length": 30, "width": 30, "height": 30},
        ],
    } | fields
    return BoxSizingProblemModel.model_validate(data)


def small(seed: int, limit: int, cost: float = 0.0) -> BoxSizingProblemModel:
    """Ten candidates and thirty parcels: every set of sizes can be enumerated."""
    data = bs.generate_demo_data(seed=seed, parcel_count=30, max_box_types=limit).model_dump()
    largest = data["boxes"][-1]
    data["boxes"] = [*data["boxes"][1:19:2], largest]
    for index, box in enumerate(data["boxes"]):
        box["cost"] = cost * (index % 4)
    return BoxSizingProblemModel.model_validate(data)


def enumerated_optimum(problem: BoxSizingProblemModel) -> float:
    core = problem.to_core()
    best = float("-inf")
    for size in range(1, problem.max_box_types + 1):
        for chosen in itertools.combinations(range(len(problem.boxes)), size):
            assignment = []
            for parcel in problem.parcels:
                fitting = [
                    b
                    for b in chosen
                    if all(
                        p <= s
                        for p, s in zip(
                            sorted((parcel.length, parcel.width, parcel.height)),
                            sorted((problem.boxes[b].length, problem.boxes[b].width, problem.boxes[b].height)),
                        )
                    )
                    and (problem.boxes[b].max_weight is None or parcel.weight <= problem.boxes[b].max_weight)
                ]
                if not fitting:
                    break
                box = problem.boxes
                assignment.append(
                    min(fitting, key=lambda b: box[b].length * box[b].width * box[b].height + box[b].cost)
                )
            else:
                core.set_assignment(assignment)
                score = bs.calculate_score(core)
                if score.hard == 0:
                    best = max(best, score.soft)
    return best


def test_the_nested_problem_by_hand() -> None:
    solved = solve(nested(max_box_types=2), time_limit_seconds=5, parallel_solver_count=2)
    assert {parcel.id: parcel.box for parcel in solved.parcels} == {"small": "M", "medium": "M", "large": "L"}
    assert (solved.score_hard, solved.score_soft, solved.box_types_used) == (0, -80000.0, 2)


def test_the_objective_is_the_engines_soft_score() -> None:
    """The objective value of the model's solution, unscaled, is minus the soft score the
    engine gives the same assignment: one cost definition on both sides."""
    problem = small(seed=3, limit=3, cost=2.5)
    model, ships, scale = build(problem)
    assert scale == 10
    solver = cp_model.CpSolver()
    solver.parameters.num_workers = 2
    assert solver.solve(model) == cp_model.OPTIMAL
    core = problem.to_core()
    assignment = [-1] * len(problem.parcels)
    for members, b, variable in ships:
        if solver.boolean_value(variable):
            for p in members:
                assignment[p] = b
    core.set_assignment(assignment)
    score = bs.calculate_score(core)
    assert score.hard == 0
    assert solver.objective_value / scale == pytest.approx(-score.soft)


@pytest.mark.parametrize(("seed", "limit", "cost"), [(1, 2, 0.0), (2, 3, 0.0), (3, 4, 40000.0), (4, 3, 2.5)])
def test_the_model_and_the_engine_reach_the_enumerated_optimum(seed, limit, cost) -> None:
    problem = small(seed, limit, cost)
    optimum = enumerated_optimum(problem)
    cpsat = solve(problem, time_limit_seconds=10, parallel_solver_count=2)
    config = bs.SolverConfig(stop={"time_limit_seconds": 60, "step_limit": 20000}, run={"random_seed": 1})
    engine = bs.solve(problem, config=config)
    assert cpsat.score_hard == 0 and cpsat.score_soft == pytest.approx(optimum)
    assert engine.score_hard == 0 and engine.score_soft == pytest.approx(optimum)


def test_required_boxes_and_weight_limits_are_in_the_model() -> None:
    data = nested(max_box_types=2).model_dump()
    data["boxes"][0]["required"] = True  # S stays: S and L, the medium parcel goes to L
    solved = solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=5, parallel_solver_count=2)
    assert {parcel.id: parcel.box for parcel in solved.parcels} == {"small": "S", "medium": "L", "large": "L"}
    data = nested().model_dump()
    data["boxes"][0]["max_weight"] = 100
    data["parcels"][0]["weight"] = 150  # too heavy for S
    solved = solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=5, parallel_solver_count=2)
    assert solved.parcels[0].box == "M" and solved.score_hard == 0


def test_cost_scale() -> None:
    assert cost_scale(nested()) == 1
    data = nested().model_dump()
    data["boxes"][0]["cost"] = 0.125
    assert cost_scale(BoxSizingProblemModel.model_validate(data)) == 1000
    data["boxes"][0]["cost"] = 0.0001
    with pytest.raises(ValueError, match="multiples of 0.001"):
        cost_scale(BoxSizingProblemModel.model_validate(data))


def test_parcels_that_fit_the_same_boxes_form_one_group() -> None:
    data = nested().model_dump()
    data["parcels"].append({"id": "small-2", "length": 9, "width": 9, "height": 9, "quantity": 3})
    problem = BoxSizingProblemModel.model_validate(data)
    assert groups(problem) == {(0, 1, 2): [0, 3], (1, 2): [1], (2,): [2]}
    solved = solve(problem.model_copy(update={"max_box_types": 2}), time_limit_seconds=5, parallel_solver_count=2)
    assert {parcel.id: parcel.box for parcel in solved.parcels} == {
        "small": "M",
        "small-2": "M",
        "medium": "M",
        "large": "L",
    }
