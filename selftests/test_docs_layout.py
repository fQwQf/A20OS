"""Catalog, translation release gates and cross-format link behavior."""
import json
from pathlib import Path
import tempfile
import unittest

from tools.docs import build_pdf as pdf
from tools.docs import doc_translator as tr
from tools.docs.layout import book, document_id, document_path, navigation


class DocumentationLayoutTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.write('docs/zh/dev/build.md', '# 构建\n\n正文\n')
        self.write('docs/zh/dev/debug.md', '# 调试\n')
        self.write(tr.GLOSSARY, '{}')
        self.write('docs/_meta/navigation.yml', 'nav:\n- section: development\n  children: [dev/build, dev/debug]\n')
        for lang in ('zh', 'en'):
            self.write(f'docs/_meta/languages/{lang}.yml', 'sections: {development: Development}\nbooks: {user: User Manual}\n')
        self.write('docs/_meta/books/user.yml', 'chapters: [dev/build]\n')

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def translate(self):
        source = document_path(self.root, 'dev/build')
        target = self.write('docs/en/dev/build.md', '# Build\n\nBody\n')
        tr.save_cache(self.root, {'dev/build': tr.entry_for(source, target, tr.policy_hash(self.root), 'reviewed')})

    def test_same_document_identity_and_root_readme_exception(self):
        for lang in ('zh', 'en'):
            self.assertEqual(document_id(document_path(self.root, 'dev/build', lang), self.root), 'dev/build')
        self.assertEqual(document_path(self.root, '@repository-readme', 'en'), self.root / 'README.en.md')
        for key in ('../escape', '/absolute', 'dev/../build', 'archive/old'):
            with self.assertRaises(ValueError):
                document_path(self.root, key)

    def test_invalid_catalog_never_falls_back_to_directory_scanning(self):
        self.assertEqual(book(self.root, 'user'), ['dev/build'])
        self.write('docs/_meta/books/user.yml', 'chapters: [dev/build, dev/build]')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            book(self.root, 'user')
        self.write('docs/_meta/books/user.yml', 'chapters: [missing]')
        with self.assertRaisesRegex(ValueError, 'unindexed'):
            book(self.root, 'user')
        self.write('docs/_meta/navigation.yml', 'nav: [missing]')
        with self.assertRaisesRegex(ValueError, 'missing'):
            navigation(self.root)

    def test_english_release_requires_current_protected_translation(self):
        with self.assertRaisesRegex(ValueError, 'dev/build.*missing'):
            pdf.plan(self.root, 'user', 'en', 'strict')
        preview = pdf.plan(self.root, 'user', 'en', 'preview')[0]
        self.assertTrue(preview['fallback'])
        self.assertEqual(preview['path'], 'docs/zh/dev/build.md')
        self.translate()
        formal = pdf.plan(self.root, 'user', 'en', 'strict')[0]
        self.assertFalse(formal['fallback'])
        self.assertEqual(formal['path'], 'docs/en/dev/build.md')
        document_path(self.root, 'dev/build').write_text('# 构建\n\n更新\n')
        with self.assertRaisesRegex(ValueError, 'stale'):
            pdf.plan(self.root, 'user', 'en', 'strict')

    def test_cross_book_links_and_unique_anchor_namespaces(self):
        source = document_path(self.root, 'dev/build')
        ids = {'dev/build': {'': 'build-top', '开始': pdf.anchor('dev/build', '开始')}}
        indexed = {'dev/build', 'dev/debug'}
        self.assertNotEqual(pdf.anchor('dev/build', '开始'), pdf.anchor('dev/debug', '开始'))
        self.assertEqual(pdf.resolve_link('#开始', source, self.root, 'en', ids, indexed), '#' + ids['dev/build']['开始'])
        self.assertEqual(pdf.resolve_link('debug.md#trace', source, self.root, 'en', ids, indexed), pdf.SITE_URL + 'en/dev/debug/#trace')
        self.write('kernel/main.c', '/* source */')
        self.assertEqual(pdf.resolve_link('../../../kernel/main.c#L1', source, self.root, 'en', ids, indexed), pdf.REPO_URL + '/blob/main/kernel/main.c#L1')
        with self.assertRaisesRegex(ValueError, 'Missing book anchor'):
            pdf.resolve_link('#unknown', source, self.root, 'zh', ids, indexed)

    def test_missing_english_link_uses_source_identity(self):
        self.translate()
        source = document_path(self.root, 'dev/build', 'en')
        self.assertEqual(pdf.resolve_link('debug.md', source, self.root, 'en', {}, {'dev/debug'}), pdf.SITE_URL + 'en/dev/debug/')


if __name__ == '__main__':
    unittest.main()
