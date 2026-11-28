"""optimaru.box_sizing.solve — the search and its settings."""

import itertools

import pytest

import optimaru.box_sizing as bs
from optimaru.box_sizing.models import BoxSizingProblemModel


def demo(**arguments) -> BoxSizingProblemModel:
    return bs.generate_demo_data(**arguments)


def optimum(model: BoxSizingProblemModel) -> float:
    """The best soft score over every set of at most max_box_types sizes, each parcel in its
    cheapest box of the set (the default weights)."""
    core = model.to_core()
    boxes, parcels = core.boxes, core.parcels

    def fits(parcel, box):
        sides = zip(sorted((parcel.length, parcel.width, parcel.height)), sorted((box.length, box.width, box.height)))
        return all(p <= b for p, b in sides) and (box.max_weight is None or parcel.weight <= box.max_weight)

    price = [
        [
            parcel.quantity
            * (box.length * box.width * box.height - parcel.length * parcel.width * parcel.height + box.cost)
            if fits(parcel, box)
            else None
            for box in boxes
        ]
        for parcel in parcels
    ]
    best = float("-inf")
    for size in range(1, model.max_box_types + 1):
        for chosen in itertools.combinations(range(len(boxes)), size):
            total = 0.0
            for row in price:
                options = [row[b] for b in chosen if row[b] is not None]
                if not options:
                    break
                total += min(options)
            else:
                best = max(best, -total)
    return best


def small(seed: int, limit: int, cost: float = 0.0) -> BoxSizingProblemModel:
    """12 of the demo's candidates and 40 parcels: small enough to enumerate every set."""
    model = demo(seed=seed, parcel_count=40, max_box_types=limit)
    data = model.model_dump()
    data["boxes"] = data["boxes"][::2]
    data["boxes"][-1] = model.boxes[-1].model_dump()  # the one that takes every parcel
    for index, box in enumerate(data["boxes"]):
        box["cost"] = cost * (index % 5)
    return BoxSizingProblemModel.model_validate(data)


def test_solve_assigns_every_parcel_feasibly():
    solved = bs.solve(demo(), time_limit_seconds=0.5, random_seed=1)
    assert solved.score_hard == 0
    assert all(parcel.box is not None for parcel in solved.parcels)
    assert solved.box_types_used <= 6
    assert sum(box.parcels for box in solved.boxes) == sum(parcel.quantity for parcel in solved.parcels)
    assert 0 < solved.fill_ratio <= 1


def test_the_score_on_the_result_is_a_full_recompute():
    core = demo().to_core()
    bs.solve(core, time_limit_seconds=0.3, random_seed=2)
    reported = (core.score.hard, core.score.soft)
    fresh = bs.calculate_score(core)
    assert (fresh.hard, fresh.soft) == reported
    assert core.score.soft == -core.void_volume()


def test_seeded_runs_with_a_step_limit_are_reproducible():
    def run():
        config = bs.SolverConfig(stop={"time_limit_seconds": 60, "step_limit": 3000}, run={"random_seed": 7})
        return bs.solve(demo(), config=config)

    assert run() == run()


@pytest.mark.parametrize(("seed", "limit", "cost"), [(1, 2, 0.0), (2, 3, 0.0), (3, 4, 250000.0), (4, 5, 0.0)])
def test_small_instances_are_solved_to_the_optimum(seed, limit, cost):
    problem = small(seed, limit, cost)
    config = bs.SolverConfig(stop={"time_limit_seconds": 60, "step_limit": 20000}, run={"random_seed": 1})
    solved = bs.solve(problem, config=config)
    assert solved.score_hard == 0
    assert solved.score_soft == pytest.approx(optimum(problem), abs=1e-6)


def test_a_zero_budget_returns_the_greedy_construction_and_the_search_is_never_worse():
    for seed in range(4):
        problem = demo(seed=seed)
        greedy = bs.solve(problem, time_limit_seconds=0)
        assert greedy.score_hard == 0 and greedy.box_types_used <= 6
        config = bs.SolverConfig(stop={"time_limit_seconds": 60, "step_limit": 5000})
        searched = bs.solve(problem, config=config)
        assert (searched.score_hard, searched.score_soft) >= (greedy.score_hard, greedy.score_soft)


def test_the_search_improves_on_the_greedy_construction_somewhere():
    """On some demo instance the greedy's set of sizes is not the best one, and the moves find a better."""
    config = bs.SolverConfig(stop={"time_limit_seconds": 60, "step_limit": 5000})
    gains = []
    for seed in range(8):
        problem = demo(seed=seed, max_box_types=4)
        gains.append(bs.solve(problem, config=config).score_soft - bs.solve(problem, time_limit_seconds=0).score_soft)
    assert min(gains) >= 0 and max(gains) > 0


def test_without_the_moves_on_the_sizes_the_search_keeps_the_greedy_sizes():
    problem = demo(seed=3, max_box_types=4)
    greedy = bs.solve(problem, time_limit_seconds=0)
    config = bs.SolverConfig(
        stop={"time_limit_seconds": 60, "step_limit": 5000},
        custom_moves={"close_box": False, "open_box": False, "swap_box": False},
    )
    plain = bs.solve(problem, config=config)
    assert {parcel.box for parcel in plain.parcels} == {parcel.box for parcel in greedy.parcels}
    assert plain.score_soft == greedy.score_soft


def test_no_limit_puts_every_parcel_in_its_tightest_box():
    problem = demo(max_box_types=None)
    solved = bs.solve(problem, time_limit_seconds=0)
    core = problem.to_core()
    volume = {box.id: box.length * box.width * box.height for box in problem.boxes}

    def tightest(parcel):
        sides = sorted((parcel.length, parcel.width, parcel.height))
        fitting = [
            box
            for box in core.boxes
            if all(p <= b for p, b in zip(sides, sorted((box.length, box.width, box.height))))
            and (box.max_weight is None or parcel.weight <= box.max_weight)
        ]
        return min(volume[box.id] for box in fitting)

    assert all(volume[parcel.box] == tightest(parcel) for parcel in solved.parcels)
    assert solved.score_hard == 0


def test_a_required_box_counts_and_stays():
    data = demo(max_box_types=3).model_dump()
    data["boxes"][7]["required"] = True  # BOX-08
    solved = bs.solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=0.3, random_seed=1)
    assert solved.score_hard == 0 and solved.box_types_used <= 3
    in_use = {parcel.box for parcel in solved.parcels}
    assert len(in_use | {"BOX-08"}) == solved.box_types_used


def test_a_weight_limit_keeps_heavy_parcels_out_of_a_box(nested):
    problem = nested()
    data = problem.model_dump()
    data["boxes"][0]["max_weight"] = 100  # S carries 100
    data["parcels"][0]["weight"] = 150  # the small parcel is too heavy for S
    solved = bs.solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=0.1)
    assert solved.parcels[0].box == "M" and solved.score_hard == 0


def test_a_box_cost_moves_parcels_to_the_cheaper_box(nested):
    data = nested().model_dump()
    data["boxes"][1]["cost"] = 30000.0  # M: 8,000 of volume + 30,000 is dearer than L's 27,000
    solved = bs.solve(BoxSizingProblemModel.model_validate(data), time_limit_seconds=0.1)
    assert [parcel.box for parcel in solved.parcels] == ["S", "L", "L"]
    assert solved.score_soft == -(5 * (27000 - 6000))


def test_stop_at_first_feasible_and_on_new_best():
    seen = []
    solved = bs.solve(
        demo(),
        time_limit_seconds=20,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: seen.append((step, score.hard, score.soft)),
    )
    assert solved.score_hard == 0
    assert seen[0][0] == 0 and seen[-1][1:] == (solved.score_hard, solved.score_soft)


def test_parallel_starts_return_a_scored_solution():
    solved = bs.solve(demo(), time_limit_seconds=0.3, random_seed=3, parallel_solver_count=3)
    core = solved.to_core()
    assert bs.calculate_score(core).soft == solved.score_soft and solved.score_hard == 0


def test_full_assert_passes_on_both_paths():
    from optimaru.box_sizing.constraints import BoxTypeCost, get_constraints

    data = demo(parcel_count=40).model_dump()
    for index, box in enumerate(data["boxes"]):
        box["cost"] = 1000.0 * (index % 7)
    problem = BoxSizingProblemModel.model_validate(data)
    config = bs.SolverConfig(stop={"time_limit_seconds": 60, "step_limit": 3000})
    assert bs.solve(problem, config=config, full_assert=True).score_hard == 0
    picks = [BoxTypeCost(weight=500000) if spec.name == "box_type_cost" else spec for spec in get_constraints()]
    assert bs.solve(problem, config=config, full_assert=True, constraints=picks).score_hard == 0


def test_the_settings_of_this_use_case():
    config = bs.SolverConfig()
    assert config.custom_moves.to_dict() == {"share": 1.0, "close_box": True, "open_box": True, "swap_box": True}
    assert config.acceptance.to_dict() == {"late_acceptance_size": 0, "reheat": True}
    assert config.alns_trigger.enabled is False
    with pytest.raises(AttributeError, match="is now SolverConfig.custom_moves.swap_box"):
        config.swap_box_moves = False


def test_reheating_reaches_what_a_plain_descent_does_not():
    """Late acceptance from the greedy construction is a descent; with the reheats the search
    leaves the first set of sizes no single move improves. Over the seeds it is never worse."""

    def run(problem, reheat):
        config = bs.SolverConfig(
            stop={"time_limit_seconds": 60, "step_limit": 20000}, acceptance={"reheat": reheat}, run={"random_seed": 1}
        )
        return bs.solve(problem, config=config).score_soft

    with_reheat, without = [], []
    for seed in range(12):
        problem = small(seed, 2 + seed % 4)
        with_reheat.append(run(problem, True))
        without.append(run(problem, False))
    assert all(a >= b for a, b in zip(with_reheat, without))
