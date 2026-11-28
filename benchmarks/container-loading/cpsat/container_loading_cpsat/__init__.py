"""Engine vs CP-SAT comparison for container loading.

The CP-SAT model is in this package (``container_loading_cpsat.model``), not in
external-solver: it places the items freely in space, where the engine places them by
decoding a loading order, so its optimum is a bound on what any loading order can reach.
Importing this package checks that ``optimaru.container_loading`` is the engine in this
checkout (see ``cpsat_plugins.checkout``).
"""

from cpsat_plugins.checkout import use_checkout_engine

use_checkout_engine("container_loading")
