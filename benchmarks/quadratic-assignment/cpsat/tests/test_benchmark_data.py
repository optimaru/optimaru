import io
import sys
import tarfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import bench.data
from bench.data import QapReference, apply_permutation, parse_qap_data, parse_qap_solution
from bench.run_bench import REFERENCE_ERRATA, reference_objective
from optimaru.quadratic_assignment import constraint_costs


def test_qaplib_parsers_and_reference_rescore() -> None:
    problem = parse_qap_data("2 0 3 3 0 0 5 5 0")
    reference = parse_qap_solution("2 30 1 2")

    apply_permutation(problem, reference.permutation)

    assert problem.distances == [[0, 3], [3, 0]]
    assert problem.flows == [[0, 5], [5, 0]]
    assert constraint_costs(problem)["flow_distance_cost"]["costs"] == [30]
    assert reference.objective == 30


def test_qaplib_solution_parser_accepts_zero_based_permutations() -> None:
    reference = parse_qap_solution("3 42 2 0 1")

    assert reference.objective == 42
    assert reference.permutation == (2, 0, 1)


def _tar_gz(path: Path, members: dict[str, str]) -> Path:
    with tarfile.open(path, "w:gz") as archive:
        for name, text in members.items():
            data = text.encode("ascii")
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    return path


def test_load_suite_reports_missing_suite_instances(tmp_path, monkeypatch) -> None:
    data = _tar_gz(tmp_path / "data.tar.gz", {"nug30.dat": "2 0 3 3 0 0 5 5 0"})
    solutions = _tar_gz(tmp_path / "soln.tar.gz", {"nug30.sln": "2 30 1 2"})
    monkeypatch.setattr(bench.data, "download_archives", lambda _: (data, solutions))

    with pytest.raises(ValueError, match="QAPLIB archive lacks esc32a"):
        bench.data.load_suite(tmp_path)


def test_load_suite_keeps_only_the_suite(tmp_path, monkeypatch) -> None:
    members = {f"{name}.dat": "2 0 3 3 0 0 5 5 0" for name in bench.data.SUITE}
    members["chr12a.dat"] = "2 0 1 1 0 0 1 1 0"
    data = _tar_gz(tmp_path / "data.tar.gz", members)
    solutions = _tar_gz(tmp_path / "soln.tar.gz", {"nug30.sln": "2 30 1 2"})
    monkeypatch.setattr(bench.data, "download_archives", lambda _: (data, solutions))

    suite = bench.data.load_suite(tmp_path)

    assert list(suite) == list(bench.data.SUITE)
    assert suite["nug30"][1].objective == 30
    assert suite["tai30a"][1] is None


def test_reference_objective_reads_a_solution_file_in_either_index_convention(monkeypatch) -> None:
    # Asymmetric matrices, so a permutation and its inverse score differently.
    problem = parse_qap_data("3  0 1 2  3 0 4  5 6 0   0 7 1  2 0 3  4 5 0")

    def score(permutation: tuple[int, ...]) -> int:
        placed = problem.model_copy(deep=True)
        apply_permutation(placed, permutation)
        return constraint_costs(placed)["flow_distance_cost"]["costs"][0]

    permutation, inverse = (1, 2, 0), (2, 0, 1)
    assert score(permutation) != score(inverse)
    for stated in (score(permutation), score(inverse)):
        assert reference_objective("any", problem, QapReference(stated, permutation)) == stated
    # A file known to state another value than its permutation scores.
    monkeypatch.setitem(REFERENCE_ERRATA, "misprint", score(inverse))
    assert reference_objective("misprint", problem, QapReference(1, permutation)) == score(inverse)
    # Any other mismatch is a parsing or scoring bug, not a reference.
    with pytest.raises(AssertionError, match="reference mismatch for other: the solution file states 1"):
        reference_objective("other", problem, QapReference(1, permutation))
