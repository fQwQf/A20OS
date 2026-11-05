import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from tools.docs.mkdocs_repo_links import on_page_markdown, published_url


class PublishedLinksTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.docs = self.root / "docs"
        self.docs.mkdir()
        self.source = self.docs / "README.md"
        self.repo_url = "https://example.com/project"

    def link(self, url):
        return published_url(url, self.source, self.docs, self.repo_url)

    def add_file(self, path):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.touch()

    def test_source_link_keeps_line_fragment(self):
        self.add_file("kernel/core/main.c")
        self.assertEqual(self.repo_url + "/blob/main/kernel/core/main.c#L10",
                         self.link("../kernel/core/main.c#L10"))

    def test_archived_page_links_to_existing_repository_source(self):
        self.add_file("docs/archive/README.md")
        self.assertEqual(self.repo_url + "/blob/main/docs/archive/README.md",
                         self.link("archive/"))

    def test_document_directory_resolves_to_its_index(self):
        self.add_file("docs/dev/README.md")
        self.assertEqual("dev/README.md#usage", self.link("dev/#usage"))

    def test_missing_document_is_left_for_strict_validation(self):
        self.assertEqual("missing.md", self.link("missing.md"))

    def test_external_urls_and_local_anchors_are_unchanged(self):
        for url in ("#usage", "https://example.org/file.md", "mailto:dev@example.org"):
            self.assertEqual(url, self.link(url))

    def test_inline_links_are_rewritten_but_code_examples_are_not(self):
        self.add_file("tools/example.py")
        page = SimpleNamespace(file=SimpleNamespace(abs_src_path=str(self.source)))
        config = {"docs_dir": str(self.docs), "repo_url": self.repo_url}
        link = "[source](../tools/example.py)"
        code = "```md\n" + link + "\n```"
        result = on_page_markdown(link + "\n" + code, page=page, config=config, files=None)
        self.assertIn("[source](" + self.repo_url + "/blob/main/tools/example.py)", result)
        self.assertIn(code, result)


if __name__ == "__main__":
    unittest.main()
