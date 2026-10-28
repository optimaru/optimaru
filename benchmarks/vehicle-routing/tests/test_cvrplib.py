import pytest

from vrp_benchmark.cvrplib import parse_x_catalog

CATALOG = """\
<table>
  <tr>
    <td><a href="/cvrplib/index.php/en/download/instance/158">X-n101-k25</a></td>
    <td class="text-end">$100$</td>
    <td class="text-end">$25$</td>
    <td class="text-end">$206$</td>
    <td><a href="/cvrplib/index.php/en/download/bks/158"><span>$27{,}591.00$</span></a></td>
    <td><span>yes</span></td>
  </tr>
</table>
"""


def test_parse_official_set_x_catalog_row() -> None:
    entry = parse_x_catalog(CATALOG)[0]
    assert entry.name == "X-n101-k25"
    assert entry.customer_count == 100
    assert entry.vehicle_count == 25
    assert entry.best_known_distance == 27_591
    assert entry.proven_optimal
    assert entry.instance_url.endswith("/download/instance/158")


def test_mirror_solution_cost_and_entry() -> None:
    from vrp_benchmark.cvrplib import (
        MIRROR_BASE_URL,
        SET_X_NAMES,
        mirror_entry,
        parse_mirror_solution_cost,
    )

    solution = "Route #1: 5 2 7\nRoute #2: 1 3\nCost 27591\n"
    assert parse_mirror_solution_cost(solution) == 27_591
    entry = mirror_entry("X-n101-k25", solution)
    assert entry.customer_count == 100
    assert entry.vehicle_count == 25
    assert entry.best_known_distance == 27_591
    assert not entry.proven_optimal
    assert entry.instance_url == f"{MIRROR_BASE_URL}X-n101-k25.vrp"
    # Uchoa et al.'s Set X: 100 instances, X-n101-k25 through X-n1001-k43.
    assert len(SET_X_NAMES) == 100
    assert SET_X_NAMES[0] == "X-n101-k25" and SET_X_NAMES[-1] == "X-n1001-k43"
    assert len(set(SET_X_NAMES)) == 100


def test_unreadable_cached_mirror_solution_is_fetched_again(
    tmp_path, monkeypatch
) -> None:
    from vrp_benchmark import cvrplib

    entry = cvrplib.mirror_entry("X-n101-k25")
    cached = tmp_path / "cvrplib-x" / "X-n101-k25.sol"
    cached.parent.mkdir(parents=True)
    cached.write_text(
        "<html>502 Bad Gateway</html>"
    )  # a proxy error page, cached earlier

    downloads: list[str] = []

    def fake_download(url: str) -> bytes:
        downloads.append(url)
        return b"Route #1: 1 2\nCost 27591\n"

    monkeypatch.setattr(cvrplib, "_download", fake_download)
    resolved = cvrplib.resolve_mirror_bks(entry, tmp_path, refresh=False)
    assert resolved.best_known_distance == 27_591
    assert downloads == [f"{cvrplib.MIRROR_BASE_URL}X-n101-k25.sol"]
    assert cached.read_text().endswith("Cost 27591\n")
    assert not list(cached.parent.glob("*.part"))

    # A fresh download that does not parse is a real error, not a retry loop.
    cached.unlink()
    monkeypatch.setattr(cvrplib, "_download", lambda url: b"<html>502</html>")
    with pytest.raises(ValueError):
        cvrplib.resolve_mirror_bks(entry, tmp_path, refresh=False)
