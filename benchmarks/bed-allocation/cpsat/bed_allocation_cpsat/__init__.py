"""Engine vs CP-SAT comparison for bed-allocation.

The CP-SAT model is ``external_solver.bed_allocation`` (github.com/optimaru/external-solver).
Importing this package points it at the engine in this checkout, so both solvers
use the same ``bed_allocation`` (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("bed_allocation")
