import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from bench.data import parse_bpp_data


def test_hexaly_bpp_parser_maps_capacity_and_weights() -> None:
    problem = parse_bpp_data("3 10 4 5 6")

    assert problem.bin_capacity == 10
    assert [item.weight for item in problem.items] == [4, 5, 6]
