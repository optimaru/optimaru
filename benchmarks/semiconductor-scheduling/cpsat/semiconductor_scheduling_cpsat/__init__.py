"""Engine vs CP-SAT comparison for semiconductor-scheduling.

The CP-SAT model is ``external_solver.semiconductor_scheduling`` (github.com/optimaru/external-solver).
Importing this package points it at the engine in this checkout, so both solvers
use the same ``semiconductor_scheduling`` (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("semiconductor_scheduling")
