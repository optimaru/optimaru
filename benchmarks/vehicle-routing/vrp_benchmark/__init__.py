"""CVRP benchmark harness: optimaru vs OR-Tools Routing."""

import os

# A benchmark measures this machine: with an API key in the environment the engine's side
# would be solved on the optimaru API.
os.environ.pop("OPTIMARU_API_KEY", None)
