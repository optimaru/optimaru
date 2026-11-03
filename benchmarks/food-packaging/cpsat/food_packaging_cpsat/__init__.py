"""Engine vs CP-SAT comparison for food-packaging.

The CP-SAT model is ``external_solver.food_packaging`` (github.com/optimaru/external-solver).
Importing this package points it at the engine in this checkout, so both solvers
use the same ``food_packaging`` (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("food_packaging")
