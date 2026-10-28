from __future__ import annotations

import hashlib
import io
import re
import tarfile
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

from quadratic_assignment import QuadraticAssignmentProblemModel

HEXALY_QAP_URL = "https://www.hexaly.com/examples/qap.zip"
HEXALY_QAP_SHA256 = "16fc0a8b0ab048074a9f23728c1c98c5b01d98f3c61118b6f205f3865f1c0de9"
QAPLIB_SOLUTIONS_URL = (
    "https://coral.ise.lehigh.edu/wp-content/uploads/2014/07/qapsoln.tar.gz"
)
QAPLIB_SOLUTIONS_SHA256 = (
    "002243dea9452000ec7ee5e43bd0aa651a639eb61039c4f35679531e67d71744"
)


@dataclass(frozen=True)
class QapReference:
    objective: int
    permutation: tuple[int, ...]


def _download(url: str, destination: Path, sha256: str) -> Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if (
        destination.exists()
        and hashlib.sha256(destination.read_bytes()).hexdigest() == sha256
    ):
        return destination
    request = urllib.request.Request(
        url, headers={"User-Agent": "optimaru-benchmark/1.0"}
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        payload = response.read()
    actual = hashlib.sha256(payload).hexdigest()
    if actual != sha256:
        raise ValueError(
            f"checksum mismatch for {url}: expected {sha256}, got {actual}"
        )
    destination.write_bytes(payload)
    return destination


def download_archives(cache_dir: Path) -> tuple[Path, Path]:
    return (
        _download(HEXALY_QAP_URL, cache_dir / "hexaly-qap.zip", HEXALY_QAP_SHA256),
        _download(
            QAPLIB_SOLUTIONS_URL,
            cache_dir / "qaplib-solutions.tar.gz",
            QAPLIB_SOLUTIONS_SHA256,
        ),
    )


def parse_qap_data(payload: str) -> QuadraticAssignmentProblemModel:
    values = [int(value) for value in payload.split()]
    if not values:
        raise ValueError("empty QAP instance")
    size = values[0]
    expected = 1 + 2 * size * size
    if len(values) != expected:
        raise ValueError(
            f"expected {expected} integers for a {size}x{size} QAP, got {len(values)}"
        )
    cursor = 1
    distances = [
        values[cursor + row * size : cursor + (row + 1) * size] for row in range(size)
    ]
    cursor += size * size
    flows = [
        values[cursor + row * size : cursor + (row + 1) * size] for row in range(size)
    ]
    return QuadraticAssignmentProblemModel(
        facilities=[f"facility-{index + 1}" for index in range(size)],
        locations=[f"location-{index + 1}" for index in range(size)],
        distances=distances,
        flows=flows,
    )


def parse_qap_solution(payload: str) -> QapReference:
    values = [int(value) for value in re.findall(r"-?\d+", payload)]
    if len(values) < 2:
        raise ValueError("invalid QAPLIB solution")
    size, objective = values[:2]
    raw_permutation = tuple(values[2:])
    if set(raw_permutation) == set(range(1, size + 1)):
        permutation = tuple(value - 1 for value in raw_permutation)
    else:
        permutation = raw_permutation
    if len(permutation) != size or set(permutation) != set(range(size)):
        raise ValueError("QAPLIB solution is not a complete one-based permutation")
    return QapReference(objective=objective, permutation=permutation)


def load_suite(
    cache_dir: Path,
) -> dict[str, tuple[QuadraticAssignmentProblemModel, QapReference | None]]:
    data_archive, solution_archive = download_archives(cache_dir)
    references: dict[str, QapReference] = {}
    with tarfile.open(solution_archive, "r:gz") as archive:
        for member in archive.getmembers():
            if not member.isfile() or not member.name.lower().endswith(".sln"):
                continue
            extracted = archive.extractfile(member)
            if extracted is not None:
                references[Path(member.name).stem.lower()] = parse_qap_solution(
                    extracted.read().decode("ascii")
                )

    suite: dict[str, tuple[QuadraticAssignmentProblemModel, QapReference | None]] = {}
    with zipfile.ZipFile(io.BytesIO(data_archive.read_bytes())) as archive:
        for name in sorted(archive.namelist()):
            if not name.lower().endswith(".dat"):
                continue
            instance = Path(name).stem.lower()
            suite[instance] = (
                parse_qap_data(archive.read(name).decode("ascii")),
                references.get(instance),
            )
    return suite


def apply_permutation(
    problem: QuadraticAssignmentProblemModel, permutation: tuple[int, ...]
) -> None:
    problem.assignment = {
        location: problem.facilities[permutation[index]]
        for index, location in enumerate(problem.locations)
    }
