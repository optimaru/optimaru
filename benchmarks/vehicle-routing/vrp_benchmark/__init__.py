"""CVRP benchmark harness: optimaru vs OR-Tools Routing."""

import os

import optimaru

# A benchmark measures this machine: with an API key the engine's side would be solved on the
# optimaru API. The variable goes for the processes this one starts too.
os.environ.pop("OPTIMARU_API_KEY", None)
optimaru.use_local()
