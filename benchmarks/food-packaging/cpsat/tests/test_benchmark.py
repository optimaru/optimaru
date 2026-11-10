import sys

from food_packaging_cpsat import benchmark


def test_external_solver_uses_the_engine_in_this_checkout():
    import external_solver.food_packaging

    from optimaru import food_packaging

    assert sys.modules["optimaru.food_packaging"] is food_packaging
    assert callable(external_solver.food_packaging.catalog)


def test_every_instance_builds():
    assert benchmark.instances()
    for build in benchmark.instances().values():
        assert build() is not None
