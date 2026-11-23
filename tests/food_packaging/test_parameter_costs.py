"""Food packaging has no parameter that acts only through a cost: the check finds nothing."""

from pathlib import Path

import optimaru.food_packaging as fp
from optimaru.food_packaging.constraints import SquaredMakespanCost
from optimaru.food_packaging.models import PackagingScheduleModel, parameter_uses

FIXTURES = Path(__file__).parent / "fixtures"


def test_nothing_in_a_schedule_needs_a_cost_of_its_own():
    model = PackagingScheduleModel.model_validate_json((FIXTURES / "rush_week_problem.json").read_text())
    assert parameter_uses(model) == []
    # Any selection is taken: the time windows are required fields, the operator a decision.
    fp.solve(model, time_limit_seconds=0.05, random_seed=1, constraints=iter([SquaredMakespanCost()]))
