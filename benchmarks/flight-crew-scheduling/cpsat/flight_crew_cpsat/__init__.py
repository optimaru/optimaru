"""Engine vs CP-SAT comparison for flight-crew-scheduling.

The CP-SAT model is ``external_solver.flight_crew_scheduling`` (github.com/optimaru/external-solver).
Importing this package points it at the engine in this checkout, so both solvers
use the same ``flight_crew`` (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("flight_crew")
