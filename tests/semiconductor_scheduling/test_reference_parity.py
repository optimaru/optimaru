"""The C++ implementation against the pure-Python reference scorer and decoder (the oracle).

Same input and assignment -> the same raw cost per constraint (and the same per-item lists),
the same default score, the same decoded schedule (order and batch ids included) and the
same seed conversion — on every sample and on seeded random instances and search states.
"""

import json
from pathlib import Path

import pytest

import optimaru.semiconductor_scheduling as fab
from optimaru.semiconductor_scheduling.models import SemiconductorScheduleModel

from . import reference_semiconductor_scheduling as ref
from .fab_random import instance, search_state

# A copy of the web demo sample (web: src/lib/demo-data/semiconductor-scheduling.json).
WEB_DEMO = Path(__file__).parent / "data" / "web-demo.json"
RANDOM_INSTANCES = 200


def _samples() -> list[tuple[str, dict]]:
    """The web demo (with a pinned variant) and the package's test fixtures."""
    out = []
    if WEB_DEMO.exists():
        web = json.loads(WEB_DEMO.read_text())
        out.append(("web-demo", web))
        pinned = json.loads(WEB_DEMO.read_text())
        pinned["pinned_assignments"] = [
            {"lot_id": "LOT-A01", "operation_id": "PHOTO-1", "machine_id": "LITHO-02", "start": 2, "end": 11}
        ]
        out.append(("web-demo-pinned", pinned))
    from .test_initial_solution import QUEUE_SEED, _batch_problem, _choice_problem, _queue_problem
    from .test_semiconductor_scheduling import _fab_problem

    out.append(("fab", _fab_problem(with_assignments=True).model_dump(mode="json")))
    out.append(("fab-empty", _fab_problem(with_assignments=False).model_dump(mode="json")))
    queue = _queue_problem().model_dump(mode="json")
    queue["assignments"] = QUEUE_SEED
    out.append(("queue-seed", queue))
    out.append(("choice", _choice_problem().model_dump(mode="json")))
    out.append(("batch", _batch_problem().model_dump(mode="json")))
    return out


SAMPLES = _samples()
CASES = [(name, data) for name, data in SAMPLES] + [
    (f"random-{seed}", instance(seed)) for seed in range(RANDOM_INSTANCES)
]


def _model(data: dict) -> SemiconductorScheduleModel:
    return SemiconductorScheduleModel.model_validate(data)


def _rows(assignments) -> list[tuple]:
    return [(a.lot_id, a.operation_id, a.machine_id, a.start, a.end, a.batch_id) for a in assignments]


def _assert_costs_match(model: SemiconductorScheduleModel) -> None:
    oracle = ref.constraint_costs(model)
    costs = fab.constraint_costs(model)
    items = fab.constraint_items(model)
    assert list(costs) == ref.ENGINE_NAMES
    for reference, engine in ref.ENGINE_NAME.items():
        assert costs[engine]["costs"] == [sum(oracle[reference])], engine
        assert items[engine] == oracle[reference], engine
    assert tuple(fab.calculate_score(model.model_copy(deep=True))) == ref.score(oracle)
    assert tuple(fab.constraints.score(costs)) == ref.score(oracle)


@pytest.mark.parametrize(("name", "data"), CASES, ids=[name for name, _ in CASES])
def test_raw_costs_items_and_score_match_the_oracle(name, data):
    _assert_costs_match(_model(data))


@pytest.mark.parametrize(("name", "data"), CASES, ids=[name for name, _ in CASES])
def test_decoder_matches_the_oracle(name, data):
    """Random (choices, ranks) states decode to the same schedule; its costs match too."""
    model = _model(data)
    rows = fab._operation_rows(model)
    counts = [len(operation.eligible_machines) for _lot, _index, operation in rows]
    seed = sum(map(ord, name))
    for k in range(3):
        choices, ranks = search_state(seed * 10 + k, len(rows), counts)
        try:
            expected = _rows(ref.construct(model, choices, ranks))
        except RuntimeError:
            with pytest.raises(RuntimeError):
                fab._construct(model, choices, ranks)
            continue
        decoded = fab._construct(model, choices, ranks)
        assert _rows(decoded) == expected
        _assert_costs_match(model.model_copy(update={"assignments": decoded}))


@pytest.mark.parametrize(("name", "data"), CASES, ids=[name for name, _ in CASES])
def test_seed_state_and_default_choices_match_the_oracle(name, data):
    model = _model(data)
    rows = fab._operation_rows(model)
    core = model.to_core()
    assert fab._core.default_choices(core) == ref.default_choices(model)
    choices, _ranks = search_state(len(name), len(rows), [len(op.eligible_machines) for _l, _i, op in rows])
    choices = [choice % len(op.eligible_machines) for choice, (_l, _i, op) in zip(choices, rows, strict=True)]
    assert fab._seed_state(model, rows, choices) == ref.seed_state(model, choices)


@pytest.mark.parametrize(("name", "data"), SAMPLES, ids=[name for name, _ in SAMPLES])
def test_a_zero_budget_solve_is_the_oracle_start(name, data):
    """The solve start is the oracle's: the default choices at identity ranks (or the seed), decoded."""
    model = _model(data)
    rows = fab._operation_rows(model)
    expected = _rows(ref.construct(model, ref.default_choices(model), list(range(len(rows)))))
    config = fab.SolverConfig()
    config.spent_limit_seconds = 0
    model = fab.solve(model, config=config)
    assert _rows(model.assignments) == expected
