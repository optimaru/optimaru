"""Engine vs CP-SAT comparison for task-assigning.

The CP-SAT model is ``external_solver.task_assigning`` (github.com/optimaru/external-solver).
Importing this package points it at the engine in this checkout, so both solvers
use the same ``task_assigning`` (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("task_assigning")
