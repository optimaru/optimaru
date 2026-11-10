"""MkDocs hook: point links to repository files outside docs/ at GitHub.

The guides link to source files (``../../optimaru/knapsack/cpp/...``,
``../../benchmarks/...``) so that they work when browsed on GitHub. The site
only contains docs/, so on the site those links are rewritten to the same file
on GitHub (`main`).
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
            return match.group(0)  # a page of the site
        kind = "tree" if (ROOT / repo_path).is_dir() else "blob"
        url = f"{GITHUB}/{kind}/main/{repo_path}" + (f"#{fragment}" if sep else "")
        return f"{match.group(1)}{url}{match.group(3)}"

    return LINK.sub(rewrite, markdown)
