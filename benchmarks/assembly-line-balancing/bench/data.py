from __future__ import annotations

import hashlib
import io
import re
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

from assembly_line_balancing.models import (
    AssemblyLineBalancingProblemModel,
    PrecedenceModel,
    TaskModel,
)

SALBP_URL = (
    "https://assembly-line-balancing.de/wp-content/uploads/2017/01/SALBP_benchmark.zip"
)
SALBP_SHA256 = "379a419ff000aab62b12662f6224978f5fc875967ac5944b6a7d09d4555417cd"

DATASETS = {
    "small": ("small data set_n=20.zip", "Details of the small data set (n=20).xlsx"),
    "medium": (
        "medium data set_n=50.zip",
        "Details of the medium data set (n=50 and n=50permuted).xlsx",
    ),
    "medium-permuted": (
        "medium data set_n=50permuted.zip",
        "Details of the medium data set (n=50 and n=50permuted).xlsx",
    ),
    "large": ("large data set_n=100.zip", "Details of the large data set (n=100).xlsx"),
    "very-large": (
        "very large data set_n=1000.zip",
        "Details of the very large data set (n=1000).xlsx",
    ),
}

_XML_NS = {"x": "http://schemas.openxmlformats.org/spreadsheetml/2006/main"}


def _download(destination: Path) -> Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if (
        destination.exists()
        and hashlib.sha256(destination.read_bytes()).hexdigest() == SALBP_SHA256
    ):
        return destination
    request = urllib.request.Request(
        SALBP_URL, headers={"User-Agent": "optimaru-benchmark/1.0"}
    )
    with urllib.request.urlopen(request, timeout=180) as response:
        payload = response.read()
    actual = hashlib.sha256(payload).hexdigest()
    if actual != SALBP_SHA256:
        raise ValueError(
            f"checksum mismatch for {SALBP_URL}: expected {SALBP_SHA256}, got {actual}"
        )
    destination.write_bytes(payload)
    return destination


def parse_alb_data(payload: str) -> AssemblyLineBalancingProblemModel:
    sections: dict[str, list[str]] = {}
    current = ""
    for raw_line in payload.replace("\r", "").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("<") and line.endswith(">"):
            current = line[1:-1].strip().lower()
            sections.setdefault(current, [])
        else:
            sections.setdefault(current, []).append(line)

    size = int(sections["number of tasks"][0])
    cycle_time = int(sections["cycle time"][0])
    durations = {}
    for line in sections["task times"]:
        task, duration = line.split()
        durations[int(task)] = int(duration)
    if set(durations) != set(range(1, size + 1)):
        raise ValueError("SALBP task-time section is incomplete")
    precedences = []
    for line in sections.get("precedence relations", []):
        before, after = (int(value) for value in re.split(r"\s*,\s*", line))
        precedences.append(PrecedenceModel(before=str(before), after=str(after)))
    return AssemblyLineBalancingProblemModel(
        cycle_time=cycle_time,
        max_stations=size,
        tasks=[
            TaskModel(id=str(task), duration=durations[task])
            for task in range(1, size + 1)
        ],
        precedences=precedences,
    )


def _column(reference: str) -> int:
    letters = "".join(character for character in reference if character.isalpha())
    value = 0
    for character in letters:
        value = value * 26 + ord(character.upper()) - ord("A") + 1
    return value - 1


def parse_xlsx_best_known(payload: bytes) -> dict[str, int]:
    with zipfile.ZipFile(io.BytesIO(payload)) as workbook:
        shared_root = ET.fromstring(workbook.read("xl/sharedStrings.xml"))
        shared = [
            "".join(text.text or "" for text in item.findall(".//x:t", _XML_NS))
            for item in shared_root.findall("x:si", _XML_NS)
        ]
        sheet = ET.fromstring(workbook.read("xl/worksheets/sheet1.xml"))

    rows: list[dict[int, str]] = []
    for row in sheet.findall(".//x:row", _XML_NS):
        values = {}
        for cell in row.findall("x:c", _XML_NS):
            value = cell.find("x:v", _XML_NS)
            if value is None or value.text is None:
                continue
            raw = shared[int(value.text)] if cell.get("t") == "s" else value.text
            values[_column(cell.get("r", ""))] = raw.strip()
        rows.append(values)

    # The official workbook uses B for the stem and I for the best feasible upper
    # bound. Column G can contain an interval when optimality is still unknown.
    result = {}
    for row in rows[2:]:
        name, best_known = row.get(1), row.get(8)
        if name and best_known:
            result[name.lower()] = int(float(best_known))
    return result


def load_suite(
    cache_dir: Path, dataset: str
) -> dict[str, tuple[AssemblyLineBalancingProblemModel, int | None]]:
    try:
        nested_name, details_name = DATASETS[dataset]
    except KeyError as error:
        raise ValueError(
            f"unknown SALBP dataset {dataset!r}; choose from {', '.join(DATASETS)}"
        ) from error
    archive_path = _download(cache_dir / "salbp-benchmark.zip")
    with zipfile.ZipFile(archive_path) as outer:
        nested_payload = outer.read(nested_name)
        references = parse_xlsx_best_known(outer.read(details_name))
    suite = {}
    with zipfile.ZipFile(io.BytesIO(nested_payload)) as nested:
        for name in sorted(nested.namelist()):
            if not name.lower().endswith(".alb"):
                continue
            stem = Path(name).stem.lower()
            suite[stem] = (
                parse_alb_data(nested.read(name).decode("ascii")),
                references.get(stem),
            )
    return suite
