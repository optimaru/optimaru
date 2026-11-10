import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from bench.data import parse_orlib_bpp
from bench.run_bench import main, sample_per_class

ORLIB_SAMPLE = """\
2
 u3_00
 10 3 2
4
5
6
 t3_00
 100.0 3 1
33.4
 33.3
33.3
"""


def test_orlib_bpp_parser_maps_capacity_and_weights() -> None:
    instances = parse_orlib_bpp(ORLIB_SAMPLE)

    assert list(instances) == ["u3_00", "t3_00"]
    assert instances["u3_00"].best_known_bins == 2
    assert instances["t3_00"].best_known_bins == 1
    # One scale per file: the one-decimal triplet sizes make it 10, and the
    # integral instance of the same file shares that unit.
    assert instances["u3_00"].problem.bin_capacity == 100
    assert [item.weight for item in instances["u3_00"].problem.items] == [40, 50, 60]
    assert instances["t3_00"].problem.bin_capacity == 1000
    assert [item.weight for item in instances["t3_00"].problem.items] == [334, 333, 333]


def test_orlib_bpp_parser_keeps_integral_files_unscaled() -> None:
    instances = parse_orlib_bpp(ORLIB_SAMPLE.split(" t3_00")[0].replace("2\n", "1\n", 1))

    assert instances["u3_00"].problem.bin_capacity == 10
    assert [item.weight for item in instances["u3_00"].problem.items] == [4, 5, 6]


def test_orlib_bpp_parser_rejects_a_truncated_file() -> None:
    with pytest.raises(ValueError):
        parse_orlib_bpp(ORLIB_SAMPLE.rsplit("\n", 2)[0])


def test_orlib_bpp_parser_rejects_trailing_data() -> None:
    with pytest.raises(ValueError, match="trailing data"):
        parse_orlib_bpp(ORLIB_SAMPLE + "7\n")


def test_orlib_bpp_parser_rejects_an_empty_file() -> None:
    with pytest.raises(ValueError, match="empty"):
        parse_orlib_bpp("")


def test_sample_per_class_spreads_evenly_from_first_to_last() -> None:
    names = [f"u120_{index:02d}" for index in range(20)] + [f"t60_{index:02d}" for index in range(20)]
    assert sample_per_class(names, 3) == ["u120_00", "u120_10", "u120_19", "t60_00", "t60_10", "t60_19"]
    assert sample_per_class(names, 2) == ["u120_00", "u120_19", "t60_00", "t60_19"]
    assert sample_per_class(names, 1) == ["u120_09", "t60_09"]
    # Never more than a class holds, and never an instance twice.
    assert sample_per_class(["a_0", "a_1", "b_0"], 5) == ["a_0", "a_1", "b_0"]


def test_one_way_of_choosing_instances_per_run() -> None:
    for argv in (["--all", "--instance", "u120_00"], ["--sample-per-class", "3", "--instance", "u120_00"]):
        with pytest.raises(SystemExit) as exit_info:
            main(argv)
        assert exit_info.value.code == 2
