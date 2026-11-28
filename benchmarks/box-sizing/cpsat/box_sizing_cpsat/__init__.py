"""Engine vs CP-SAT comparison for box sizing.

The CP-SAT model is this package's own (``box_sizing_cpsat.model``). Importing the
package checks that ``optimaru.box_sizing`` is the engine in this checkout, so both
solvers use the same modules (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("box_sizing")
