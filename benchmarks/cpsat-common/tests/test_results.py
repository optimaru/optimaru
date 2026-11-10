import json

from cpsat_plugins.results import write_results


def test_a_results_file_is_rewritten_whole(tmp_path) -> None:
    path = tmp_path / "runs" / "sweep.json"

    write_results(path, [{"instance": "a"}])
    write_results(path, [{"instance": "a"}, {"instance": "b"}])

    assert [result["instance"] for result in json.loads(path.read_text())] == ["a", "b"]
    # Written beside the file and moved over it: nothing is left behind.
    assert [item.name for item in path.parent.iterdir()] == ["sweep.json"]
