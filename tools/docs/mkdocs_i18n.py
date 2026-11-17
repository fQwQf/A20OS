"""English navigation, explicit fallback notices and stable source heading IDs."""

from html.parser import HTMLParser
from pathlib import Path
import re
import sys

import markdown as md
from markdown.extensions import Extension
from markdown.treeprocessors import Treeprocessor
from mkdocs.plugins import event_priority

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.docs.doc_translator import ROOT, is_current, load_cache, policy_hash
from tools.docs.layout import document_id, document_path, navigation

_heading_ids = None
_cache = {}
_policy = ""


class HeadingCollector(HTMLParser):
    def __init__(self):
        super().__init__()
        self.headings = []

    def handle_starttag(self, tag, attrs):
        if re.fullmatch(r"h[1-6]", tag):
            self.headings.append((tag, dict(attrs).get("id")))


class SourceHeadingIds(Treeprocessor):
    def run(self, root):
        if _heading_ids is None:
            return
        headings = [node for node in root.iter() if re.fullmatch(r"h[1-6]", node.tag)]
        if [node.tag for node in headings] != [tag for tag, _ in _heading_ids]:
            raise ValueError("Translation heading structure differs from its source")
        for node, (_, anchor) in zip(headings, _heading_ids):
            if anchor:
                node.set("id", anchor)


class PreserveSourceIds(Extension):
    def extendMarkdown(self, markdown):
        # Inline markup and attribute lists have been processed; toc runs next (5).
        markdown.treeprocessors.register(SourceHeadingIds(markdown), "source-heading-ids", 6)


def on_config(config):
    language = config.plugins["i18n"].current_language or "zh"
    config["nav"] = navigation(ROOT, language)[0]
    if not any(isinstance(ext, PreserveSourceIds) for ext in config["markdown_extensions"]):
        config["markdown_extensions"].append(PreserveSourceIds())
    return config


@event_priority(100)
def on_files(files, *, config):
    # The repository language chooser is not a second site index. Catalog YAML
    # is build input, not a downloadable website asset (exclude_docs is too late
    # for folder-localization conflict detection).
    for file in list(files):
        if file.src_uri == 'README.md' or file.src_uri.startswith('_meta/'):
            files.remove(file)
    return files


def on_pre_build(config):
    global _cache, _policy, _heading_ids
    _cache, _policy, _heading_ids = load_cache(ROOT), policy_hash(ROOT), None


def source_for(path):
    return document_path(ROOT, document_id(path, ROOT), "zh")


def current(path):
    source = source_for(path)
    return source.is_file() and is_current(
        source, path, _cache.get(document_id(source, ROOT), {}), _policy)


@event_priority(-110)
def on_nav(nav, *, config, files):
    if config.plugins["i18n"].current_language != "en":
        return nav
    for page in nav.pages:
        path = Path(page.file.abs_src_path)
        if path.is_relative_to(ROOT / "docs/en") and current(path):
            match = re.search(r"^#\s+(.+)$", path.read_text(), re.M)
            if match:
                page.title = re.sub(r"\s*\{[^}]*\}\s*$", "", match[1])
        else:
            # Source filenames are stable English identifiers; do not present
            # untranslated navigation labels as if they were English prose.
            rel = Path(page.file.src_uri)
            name = rel.parent.name if rel.stem == "README" else rel.stem
            name = name.removesuffix(".en").replace("-", " ").replace("_", " ")
            page.title = name.capitalize() + " (Chinese)"
    return nav


@event_priority(60)
def on_page_markdown(markdown, *, page, config, files):
    global _heading_ids
    _heading_ids = None
    if config.plugins["i18n"].current_language != "en":
        return markdown
    path = Path(page.file.abs_src_path)
    if not path.is_relative_to(ROOT / "docs/en"):
        return '> **Translation pending.** This page is currently available in Chinese.\n\n' + markdown
    source = source_for(path)
    if not current(path):
        return ('> **Translation update pending.** The Chinese source has changed; '
                'the current Chinese version is shown below.\n\n' + source.read_text())
    parser = HeadingCollector()
    # Use the same renderer as the site, including Unicode slugs and explicit IDs.
    renderer = md.Markdown(extensions=[ext for ext in config["markdown_extensions"]
                                      if not isinstance(ext, PreserveSourceIds)],
                           extension_configs=config["mdx_configs"])
    parser.feed(renderer.convert(source.read_text()))
    _heading_ids = parser.headings
    return markdown
