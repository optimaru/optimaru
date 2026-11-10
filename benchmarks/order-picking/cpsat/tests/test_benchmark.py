import sys

from order_picking_cpsat import benchmark


def test_external_solver_uses_the_engine_in_this_checkout():
    import external_solver.order_picking
    from optimaru import order_picking

    assert sys.modules["optimaru.order_picking"] is order_picking
    assert callable(external_solver.order_picking.catalog)


def test_every_instance_builds():
    assert benchmark.instances()
    for build in benchmark.instances().values():
        assert build() is not None
