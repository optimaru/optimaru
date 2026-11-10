from __future__ import annotations

import hashlib
import re
import tarfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from optimaru.quadratic_assignment import QuadraticAssignmentProblemModel

QAPLIB_DATA_URL = "https://coral.ise.lehigh.edu/wp-content/uploads/2014/07/qapdata.tar.gz"
QAPLIB_DATA_SHA256 = "c6bf0e277156d097df21124f53ca5a5d0fa5344481ee401e13acb67694e190b3"
QAPLIB_SOLUTIONS_URL = "https://coral.ise.lehigh.edu/wp-content/uploads/2014/07/qapsoln.tar.gz"
QAPLIB_SOLUTIONS_SHA256 = "002243dea9452000ec7ee5e43bd0aa651a639eb61039c4f35679531e67d71744"


# The benchmark covers every QAPLIB instance with 30 to 50 facilities (33 of
# the archive's 136); `load_suite` reads only these.
SUITE = (
    "esc32a", "esc32b", "esc32c", "esc32d", "esc32e", "esc32g", "esc32h",
    "kra30a", "kra30b", "kra32",
    "lipa30a", "lipa30b", "lipa40a", "lipa40b", "lipa50a", "lipa50b",
    "nug30",
    "sko42", "sko49",
    "ste36a", "ste36b", "ste36c",
    "tai30a", "tai30b", "tai35a", "tai35b", "tai40a", "tai40b", "tai50a", "tai50b",
    "tho30", "tho40",
    "wil50",
)  # fmt: skip


@dataclass(frozen=True)
class QapReference:
    objective: int
    permutation: tuple[int, ...]


def _download(url: str, destination: Path, sha256: str) -> Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and hashlib.sha256(destination.read_bytes()).hexdigest() == sha256:
        return destination
    request = urllib.request.Request(url, headers={"User-Agent": "optimaru-benchmark/1.0"})
    with urllib.request.urlopen(request, timeout=120) as response:
        payload = response.read()
    actual = hashlib.sha256(payload).hexdigest()
    if actual != sha256:
        raise ValueError(f"checksum mismatch for {url}: expected {sha256}, got {actual}")
    destination.write_bytes(payload)
    return destination


def download_archives(cache_dir: Path) -> tuple[Path, Path]:
    return (
        _download(QAPLIB_DATA_URL, cache_dir / "qaplib-data.tar.gz", QAPLIB_DATA_SHA256),
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
        raise ValueError(f"expected {expected} integers for a {size}x{size} QAP, got {len(values)}")
    cursor = 1
    distances = [values[cursor + row * size : cursor + (row + 1) * size] for row in range(size)]
    cursor += size * size
    flows = [values[cursor + row * size : cursor + (row + 1) * size] for row in range(size)]
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
                references[Path(member.name).stem.lower()] = parse_qap_solution(extracted.read().decode("ascii"))

    payloads: dict[str, str] = {}
    with tarfile.open(data_archive, "r:gz") as archive:
        for member in archive.getmembers():
            if not member.isfile() or not member.name.lower().endswith(".dat"):
                continue
            instance = Path(member.name).stem.lower()
            if instance not in SUITE:
                continue
            extracted = archive.extractfile(member)
            if extracted is not None:
                payloads[instance] = extracted.read().decode("ascii")
    missing = sorted(set(SUITE) - payloads.keys())
    if missing:
        raise ValueError(f"QAPLIB archive lacks {', '.join(missing)}")
    return {instance: (parse_qap_data(payloads[instance]), references.get(instance)) for instance in SUITE}


def apply_permutation(problem: QuadraticAssignmentProblemModel, permutation: tuple[int, ...]) -> None:
    problem.assignment = {
        location: problem.facilities[permutation[index]] for index, location in enumerate(problem.locations)
    }
