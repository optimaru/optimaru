import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from bench.data import parse_orlib_bpp

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
    problems = parse_orlib_bpp(ORLIB_SAMPLE)

    assert list(problems) == ["u3_00", "t3_00"]
    assert problems["u3_00"].bin_capacity == 10
    assert [item.weight for item in problems["u3_00"].items] == [4, 5, 6]
    # One-decimal triplet sizes are scaled to integers with the capacity.
    assert problems["t3_00"].bin_capacity == 1000
    assert [item.weight for item in problems["t3_00"].items] == [334, 333, 333]


def test_orlib_bpp_parser_rejects_a_truncated_file() -> None:
    with pytest.raises(ValueError):
        parse_orlib_bpp(ORLIB_SAMPLE.rsplit("\n", 2)[0])
