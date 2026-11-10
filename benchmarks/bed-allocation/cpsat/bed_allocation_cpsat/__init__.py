"""Engine vs CP-SAT comparison for bed-allocation.

The CP-SAT model is ``external_solver.bed_allocation`` (github.com/optimaru/external-solver).
Importing this package checks that ``optimaru.bed_allocation`` is the engine in this
checkout, so both solvers use the same modules (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("bed_allocation")
