from bin_packing_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert benchmark.instances()
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)
    for build in benchmark.instances().values():
        assert build().item_count() > 0
