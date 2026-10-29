"""A case whose CP-SAT side never returns, for the wall-clock bound test."""

import time

NAME = "stalled"
PACKAGE = "stalled_package"


def instances():
    return {"only": lambda: None}


def solve_cpsat(problem, seconds: float, seed: int, workers: int):
    time.sleep(600)
