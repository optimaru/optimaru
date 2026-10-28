from __future__ import annotations

import hashlib
import urllib.request
import zipfile
from pathlib import Path

from bin_packing.models import BinPackingProblemModel, ItemModel

HEXALY_BPP_URL = "https://www.hexaly.com/examples/bin_packing.zip"
HEXALY_BPP_SHA256 = "0809c65a30d435d05ea54f5635c04cf6660a3a167452fe60c2babdb36f0d5273"


def _download(destination: Path) -> Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if (
        destination.exists()
        and hashlib.sha256(destination.read_bytes()).hexdigest() == HEXALY_BPP_SHA256
    ):
        return destination
    request = urllib.request.Request(
        HEXALY_BPP_URL, headers={"User-Agent": "optimaru-benchmark/1.0"}
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        payload = response.read()
    actual = hashlib.sha256(payload).hexdigest()
    if actual != HEXALY_BPP_SHA256:
        raise ValueError(
            f"checksum mismatch for {HEXALY_BPP_URL}: expected {HEXALY_BPP_SHA256}, got {actual}"
        )
    destination.write_bytes(payload)
    return destination


def parse_bpp_data(payload: str) -> BinPackingProblemModel:
    values = [int(value) for value in payload.split()]
    if len(values) < 2:
        raise ValueError("invalid bin-packing instance")
    size, capacity = values[:2]
    weights = values[2:]
    if len(weights) != size:
        raise ValueError(f"expected {size} item weights, got {len(weights)}")
    return BinPackingProblemModel(
        bin_capacity=capacity,
        items=[
            ItemModel(id=f"item-{index + 1}", weight=weight)
            for index, weight in enumerate(weights)
        ],
    )


def load_suite(cache_dir: Path) -> dict[str, BinPackingProblemModel]:
    archive_path = _download(cache_dir / "hexaly-bin-packing.zip")
    suite = {}
    with zipfile.ZipFile(archive_path) as archive:
        for name in sorted(archive.namelist()):
            if not name.lower().endswith(".txt"):
                continue
            suite[Path(name).stem.lower()] = parse_bpp_data(
                archive.read(name).decode("ascii")
            )
    return suite
