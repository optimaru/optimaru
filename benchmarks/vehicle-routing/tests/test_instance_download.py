import hashlib
import io

import pytest

from vrp_benchmark import instance


class _Response(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def test_unknown_augerat_instance_lists_the_set(tmp_path) -> None:
    with pytest.raises(ValueError, match="available: A-n32-k5"):
        instance.load_augerat_instance("A-n44-k7", tmp_path)


def test_checksum_mismatch_is_not_cached(tmp_path, monkeypatch) -> None:
    monkeypatch.setattr(instance.urllib.request, "urlopen", lambda *a, **k: _Response(b"<html>busy</html>"))

    with pytest.raises(ValueError, match="checksum mismatch"):
        instance.load_augerat_instance("A-n32-k5", tmp_path)
    assert not (tmp_path / "cvrplib-a" / "A-n32-k5.vrp").exists()


def test_bad_cached_copy_is_fetched_again(tmp_path, monkeypatch) -> None:
    text = (
        "NAME : A-n32-k5\nCOMMENT : (Optimal value: 10)\nTYPE : CVRP\nDIMENSION : 2\n"
        "EDGE_WEIGHT_TYPE : EUC_2D\nCAPACITY : 5\nNODE_COORD_SECTION\n1 0 0\n2 3 4\n"
        "DEMAND_SECTION\n1 0\n2 1\nDEPOT_SECTION\n1\n-1\nEOF\n"
    ).encode()
    catalog_id, _ = instance.AUGERAT_A["A-n32-k5"]
    monkeypatch.setitem(instance.AUGERAT_A, "A-n32-k5", (catalog_id, hashlib.sha256(text).hexdigest()))
    cached = tmp_path / "cvrplib-a" / "A-n32-k5.vrp"
    cached.parent.mkdir(parents=True)
    cached.write_bytes(b"stale error page")
    monkeypatch.setattr(instance.urllib.request, "urlopen", lambda *a, **k: _Response(text))

    loaded = instance.load_augerat_instance("A-n32-k5", tmp_path)

    assert loaded.customer_count == 1
    assert cached.read_bytes() == text
