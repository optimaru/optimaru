from datetime import date, datetime, timedelta

from food_packaging.models import (
    JobModel,
    LineModel,
    OperatorModel,
    PackagingScheduleModel,
    ProductModel,
)
import pytest

from food_packaging import calculate_score, constraint_costs
from food_packaging.constraints import get_constraints
from food_packaging_cpsat import catalog, solve


def test_smoke_and_plugin_catalog():
    product = ProductModel(
        id="P", name="Product", cleaning_durations={"P": timedelta(0)}
    )
    problem = PackagingScheduleModel(
        from_date=date(2026, 1, 1),
        to_date=date(2026, 1, 2),
        products=[product],
        operators=[OperatorModel(id="O")],
        lines=[LineModel(id="L", name="Line", start_date_time=datetime(2026, 1, 1))],
        jobs=[
            JobModel(
                id="J",
                name="Job",
                product_id="P",
                duration=timedelta(minutes=30),
                min_start_time=datetime(2026, 1, 1),
                ideal_end_time=datetime(2026, 1, 1, 1),
                max_end_time=datetime(2026, 1, 1, 2),
            )
        ],
    )
    result = solve(problem, time_limit_seconds=2)
    assert result.lines[0].job_ids == ["J"]
    assert result.jobs[0].end_date_time == datetime(2026, 1, 1, 0, 30)
    assert result.score_hard == 0
    assert len(catalog()) == 5
    shared = calculate_score(result.to_core())
    assert (result.score_hard, result.score_medium, result.score_soft) == (
        shared.hard,
        shared.medium,
        shared.soft,
    )


def test_large_horizon_keeps_objective_within_cp_sat_integer_domain():
    start = datetime(2026, 1, 1)
    product = ProductModel(
        id="P", name="Product", cleaning_durations={"P": timedelta(0)}
    )
    problem = PackagingScheduleModel(
        from_date=start.date(),
        to_date=(start + timedelta(minutes=200_000)).date(),
        products=[product],
        operators=[OperatorModel(id="O")],
        lines=[LineModel(id="L", name="Line", start_date_time=start)],
        jobs=[
            JobModel(
                id=f"J{index}",
                name=f"Job {index}",
                product_id="P",
                duration=timedelta(minutes=30),
                min_start_time=start,
                ideal_end_time=start + timedelta(minutes=200_000),
                max_end_time=start + timedelta(minutes=200_000),
            )
            for index in range(10)
        ],
    )

    result = solve(problem, time_limit_seconds=2)

    assert result.score_hard == 0
    assert len(result.lines[0].job_ids) == 10


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_cpsat_constraint_matches_shared_raw_cost(constraint):
    start = datetime(2026, 1, 1)
    products = [
        ProductModel(
            id="A",
            name="A",
            cleaning_durations={"A": timedelta(0), "B": timedelta(minutes=60)},
        ),
        ProductModel(
            id="B",
            name="B",
            cleaning_durations={"A": timedelta(minutes=40), "B": timedelta(0)},
        ),
    ]

    def job(identifier, product, duration, ideal, maximum):
        return JobModel(
            id=identifier,
            name=identifier,
            product_id=product,
            duration=timedelta(minutes=duration),
            min_start_time=start,
            ideal_end_time=start + timedelta(minutes=ideal),
            max_end_time=start + timedelta(minutes=maximum),
        )

    problem = PackagingScheduleModel(
        from_date=start.date(),
        to_date=(start + timedelta(days=1)).date(),
        products=products,
        operators=[OperatorModel(id="O1"), OperatorModel(id="O2")],
        lines=[
            LineModel(id="L1", name="L1", start_date_time=start),
            LineModel(id="L2", name="L2", start_date_time=start),
        ],
        jobs=[
            job("J0", "B", 10, 5, 8),
            job("J1", "A", 100, 80, 100),
            job("J2", "B", 30, 20, 25),
            job("J3", "A", 100, 100, 110),
            job("J4", "A", 50, 20, 30),
        ],
    )
    proxy: dict[str, int] = {}
    result = solve(
        problem,
        time_limit_seconds=3,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_line_orders=[[0, 1], [2, 3]],
        _fixed_operator_by_line=[0, 0],
        _proxy_score=proxy,
    )

    expected = -sum(constraint_costs(result.to_core())[constraint.name]["costs"])
    assert sum(proxy.values()) == expected
