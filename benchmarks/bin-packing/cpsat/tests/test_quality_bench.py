import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import bin_packing
from bench import quality
from bin_packing.models import BinPackingProblemModel


def test_the_review_instance_is_reproduced() -> None:
    data = quality.instances()["sq1"]()
    assert len(data["items"]) == 120
    assert len(data["conflicts"]) == 10
    assert sum(item["weight"] for item in data["items"]) == 4288
    assert quality.lower_bound(data) == 43
    assert sum("color" in item for item in data["items"]) == 51


def test_plain_twins_keep_the_weights_and_drop_the_rest() -> None:
    built = quality.instances()
    for name in quality.REVIEW_SEEDS:
        coloured, plain = built[name](), built[f"plain-{name}"]()
        assert [item["weight"] for item in plain["items"]] == [item["weight"] for item in coloured["items"]]
        assert not plain["conflicts"]
        assert not any("color" in item for item in plain["items"])


def test_every_extra_instance_validates() -> None:
    for name, build in quality.instances(extra=True).items():
        model = BinPackingProblemModel.model_validate(build())
        assert model.items, name
    assert len(quality.instances(extra=True)["web-demo-x6"]()["items"]) == 120


def test_a_short_step_limited_solve_is_no_worse_than_0_1_5() -> None:
    config = bin_packing.SolverConfig()
    config.step_count_limit = 50_000
    config.random_seed = 1
    for name, reference in quality.BINS_0_1_5.items():
        core = BinPackingProblemModel.model_validate(quality.instances()[name]()).to_core()
        score = bin_packing.solve(core, config=config)
        assert score.hard == 0, name
        assert core.bins_used() <= reference, name
