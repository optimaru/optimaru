import io
import sys
import tarfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import bench.data
from bench.data import apply_permutation, parse_qap_data, parse_qap_solution
from quadratic_assignment import constraint_costs


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
