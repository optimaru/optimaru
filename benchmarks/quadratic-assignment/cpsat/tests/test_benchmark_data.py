import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from bench.data import apply_permutation, parse_qap_data, parse_qap_solution
from quadratic_assignment import constraint_costs


def test_qaplib_parsers_and_reference_rescore() -> None:
    problem = parse_qap_data("2 0 3 3 0 0 5 5 0")
    reference = parse_qap_solution("2 30 1 2")

    apply_permutation(problem, reference.permutation)

    assert problem.distances == [[0, 3], [3, 0]]
    assert problem.flows == [[0, 5], [5, 0]]
    assert constraint_costs(problem)["minimize_flow_distance"] == [30]
    assert reference.objective == 30


def test_qaplib_solution_parser_accepts_zero_based_permutations() -> None:
    reference = parse_qap_solution("3 42 2 0 1")

    assert reference.objective == 42
    assert reference.permutation == (2, 0, 1)
