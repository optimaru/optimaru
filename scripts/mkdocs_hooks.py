"""MkDocs hook: make the guides' GitHub-style links work on the site.

The guides are written to be browsed on GitHub, so on the site two kinds of
relative link need rewriting:

- links to repository files outside docs/ (``../../optimaru/knapsack/cpp/...``,
  ``../../benchmarks/...``) point at the same file on GitHub (`main`);
- links to a directory under docs/ (``knapsack``), which GitHub shows through
  its README.md, point at that README.md (the page MkDocs builds for it).
"""

from __future__ import annotations

import posixpath
import re
from pathlib import Path

GITHUB = "https://github.com/optimaru/optimaru"
ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r"(\]\()([^)\s]+)(\))")


def on_page_markdown(markdown, page, config, files):
    src = page.file.src_uri  # path inside docs/
    if page.file.generated_by:  # generated pages carry no repository links
        return markdown
    doc_dir = posixpath.dirname(f"docs/{src}")

    def rewrite(match):
        target = match.group(2)
        if re.match(r"^[a-z]+:|^#|^/", target):
            return match.group(0)
        path, sep, fragment = target.partition("#")
        repo_path = posixpath.normpath(posixpath.join(doc_dir, path))
        if repo_path.startswith("docs/") and repo_path != "docs/pypi.md":
            if (ROOT / repo_path).is_dir() and (ROOT / repo_path / "README.md").is_file():
                readme = posixpath.join(path.rstrip("/"), "README.md")
                return f"{match.group(1)}{readme}{sep}{fragment}{match.group(3)}"
            return match.group(0)  # a page of the site
        kind = "tree" if (ROOT / repo_path).is_dir() else "blob"
        url = f"{GITHUB}/{kind}/main/{repo_path}" + (f"#{fragment}" if sep else "")
        return f"{match.group(1)}{url}{match.group(3)}"

    return LINK.sub(rewrite, markdown)
