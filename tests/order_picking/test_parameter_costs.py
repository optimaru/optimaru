"""Order picking has no optional parameter that only a cost reads: any selection is taken."""

from optimaru.order_picking.constraints import TrolleyCapacityCost
from optimaru.order_picking.models import parameter_uses, solve_request

from .test_initial_solution import problem


def test_every_cost_read_field_is_required_so_none_is_checked():
    assert parameter_uses(problem()) == []
    solve_request(problem(), constraints=[TrolleyCapacityCost()])


def test_a_generator_of_picks_is_read_once():
    solve_request(problem(), constraints=(pick for pick in [TrolleyCapacityCost()]))
