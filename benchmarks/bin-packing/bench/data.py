"""Falkenauer's (1996) uniform and triplet bin-packing instances.

The 160 instances (u120/u250/u500/u1000 and t60/t120/t249/t501, 20 each) are
read from J. E. Beasley's OR-Library files `binpack1.txt` .. `binpack8.txt`,
where Falkenauer published them. Every file is SHA-256 pinned.
"""

from __future__ import annotations

import hashlib
import urllib.request
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path

from optimaru.bin_packing.models import BinPackingProblemModel, ItemModel

ORLIB_BASE_URL = "https://people.brunel.ac.uk/~mastjjb/jeb/orlib/files/"
# binpack1-4: uniform u120..u1000; binpack5-8: triplets t60..t501.
ORLIB_BPP_SHA256 = {
    "binpack1.txt": "891fe4b3be86371b120ccaf37ce4525b5b0a2fc249d38725cd3216fbaa42b172",
    "binpack2.txt": "86fd8c3d3007047b2ed2e6d2d09924c04d286070968804643d20fd56f9123331",
    "binpack3.txt": "24ef075e7f6f26f55dddba907d2d4fc117b04ac543e0a35d595b3635435e1e02",
    "binpack4.txt": "a87d04d0157b829a21538ca2e52331c55f367440a9f9e131f2a97290e415ebc6",
    "binpack5.txt": "33cb019a3f05d972abeffdc37ebc6ee31bf84c69b5b2a7a0443b41277a74f546",
    "binpack6.txt": "b832c391bdd36221cc3e0285af474717ebd83481e7ee50a9bd88339fb8bbfd6b",
    "binpack7.txt": "30eb806cab226c398f9a524cad04d19dc64494e5e8778cb22357cd463e112ab8",
    "binpack8.txt": "841eb57d0417a70e9b60c617c590671ace73f39445a0424257a121c4876eadf6",
}


def _download(name: str, cache_dir: Path) -> Path:
    expected = ORLIB_BPP_SHA256[name]
    destination = cache_dir / "orlib-binpack" / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and hashlib.sha256(destination.read_bytes()).hexdigest() == expected:
        return destination
    url = ORLIB_BASE_URL + name
    request = urllib.request.Request(url, headers={"User-Agent": "optimaru-benchmark/1.0"})
    with urllib.request.urlopen(request, timeout=120) as response:
        payload = response.read()
    actual = hashlib.sha256(payload).hexdigest()
    if actual != expected:
        raise ValueError(f"checksum mismatch for {url}: expected {expected}, got {actual}")
    destination.write_bytes(payload)
    return destination


@dataclass(frozen=True)
class OrlibInstance:
    problem: BinPackingProblemModel
    # The bin count of the best known solution, as the file states it.
    best_known_bins: int


def parse_orlib_bpp(payload: str) -> dict[str, OrlibInstance]:
    """Parse an OR-Library bin-packing file into ``{instance id: instance}``.

    Layout: the number of problems, then per problem its identifier, a line
    with the bin capacity, the item count and the best known bin count, and
    one item size per line. The triplet files give one decimal digit
    (capacity 100.0); every value of a file is scaled by the same power of
    ten, so all stay integral and all instances of a file share one unit.
    """
    tokens = payload.split()
    if not tokens:
        raise ValueError("empty OR-Library bin-packing file")
    count = int(tokens[0])
    cursor = 1
    raw: list[tuple[str, Decimal, list[Decimal], int]] = []
    for _ in range(count):
        if cursor + 4 > len(tokens):
            raise ValueError("truncated OR-Library bin-packing file")
        name = tokens[cursor].lower()
        capacity = Decimal(tokens[cursor + 1])
        size = int(tokens[cursor + 2])
        best_known = int(tokens[cursor + 3])
        cursor += 4
        weights = [Decimal(value) for value in tokens[cursor : cursor + size]]
        if len(weights) != size:
            raise ValueError(f"{name}: expected {size} item sizes, got {len(weights)}")
        cursor += size
        raw.append((name, capacity, weights, best_known))
    if cursor != len(tokens):
        raise ValueError("trailing data after the last OR-Library bin-packing problem")

    digits = max(-value.as_tuple().exponent for _, capacity, weights, _ in raw for value in (capacity, *weights))
    scale = 10 ** max(digits, 0)

    def scaled(value: Decimal) -> int:
        result = value * scale
        if result != result.to_integral_value():
            raise ValueError(f"value {value} is not integral after scaling by {scale}")
        return int(result)

    return {
        name: OrlibInstance(
            problem=BinPackingProblemModel(
                bin_capacity=scaled(capacity),
                items=[
                    ItemModel(id=f"item-{index + 1}", weight=scaled(weight)) for index, weight in enumerate(weights)
                ],
            ),
            best_known_bins=best_known,
        )
        for name, capacity, weights, best_known in raw
    }


def load_suite(cache_dir: Path) -> dict[str, OrlibInstance]:
    suite: dict[str, OrlibInstance] = {}
    for name in ORLIB_BPP_SHA256:
        suite.update(parse_orlib_bpp(_download(name, cache_dir).read_text("ascii")))
    return dict(sorted(suite.items()))
