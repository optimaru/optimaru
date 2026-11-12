import json
import multiprocessing

from cpsat_plugins.results import write_results, write_whole


def test_a_results_file_is_rewritten_whole(tmp_path) -> None:
    path = tmp_path / "runs" / "sweep.json"

    write_results(path, [{"instance": "a"}])
    write_results(path, [{"instance": "a"}, {"instance": "b"}])

    assert [result["instance"] for result in json.loads(path.read_text())] == ["a", "b"]
    # Written beside the file and moved over it: nothing is left behind.
    assert [item.name for item in path.parent.iterdir()] == ["sweep.json"]


def _write_many(path, payload: bytes) -> None:
    for _ in range(200):
        write_whole(path, payload)


def test_several_processes_can_write_the_same_file_at_once(tmp_path) -> None:
    # Lanes that start on an empty download cache all fetch and write the same archive.
    path = tmp_path / "cache" / "archive.zip"
    context = multiprocessing.get_context("spawn")
    writers = [context.Process(target=_write_many, args=(path, b"same bytes")) for _ in range(4)]
    for writer in writers:
        writer.start()
    for writer in writers:
        writer.join(60)

    assert [writer.exitcode for writer in writers] == [0, 0, 0, 0]
    assert path.read_bytes() == b"same bytes"
    assert [item.name for item in path.parent.iterdir()] == ["archive.zip"]


def test_the_seeds_of_a_generated_run_are_distinct_whole_numbers() -> None:
    import argparse

    import pytest

    from cpsat_plugins.results import seed_list

    assert seed_list("0,4,1") == [0, 4, 1]
    for text in ("0,x", "0,,1", "1,1"):
        with pytest.raises(argparse.ArgumentTypeError):
            seed_list(text)
