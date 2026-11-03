"""Engine vs CP-SAT comparison for order-picking.

The CP-SAT model is ``external_solver.order_picking`` (github.com/optimaru/external-solver).
Importing this package points it at the engine in this checkout, so both solvers
use the same ``order_picking`` (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("order_picking")
