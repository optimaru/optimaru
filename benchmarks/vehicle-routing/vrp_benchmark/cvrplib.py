"""CVRPLIB Set X catalog and instance downloader.

Two sources carry the same 100 instances and the same best-known solutions:
the official CVRPLIB catalog page, and the PyVRP/Instances GitHub mirror
(its `.sol` files carry the CVRPLIB BKS, rounded to the nearest integer like
the instances). `load_x_catalog` reads the official page by default and falls
back to the mirror when the host is unreachable; `source="mirror"` forces the
mirror, which is what the CI benchmark uses because GitHub is the more
reliable host.
"""

from __future__ import annotations

import http.client
import re
import urllib.request
from dataclasses import dataclass, replace
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urljoin

CATALOG_URL = "https://galgos.inf.puc-rio.br/cvrplib/index.php/en/instances/1"
MIRROR_BASE_URL = "https://raw.githubusercontent.com/PyVRP/Instances/main/CVRP/"
CATALOG_SOURCES = ("auto", "cvrplib", "mirror")
# The 100 Set X instances of Uchoa et al. (2017); the set has been frozen
# since publication, so listing it here avoids needing a directory index.
SET_X_NAMES = (
    "X-n101-k25",
    "X-n106-k14",
    "X-n110-k13",
    "X-n115-k10",
    "X-n120-k6",
    "X-n125-k30",
    "X-n129-k18",
    "X-n134-k13",
    "X-n139-k10",
    "X-n143-k7",
    "X-n148-k46",
    "X-n153-k22",
    "X-n157-k13",
    "X-n162-k11",
    "X-n167-k10",
    "X-n172-k51",
    "X-n176-k26",
    "X-n181-k23",
    "X-n186-k15",
    "X-n190-k8",
    "X-n195-k51",
    "X-n200-k36",
    "X-n204-k19",
    "X-n209-k16",
    "X-n214-k11",
    "X-n219-k73",
    "X-n223-k34",
    "X-n228-k23",
    "X-n233-k16",
    "X-n237-k14",
    "X-n242-k48",
    "X-n247-k50",
    "X-n251-k28",
    "X-n256-k16",
    "X-n261-k13",
    "X-n266-k58",
    "X-n270-k35",
    "X-n275-k28",
    "X-n280-k17",
    "X-n284-k15",
    "X-n289-k60",
    "X-n294-k50",
    "X-n298-k31",
    "X-n303-k21",
    "X-n308-k13",
    "X-n313-k71",
    "X-n317-k53",
    "X-n322-k28",
    "X-n327-k20",
    "X-n331-k15",
    "X-n336-k84",
    "X-n344-k43",
    "X-n351-k40",
    "X-n359-k29",
    "X-n367-k17",
    "X-n376-k94",
    "X-n384-k52",
    "X-n393-k38",
    "X-n401-k29",
    "X-n411-k19",
    "X-n420-k130",
    "X-n429-k61",
    "X-n439-k37",
    "X-n449-k29",
    "X-n459-k26",
    "X-n469-k138",
    "X-n480-k70",
    "X-n491-k59",
    "X-n502-k39",
    "X-n513-k21",
    "X-n524-k153",
    "X-n536-k96",
    "X-n548-k50",
    "X-n561-k42",
    "X-n573-k30",
    "X-n586-k159",
    "X-n599-k92",
    "X-n613-k62",
    "X-n627-k43",
    "X-n641-k35",
    "X-n655-k131",
    "X-n670-k130",
    "X-n685-k75",
    "X-n701-k44",
    "X-n716-k35",
    "X-n733-k159",
    "X-n749-k98",
    "X-n766-k71",
    "X-n783-k48",
    "X-n801-k40",
    "X-n819-k171",
    "X-n837-k142",
    "X-n856-k95",
    "X-n876-k59",
    "X-n895-k37",
    "X-n916-k207",
    "X-n936-k151",
    "X-n957-k87",
    "X-n979-k58",
    "X-n1001-k43",
)


@dataclass(frozen=True)
class CatalogEntry:
    name: str
    customer_count: int
    vehicle_count: int
    # None until resolved: the mirror catalog lists names only, and the BKS
    # is fetched for the instances a run actually selects (resolve_mirror_bks).
    best_known_distance: int | None
    proven_optimal: bool
    instance_url: str


class _CatalogParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.entries: list[CatalogEntry] = []
        self._in_row = False
        self._row_text: list[str] = []
        self._anchors: list[tuple[str, str]] = []
        self._anchor_href: str | None = None
        self._anchor_text: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "tr":
            self._in_row = True
            self._row_text = []
            self._anchors = []
        elif tag == "a" and self._in_row:
            self._anchor_href = dict(attrs).get("href")
            self._anchor_text = []

    def handle_data(self, data: str) -> None:
        if not self._in_row:
            return
        self._row_text.append(data)
        if self._anchor_href is not None:
            self._anchor_text.append(data)

    def handle_endtag(self, tag: str) -> None:
        if tag == "a" and self._anchor_href is not None:
            self._anchors.append(
                (self._anchor_href, " ".join(self._anchor_text).strip())
            )
            self._anchor_href = None
            self._anchor_text = []
        elif tag == "tr" and self._in_row:
            self._finish_row()
            self._in_row = False

    def _finish_row(self) -> None:
        instance = next(
            (
                (href, text)
                for href, text in self._anchors
                if "/download/instance/" in href and text.startswith("X-n")
            ),
            None,
        )
        bks = next(
            (text for href, text in self._anchors if "/download/bks/" in href),
            None,
        )
        if instance is None or bks is None:
            return
        href, name = instance
        name_match = re.fullmatch(r"X-n(\d+)-k(\d+)", name)
        if name_match is None:
            return
        normalized_bks = re.sub(r"[^0-9.]", "", bks)
        bks_value = float(normalized_bks)
        if not bks_value.is_integer():
            raise ValueError(f"Set X BKS is not integral: {name}={bks}")
        row_text = " ".join(self._row_text)
        self.entries.append(
            CatalogEntry(
                name=name,
                customer_count=int(name_match.group(1)) - 1,
                vehicle_count=int(name_match.group(2)),
                best_known_distance=int(bks_value),
                proven_optimal=bool(re.search(r"\byes\b", row_text, re.I)),
                instance_url=urljoin(CATALOG_URL, href),
            )
        )


def parse_x_catalog(text: str) -> tuple[CatalogEntry, ...]:
    parser = _CatalogParser()
    parser.feed(text)
    entries = tuple(
        sorted(parser.entries, key=lambda entry: (entry.customer_count, entry.name))
    )
    if not entries:
        raise ValueError("CVRPLIB catalog contains no Set X instances")
    if len({entry.name for entry in entries}) != len(entries):
        raise ValueError("CVRPLIB catalog contains duplicate Set X instances")
    return entries


def _download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "optimaru-benchmark/1.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def parse_mirror_solution_cost(text: str) -> int:
    """Return the `Cost N` line of a PyVRP/Instances `.sol` file."""
    match = re.search(r"^\s*Cost\s+(\d+)\s*$", text, re.I | re.M)
    if match is None:
        raise ValueError("mirror solution file carries no integral Cost line")
    return int(match.group(1))


def mirror_entry(name: str, solution_text: str | None = None) -> CatalogEntry:
    """A Set X entry from its name; the BKS from its mirror `.sol` when given."""
    name_match = re.fullmatch(r"X-n(\d+)-k(\d+)", name)
    if name_match is None:
        raise ValueError(f"not a Set X instance name: {name}")
    return CatalogEntry(
        name=name,
        customer_count=int(name_match.group(1)) - 1,
        vehicle_count=int(name_match.group(2)),
        best_known_distance=(
            None if solution_text is None else parse_mirror_solution_cost(solution_text)
        ),
        # The mirror does not say which BKS are proven optimal.
        proven_optimal=False,
        instance_url=f"{MIRROR_BASE_URL}{name}.vrp",
    )


def resolve_mirror_bks(
    entry: CatalogEntry, cache_dir: Path, *, refresh: bool
) -> CatalogEntry:
    """Fill in the BKS of a mirror entry from its `.sol`, cached beside the `.vrp`.

    Only the instances a run selects pay this download. With `refresh` the
    `.sol` is fetched again so an improved BKS is picked up; a failed refresh
    falls back to the cached copy when there is one.
    """
    if entry.best_known_distance is not None:
        return entry
    instance_dir = cache_dir / "cvrplib-x"
    instance_dir.mkdir(parents=True, exist_ok=True)
    cached = instance_dir / f"{entry.name}.sol"
    url = f"{MIRROR_BASE_URL}{entry.name}.sol"

    def fetch() -> None:
        # Write beside the target and rename, so an interrupted download never
        # leaves a truncated .sol behind as the cached copy.
        partial = cached.with_suffix(".sol.part")
        partial.write_bytes(_download(url))
        partial.replace(cached)

    downloaded = False
    if refresh or not cached.exists():
        try:
            fetch()
            downloaded = True
        except (OSError, http.client.HTTPException) as exc:
            if not cached.exists():
                raise
            print(
                f"mirror BKS refresh for {entry.name} failed ({exc}); using the cached copy"
            )
    try:
        cost = parse_mirror_solution_cost(cached.read_text())
    except ValueError:
        # A cached copy that does not parse (a proxy error page, an older
        # truncated write) is replaced once; a fresh download that does not
        # parse is a real error.
        if downloaded:
            raise
        print(
            f"cached mirror solution for {entry.name} is unreadable; fetching it again"
        )
        fetch()
        cost = parse_mirror_solution_cost(cached.read_text())
    return replace(entry, best_known_distance=cost)


def _load_mirror_catalog(cache_dir: Path, *, refresh: bool) -> tuple[CatalogEntry, ...]:
    # The hundred names are known; nothing is downloaded until an instance is
    # selected (load_instance_text, resolve_mirror_bks).
    del cache_dir, refresh
    return tuple(
        sorted(
            (mirror_entry(name) for name in SET_X_NAMES),
            key=lambda entry: (entry.customer_count, entry.name),
        )
    )


def load_x_catalog(
    cache_dir: Path, *, refresh: bool = False, source: str = "auto"
) -> tuple[CatalogEntry, ...]:
    if source not in CATALOG_SOURCES:
        raise ValueError(
            f"unknown catalog source {source!r}; expected one of {CATALOG_SOURCES}"
        )
    cache_dir.mkdir(parents=True, exist_ok=True)
    if source == "mirror":
        return _load_mirror_catalog(cache_dir, refresh=refresh)
    cached = cache_dir / "cvrplib-catalog.html"
    if refresh or not cached.exists():
        try:
            cached.write_bytes(_download(CATALOG_URL))
        except (OSError, http.client.HTTPException) as exc:  # URLError is an OSError
            if cached.exists():
                # A refresh is best effort: the official catalog already on
                # disk beats switching sources.
                print(
                    f"CVRPLIB catalog refresh failed ({exc}); using the cached catalog"
                )
            elif source == "cvrplib":
                raise
            else:
                print(
                    f"CVRPLIB catalog unreachable ({exc}); using the PyVRP/Instances mirror"
                )
                return _load_mirror_catalog(cache_dir, refresh=refresh)
    entries = parse_x_catalog(cached.read_text())
    if not entries and source == "auto":
        # A page that downloads but no longer parses (a redesign, a block
        # page) is as good as unreachable.
        print(
            "CVRPLIB catalog page parsed to nothing; using the PyVRP/Instances mirror"
        )
        return _load_mirror_catalog(cache_dir, refresh=refresh)
    return entries


def load_instance_text(entry: CatalogEntry, cache_dir: Path) -> str:
    # One cache entry per instance whatever the catalog source: CVRPLIB and
    # the PyVRP/Instances mirror serve the same files, so a `.vrp` fetched
    # under one source is valid under the other.
    instance_dir = cache_dir / "cvrplib-x"
    instance_dir.mkdir(parents=True, exist_ok=True)
    cached = instance_dir / f"{entry.name}.vrp"
    if not cached.exists():
        cached.write_bytes(_download(entry.instance_url))
    return cached.read_text()
