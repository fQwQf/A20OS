"""Keep repository links usable when only docs/ is published by MkDocs."""

import re
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit, urlunsplit


LINK = re.compile(r"(?P<open>!?\[[^\]\n]*\]\()(?P<url>[^\s)]+)(?P<close>\))")
EXCLUDED = {"archive", "research", "paper"}


def published_url(url, source, docs_dir, repo_url):
    parsed = urlsplit(url)
    if parsed.scheme or parsed.netloc or not parsed.path or parsed.path.startswith("/"):
        return url
    target = (source.parent / unquote(parsed.path)).resolve()
    root = docs_dir.parent
    if not target.is_relative_to(root):
        raise ValueError(f"repository link escapes checkout: {url}")
    if not target.exists():
        # Let MkDocs name missing targets in its ordinary strict diagnostics.
        return url
    in_docs = target.is_relative_to(docs_dir)
    if in_docs and target.is_dir() and (target / "README.md").is_file():
        target = target / "README.md"
    if in_docs:
        relative = target.relative_to(docs_dir)
        if relative.parts[0] not in EXCLUDED and target.is_file():
            # Resolve directory shorthand while retaining local anchor checks.
            if parsed.path.endswith("/") or Path(unquote(parsed.path)).name != target.name:
                return urlunsplit(("", "", parsed.path.rstrip("/") + "/README.md",
                                   parsed.query, parsed.fragment))
            return url
    kind = "tree" if target.is_dir() else "blob"
    path = quote(target.relative_to(root).as_posix(), safe="/")
    return urlunsplit((*urlsplit(f"{repo_url.rstrip('/')}/{kind}/main/{path}")[:3],
                       parsed.query, parsed.fragment))


def on_page_markdown(markdown, *, page, config, files):
    docs_dir = Path(config["docs_dir"]).resolve()
    source = Path(page.file.abs_src_path)

    def replace(match):
        url = published_url(match["url"], source, docs_dir, config["repo_url"])
        return match["open"] + url + match["close"]

    # Code examples may contain Markdown links; never rewrite their contents.
    return "".join(part if index % 2 else LINK.sub(replace, part)
                   for index, part in enumerate(re.split(r"(^```[^\n]*\n.*?^```[^\n]*$)",
                                                         markdown, flags=re.M | re.S)))
