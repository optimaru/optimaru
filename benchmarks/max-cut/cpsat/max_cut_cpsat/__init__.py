"""Engine vs CP-SAT comparison for max-cut.

The CP-SAT model is ``external_solver.max_cut`` (github.com/optimaru/external-solver).
Importing this package points it at the engine in this checkout, so both solvers
use the same ``max_cut`` (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("max_cut")
