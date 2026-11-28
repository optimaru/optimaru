"""Build the documentation site once per release, with a version selector.

The site (mkdocs.yml) is built for the working tree, as ``dev``, and for the
newest release tag of every minor version since the site began (0.4), each in a
folder of its own:

    <out>/versions.json    the list the theme's version selector reads
    <out>/dev/             this checkout (optimaru main)
    <out>/0.9/             tag v0.9.0, or the newest 0.9.x patch
    <out>/0.8/ ...
    <out>/latest/          a copy of the newest release's folder
    <out>/index.html ...   a redirect page for every page of latest/, so the
                           addresses of the unversioned site keep working

Run it with the docs requirements, from a clone that has the release tags
(``git fetch --tags``; a shallow clone has none)::

    uv run --no-project --with-requirements docs/requirements.txt \\
        python scripts/build_versioned_docs.py --out site \\
        --site-url https://optimaru.github.io/optimaru/

It needs Python 3.12 or later (``tarfile``'s extraction filter). Every version is built with the MkDocs of this environment: the requirements
have been the same pins since 0.4. Release builds are not strict (their
warnings cannot be fixed any more); ``--strict`` applies to ``dev``. A release
is built from ``git archive`` of its tag, where two things are adjusted: its
links to repository files name the tag instead of ``main``, and the theme gets
the version banner (overrides/main.html) when the tag predates it. An older
release that fails to build is left out of the menu with a warning; dev or the
newest release failing fails the run.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from io import BytesIO
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The first release with mkdocs.yml. Each minor adds about 20 MB to the site, and latest/ repeats
# the newest (GitHub Pages allows 1 GB): raise this when the old ones are no longer worth keeping.
FIRST_MINOR = (0, 4)
TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")
WRAPPER = ".mkdocs-versioned.yml"
LATEST = "latest"
DEV = "dev"


def release_tags() -> list[tuple[str, str]]:
    """(folder, tag) for the newest patch of every minor of this repository, newest minor first."""
    out = subprocess.run(["git", "tag", "--list", "v*"], cwd=ROOT, check=True, capture_output=True, text=True)
    return pick_releases(out.stdout.split())


def pick_releases(tags: list[str]) -> list[tuple[str, str]]:
    """(folder, tag) for the newest patch of every minor since FIRST_MINOR, newest minor first."""
    newest: dict[tuple[int, int], tuple[int, str]] = {}
    for tag in tags:
        m = TAG.match(tag)
        if not m:
            continue
        major, minor, patch = map(int, m.groups())
        if (major, minor) < FIRST_MINOR:
            continue
        if (major, minor) not in newest or patch > newest[(major, minor)][0]:
            newest[(major, minor)] = (patch, tag)
    return [(f"{major}.{minor}", tag) for (major, minor), (_, tag) in sorted(newest.items(), reverse=True)]


def export_tag(tag: str, dest: Path) -> None:
    """The tree of a tag, without a checkout."""
    archive = subprocess.run(["git", "archive", "--format=tar", tag], cwd=ROOT, check=True, capture_output=True)
    with tarfile.open(fileobj=BytesIO(archive.stdout)) as tar:
        tar.extractall(dest, filter="data")


def prepare_release_tree(tree: Path, ref: str) -> None:
    """Point a release's links to repository files at its tag, and give it the version banner."""
    hooks = tree / "scripts" / "mkdocs_hooks.py"
    if hooks.is_file():
        text = hooks.read_text()
        if "/{kind}/main/" not in text:
            print(f"warning: {ref}: repository links not pinned to the tag", file=sys.stderr)
        hooks.write_text(text.replace("/{kind}/main/", f"/{{kind}}/{ref}/"))
    banner = ROOT / "overrides" / "main.html"
    target = tree / "overrides" / "main.html"
    if banner.is_file() and not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(banner, target)


def build(tree: Path, out: Path, site_url: str, ref: str, strict: bool) -> None:
    """Build the site of ``tree`` into ``out``, with the version selector turned on."""
    wrapper = tree / WRAPPER
    # INHERIT merges this into the tree's own mkdocs.yml; relative paths stay the tree's.
    wrapper.write_text(
        json.dumps(
            {
                "INHERIT": "mkdocs.yml",
                "site_url": site_url,
                # 0.4 and 0.5 had no custom_dir: without it they would not read the banner template.
                "theme": {"custom_dir": "overrides"},
                "extra": {"version": {"provider": "mike", "default": LATEST, "alias": True}},
            }
        )
    )
    env = {**os.environ, "DISABLE_MKDOCS_2_WARNING": "true", "NO_MKDOCS_2_WARNING": "1"}
    env.pop("OPTIMARU_DOCS_SITE_URL", None)  # the wrapper sets site_url
    command = [sys.executable, "-m", "mkdocs", "build", "--quiet", "-f", str(wrapper), "-d", str(out)]
    if strict:
        command.append("--strict")
    try:
        subprocess.run(command, cwd=tree, env=env, check=True)
    finally:
        wrapper.unlink()


REDIRECT = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Redirecting</title>
<link rel="canonical" href="{url}">
<meta http-equiv="refresh" content="0; url={href}">
<script>location.replace({href_js} + location.search + location.hash)</script>
</head>
<body><a href="{href}">{url}</a></body>
</html>
"""


def write_redirects(out: Path, site_url: str, newest: str, versions: set[str]) -> int:
    """A page at every address of latest/ without the version, sending it to latest/.

    Its canonical link names the newest release's own folder, as the pages of latest/ do. ``versions``
    are the folders next to latest/ (dev and the releases), which no page may shadow.
    """
    latest = out / LATEST
    count = 0
    for page in latest.rglob("index.html"):
        rel = page.parent.relative_to(latest).as_posix()
        rel = "" if rel == "." else f"{rel}/"
        top = rel.split("/")[0]
        if top in versions | {LATEST}:
            raise SystemExit(f"the page {rel} would shadow the version folder {top}/")
        depth = rel.count("/")
        href = "../" * depth + f"{LATEST}/{rel}"
        target = out / rel / "index.html"
        target.parent.mkdir(parents=True, exist_ok=True)
        url = f"{site_url}{newest}/{rel}"
        target.write_text(REDIRECT.format(url=url, href=href, href_js=json.dumps(href)))
        count += 1
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, required=True, help="output directory (emptied first)")
    parser.add_argument("--site-url", required=True, help="where <out> is served, ending in /")
    parser.add_argument("--strict", action="store_true", help="build dev with mkdocs --strict")
    parser.add_argument("--jobs", type=int, default=4, help="builds at a time")
    parser.add_argument("--no-releases", action="store_true", help="build dev only (quick local check)")
    args = parser.parse_args()
    jobs_at_once = max(1, min(args.jobs, os.cpu_count() or 1))
    site_url = args.site_url if args.site_url.endswith("/") else f"{args.site_url}/"
    out = args.out.resolve()

    releases = [] if args.no_releases else release_tags()
    if not releases and not args.no_releases:
        sys.exit("no release tags found: fetch them first (git fetch --tags)")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    def build_release(folder: str, tag: str, tmp: str) -> None:
        tree = Path(tmp) / folder
        export_tag(tag, tree)
        prepare_release_tree(tree, tag)
        build(tree, out / folder, f"{site_url}{folder}/", tag, strict=False)

    # Each build is one MkDocs process; they share nothing but the output directory.
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(max_workers=jobs_at_once) as pool:
        # Keyed by folder: dev, or a release minor.
        jobs = {pool.submit(build, ROOT, out / DEV, f"{site_url}{DEV}/", "main", args.strict): DEV}
        jobs.update({pool.submit(build_release, folder, tag, tmp): folder for folder, tag in releases})
        failed: set[str] = set()
        for job in as_completed(jobs):
            try:
                job.result()
                print(f"built {jobs[job]}", flush=True)
            except Exception as error:  # a failed build, or a tag tree that would not extract
                print(f"error: {jobs[job]}: {error}", file=sys.stderr)
                failed.add(jobs[job])
    if DEV in failed:
        sys.exit("build failed: dev (main)")
    if releases and releases[0][0] in failed:
        sys.exit(f"build failed: the newest release, {releases[0][1]}")
    # An older release that no longer builds (say, after a change to docs/requirements.txt) is left
    # out of the menu rather than holding back the rest; the warning shows in the log.
    for folder, tag in releases:
        if folder in failed:
            print(f"warning: {tag} did not build and is left out of the version menu", file=sys.stderr)
            shutil.rmtree(out / folder, ignore_errors=True)
    releases = [(folder, tag) for folder, tag in releases if folder not in failed]
    versions = [{"version": DEV, "title": "dev (main)", "aliases": []}]
    versions += [
        {"version": folder, "title": tag.removeprefix("v"), "aliases": [LATEST] if i == 0 else []}
        for i, (folder, tag) in enumerate(releases)
    ]
    if releases:
        # The newest release again under latest/ (its canonical links keep naming its own folder).
        shutil.copytree(out / releases[0][0], out / LATEST)
        folders = {DEV, *(folder for folder, _ in releases)}
        print(f"{write_redirects(out, site_url, releases[0][0], folders)} redirect pages to {LATEST}/", flush=True)
        # GitHub Pages answers every unknown address with the root 404.html (its links are absolute).
        shutil.copyfile(out / LATEST / "404.html", out / "404.html")
    else:
        (out / "index.html").write_text(
            REDIRECT.format(url=f"{site_url}{DEV}/", href=f"{DEV}/", href_js=json.dumps(f"{DEV}/"))
        )
    (out / "versions.json").write_text(json.dumps(versions, indent=2) + "\n")
    print(f"versions: {', '.join(v['version'] for v in versions)}")


if __name__ == "__main__":
    main()
