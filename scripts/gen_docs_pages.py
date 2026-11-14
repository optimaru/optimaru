"""Generate the API reference pages and the navigation of the documentation site.

Run by mkdocs-gen-files on every `mkdocs build` / `mkdocs serve` (see mkdocs.yml),
so the site follows the packages under optimaru/ without hand-kept lists:

- reference/<use case>.md: mkdocstrings sections for optimaru.<use case>
  (the package, `models`, `constraints` and the `_core` extension stubs);
- reference/constraint-base.md: the shared constraint-spec base;
- reference/api.md: the setting that moves a solve to the optimaru API;
- SUMMARY.md: the site navigation (read by mkdocs-literate-nav).
"""

from __future__ import annotations

from pathlib import Path

import mkdocs_gen_files

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "optimaru"
DOCS = ROOT / "docs"


def use_cases() -> list[str]:
    return sorted(p.parent.name for p in PACKAGE.glob("*/__init__.py") if (p.parent / "_core.pyi").exists())


def title(slug: str) -> str:
    return slug.replace("-", " ").replace("_", " ").capitalize()


def reference_page(use_case: str) -> str:
    package = PACKAGE / use_case
    lines = [
        f"# `optimaru.{use_case}`",
        "",
        f"Generated from the sources of `optimaru/{use_case}/` and the stubs of its compiled",
        "extension. The guide explains the model and the constraints in prose.",
        "",
        f"::: optimaru.{use_case}",
        "    options:",
        "      members: false",
        "",
    ]
    for module in ("models", "constraints"):
        if (package / f"{module}.py").exists():
            lines += [f"::: optimaru.{use_case}.{module}", ""]
    lines += [f"::: optimaru.{use_case}._core", ""]
    return "\n".join(lines)


nav = mkdocs_gen_files.Nav()
nav["Home"] = "index.md"
nav["Extending a use case"] = "extending.md"
nav["Use cases", "Overview and constraint contract"] = "use-cases/README.md"
for use_case in use_cases():
    slug = use_case.replace("_", "-")
    guide = DOCS / "use-cases" / slug
    section = ("Use cases", title(slug))
    nav[(*section, "Guide")] = f"use-cases/{slug}/README.md"
    for name, label in (
        ("python.md", "Python"),
        ("cpp.md", "C++"),
        ("API_SPEC.md", "API spec"),
        ("extending.md", "Adding a constraint"),
    ):
        if (guide / name).exists():
            nav[(*section, label)] = f"use-cases/{slug}/{name}"
    path = f"reference/{slug}.md"
    with mkdocs_gen_files.open(path, "w") as page:
        page.write(reference_page(use_case))
    mkdocs_gen_files.set_edit_path(path, f"../optimaru/{use_case}/__init__.py")
    nav["API reference", f"optimaru.{use_case}"] = path

with mkdocs_gen_files.open("reference/constraint-base.md", "w") as page:
    page.write("# Constraint-spec base\n\n::: optimaru._constraint\n")
nav["API reference", "optimaru._constraint"] = "reference/constraint-base.md"
with mkdocs_gen_files.open("reference/api.md", "w") as page:
    # The setting and its error, as optimaru exports them; the decorators are for use cases, not users.
    page.write(
        "# Solving on the optimaru API\n\n::: optimaru._api\n    options:\n      members:\n"
        + "".join(
            f"        - {name}\n"
            for name in ("use_api", "use_local", "ApiError", "API_KEY_VARIABLE", "API_URL_VARIABLE", "DEFAULT_API_URL")
        )
    )
nav["API reference", "optimaru.use_api"] = "reference/api.md"
nav["Changelog"] = "changelog.md"

with mkdocs_gen_files.open("SUMMARY.md", "w") as summary:
    summary.writelines(nav.build_literate_nav())
