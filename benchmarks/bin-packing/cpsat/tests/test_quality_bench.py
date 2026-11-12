from bin_packing_cpsat import quality_bench
from optimaru import bin_packing
from optimaru.bin_packing.models import BinPackingProblemModel


def test_the_review_instance_is_reproduced() -> None:
    data = quality_bench.instances()["sq1"]()
    assert len(data["items"]) == 120
    assert len(data["conflicts"]) == 10
    assert sum(item["weight"] for item in data["items"]) == 4288
    assert quality_bench.lower_bound(data) == 43
    assert sum("color" in item for item in data["items"]) == 51


def test_plain_twins_keep_the_weights_and_drop_the_rest() -> None:
    built = quality_bench.instances()
    for name in quality_bench.REVIEW_SEEDS:
        coloured, plain = built[name](), built[f"plain-{name}"]()
        assert [item["weight"] for item in plain["items"]] == [item["weight"] for item in coloured["items"]]
        assert not plain["conflicts"]
        assert not any("color" in item for item in plain["items"])


def test_every_extra_instance_validates() -> None:
    for name, build in quality_bench.instances(extra=True).items():
        model = BinPackingProblemModel.model_validate(build())
        assert model.items, name
    assert len(quality_bench.instances(extra=True)["web-demo-x6"]()["items"]) == 120


def test_a_short_step_limited_solve_is_no_worse_than_the_baseline() -> None:
    config = bin_packing.SolverConfig()
    config.step_count_limit = 50_000
    config.random_seed = 1
    for name, reference in quality_bench.BASELINE_BINS.items():
        core = BinPackingProblemModel.model_validate(quality_bench.instances()[name]()).to_core()
        score = bin_packing.solve(core, config=config).score
        assert score.hard == 0, name
        assert core.bins_used() <= reference, name
