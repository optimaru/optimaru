"""Engine vs CP-SAT comparison for knapsack.

The CP-SAT model is ``external_solver.knapsack`` (github.com/optimaru/external-solver).
Importing this package checks that ``optimaru.knapsack`` is the engine in this
checkout, so both solvers use the same modules (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("knapsack")
