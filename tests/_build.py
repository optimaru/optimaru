"""What a test may assume about the build it runs in."""

import importlib
import os

import pytest


def use_case_module(use_case: str):
    """``optimaru.<use case>``, or a skip when OPTIMARU_USE_CASES narrowed this build to other
    use cases. An extension that was built and does not import is an error, not a skip."""
    wanted = os.environ.get("OPTIMARU_USE_CASES", "").replace(",", " ").split()
    if wanted and use_case not in wanted:
        pytest.skip(f"optimaru.{use_case} is not in this build (OPTIMARU_USE_CASES)")
    return importlib.import_module(f"optimaru.{use_case}")
