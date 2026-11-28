from box_sizing_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert benchmark.instances()
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)


def test_the_ladder_has_twenty_instances_and_the_small_ones_build() -> None:
    ladder = benchmark.instances()
    assert len(ladder) == 20
    for name in ("small", "small-2", "demo", "medium"):
        problem = ladder[name]()
        # Every rung has a size to show beside its name.
        assert benchmark.describe(problem)
    assert benchmark.describe(ladder["demo"]()) == "120 parcels / 24 boxes / 6 types"


def test_an_instance_is_the_same_every_time() -> None:
    first, again = benchmark.instances()["small-2"](), benchmark.instances()["small-2"]()
    assert [(box.length, box.width, box.height) for box in first.boxes] == [
        (box.length, box.width, box.height) for box in again.boxes
    ]
    # The largest candidate takes every parcel, so the instance is feasible.
    assert (first.boxes[-1].length, first.boxes[-1].width, first.boxes[-1].height) == benchmark.LARGEST


def test_both_solvers_solve_the_smallest_instance_to_the_same_score() -> None:
    ladder = benchmark.instances()
    engine = ladder["small"]()
    benchmark.solve_engine(engine, 1.0, 1)
    cpsat = benchmark.solve_cpsat(ladder["small"](), 5.0, 1, 2)
    import optimaru.box_sizing as bs

    rescored = bs.calculate_score(cpsat)  # the runner rescores it the same way
    assert (engine.score.hard, rescored.hard) == (0, 0)
    assert engine.score.soft == rescored.soft
