"""Engine vs CP-SAT comparison for pickup-delivery.

The CP-SAT model is ``external_solver.pickup_delivery`` (github.com/optimaru/external-solver).
Importing this package points it at the engine in this checkout, so both solvers
use the same ``pdp_routing`` (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("pdp_routing")
