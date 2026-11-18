"""Offline behavioral tests for translation safety and incremental maintenance."""

import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from tools.docs import doc_translator as tr


def options(**kwargs):
    return argparse.Namespace(**dict(dict(dry_run=False, check=False, check_current=False,
                                         adopt=False, prune=False, files=None, limit=0, force=False), **kwargs))


class FakeClient:
    model = "test"

    def __init__(self):
        self.calls = 0

    def translate(self, text, glossary):
        self.calls += 1
        return text.replace("标题", "Title").replace("正文", "Body").replace("链接", "Link")


class TranslationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "docs/zh").mkdir(parents=True)
        glossary = self.root / tr.GLOSSARY
        glossary.parent.mkdir(parents=True)
        glossary.write_text('{}\n')
        self.source = self.root / "docs/zh/guide.md"
        self.source.write_text('# 标题\n\n正文 42 `foo()` [链接](other.md#标题)\n\n```sh\necho 1\n```\n')
        self.client = FakeClient()

    def test_incremental_and_policy_change(self):
        tr.run(options(), self.root, self.client)
        calls = self.client.calls
        tr.run(options(), self.root, self.client)
        self.assertEqual(calls, self.client.calls)
        self.source.write_text(self.source.read_text() + '\n正文\n')
        tr.run(options(), self.root, self.client)
        self.assertGreater(self.client.calls, calls)
        calls = self.client.calls
        (self.root / tr.GLOSSARY).write_text('{"标题":"Title"}')
        tr.run(options(), self.root, self.client)
        self.assertGreater(self.client.calls, calls)

    def test_private_config_and_environment_isolation(self):
        with patch.object(tr, 'ROOT', self.root / 'checkout'):
            config_dir = self.root / 'private'
            config_file = config_dir / 'a20os/doc-translation.json'
            config_file.parent.mkdir(parents=True)
            data = {'DOC_TRANSLATION_BASE_URL': 'https://example.com/v1',
                    'DOC_TRANSLATION_MODEL': 'test', 'DOC_TRANSLATION_API_KEY': 'test-only-key'}
            config_file.write_text(json.dumps(data))
            config_file.chmod(0o600)
            env = {'XDG_CONFIG_HOME': str(config_dir)}
            self.assertEqual(tr.provider_config(env), data)
            self.assertEqual(tr.provider_config(data), data)
            with self.assertRaisesRegex(ValueError, 'all three'):
                tr.provider_config(dict(env, DOC_TRANSLATION_BASE_URL='https://other.example/v1'))
            config_file.chmod(0o644)
            with self.assertRaisesRegex(ValueError, '0600'):
                tr.provider_config(env)
            config_file.chmod(0o600)
            with patch.object(tr, 'ROOT', self.root):
                with self.assertRaisesRegex(ValueError, 'outside the checkout'):
                    tr.provider_config(env)

    def test_deleted_source_cleanup_and_edited_orphan(self):
        tr.run(options(), self.root, self.client)
        self.source.unlink()
        target = tr.target_for(self.source)
        original = target.read_text()
        target.write_text(original + '\nHuman edit\n')
        with self.assertRaisesRegex(ValueError, 'edited orphan'):
            tr.run(options(), self.root, self.client)
        target.write_text(original)
        tr.run(options(prune=True), self.root)
        self.assertFalse(target.exists())
        self.assertEqual(tr.load_cache(self.root), {})

    def test_missing_output_regenerated_and_manual_edit_preserved(self):
        tr.run(options(), self.root, self.client)
        target = tr.target_for(self.source)
        target.unlink()
        tr.run(options(), self.root, self.client)
        self.assertTrue(target.exists())
        target.write_text(target.read_text() + '\nHuman edit\n')
        with self.assertRaisesRegex(ValueError, 'unregistered edit'):
            tr.run(options(), self.root, self.client)
        tr.run(options(adopt=True, files=['guide']), self.root)
        tr.run(options(check=True), self.root)

    def test_bad_model_output_does_not_replace_previous_translation(self):
        tr.run(options(), self.root, self.client)
        target = tr.target_for(self.source)
        old_text, old_cache = target.read_text(), (self.root / tr.CACHE).read_text()
        self.source.write_text(self.source.read_text() + '\n正文\n')
        with patch.object(self.client, 'translate', return_value='Incomplete translation'):
            with self.assertRaises(ValueError):
                tr.run(options(), self.root, self.client)
        self.assertEqual(target.read_text(), old_text)
        self.assertEqual((self.root / tr.CACHE).read_text(), old_cache)

    def test_stale_allowed_for_check_but_not_release(self):
        tr.run(options(), self.root, self.client)
        self.source.write_text(self.source.read_text() + '\n正文\n')
        tr.run(options(check=True), self.root)
        with self.assertRaisesRegex(ValueError, 'incomplete or stale'):
            tr.run(options(check_current=True), self.root)

    def test_exclusions_selection_and_dry_run(self):
        (self.root / 'docs/zh/archive').mkdir()
        (self.root / 'docs/zh/archive/old.md').write_text('# archived')
        tr.run(options(dry_run=True), self.root)
        self.assertFalse((self.root / tr.CACHE).exists())
        self.assertEqual(tr.sources(self.root), [self.source])
        with self.assertRaisesRegex(ValueError, 'Not eligible'):
            tr.run(options(files=['docs/zh/archive/old.md']), self.root)

    def test_reject_cache_path_escape(self):
        (self.root / tr.CACHE).write_text(json.dumps({"version": 2, "files": {"../outside.md": {}}}))
        with self.assertRaisesRegex(ValueError, 'Invalid document ID'):
            tr.load_cache(self.root)

    def test_protection_roundtrip_and_corruption(self):
        source = ('# 标题 {#stable}\n\n~~~text\n# Not a heading\n```\n~~~\n\n'
                  '正文 ``a ` b`` [链接](file(a).md#标题) ![图](img.svg) 123\n\n'
                  '[ref]: https://example.com\n\n[链接][ref]\n\n'
                  '<!-- i18n:keep-start -->\n`private` 456\n```sh\necho keep\n```\n<!-- i18n:keep-end -->\n')
        text, values = tr.protect(source)
        self.assertEqual(tr.restore(text, text, values), source)
        tokens = tr.TOKEN.findall(text)
        for bad in [text.replace(tokens[0], ''), text + tokens[0], text.replace(tokens[0], 'oops')]:
            with self.assertRaises(ValueError):
                tr.restore(bad, text, values)

    def test_request_pacing_and_retry_headers(self):
        with patch.dict(tr.os.environ, {"DOC_TRANSLATION_MIN_INTERVAL": "6"}):
            client = tr.Client('https://example.com/v1', 'test', 'secret')
        with patch.object(tr.time, 'monotonic', side_effect=[100, 100, 102, 106]):
            with patch.object(tr.time, 'sleep') as sleep:
                client.pace()
                client.pace()
                sleep.assert_called_once_with(4)
        with patch.object(tr.random, 'uniform', return_value=0):
            self.assertEqual(client.retry_delay({'Retry-After': '45'}, 0), 45)
            self.assertEqual(client.retry_delay({'Retry-After': 'bad'}, 1), 20)
            with patch.object(tr.time, 'time', return_value=0):
                self.assertEqual(client.retry_delay({'Retry-After': 'Thu, 01 Jan 1970 00:01:00 GMT'}, 0), 60)
        for value in ['0', '-1', 'nan', 'inf']:
            with patch.dict(tr.os.environ, {"DOC_TRANSLATION_MIN_INTERVAL": value}):
                with self.assertRaises(ValueError):
                    tr.Client('https://example.com/v1', 'test', 'secret')

    def test_payment_error_is_not_retried(self):
        client = tr.Client('https://example.com/v1', 'test', 'secret')
        with patch.object(tr.request, 'urlopen', side_effect=HTTPError(client.url, 402, 'payment', {}, None)) as call:
            with self.assertRaisesRegex(ValueError, 'HTTP 402.*not a rate-limit'):
                client.translate('text', '{}')
            self.assertEqual(call.call_count, 1)

    def test_transport_retries_and_truncation(self):
        client = tr.Client('https://example.com/v1', 'test', 'secret')
        with patch.object(tr.request, 'urlopen', side_effect=HTTPError(client.url, 429, 'busy', {}, None)) as call:
            with patch.object(tr.time, 'sleep'):
                with self.assertRaisesRegex(ValueError, 'HTTP 429'):
                    client.translate('text', '{}')
            self.assertEqual(call.call_count, 3)
        client.next_request = 0
        with patch.object(tr.request, 'urlopen') as call:
            call.return_value.__enter__.return_value.read.return_value = json.dumps({
                'choices': [{'finish_reason': 'length', 'message': {'content': 'partial'}}]}).encode()
            with self.assertRaisesRegex(ValueError, 'truncated'):
                client.translate('text', '{}')

    def test_large_table_chunk_boundaries_and_structure(self):
        text = '| Heading | Value |\n|---|---|\n' + '| 中文 | value |\n' * 1000
        parts = list(tr.chunks(text))
        self.assertGreater(len(parts), 1)
        self.assertEqual(''.join(parts), text)
        self.assertTrue(all(len(part) <= 8000 for part in parts))
        with self.assertRaisesRegex(ValueError, 'structure'):
            tr.validate(text, text + '| added | row |\n')

    def test_frontmatter_remains_unchanged(self):
        text = '---\ntitle: 中文\ntags: [test]\n---\n\n# 标题\n'
        protected, values = tr.protect(text)
        self.assertNotIn('title:', protected)
        self.assertEqual(tr.restore(protected, protected, values), text)

    def test_heading_ids_and_stale_source_fallback(self):
        try:
            import markdown
            from markdown.extensions.toc import slugify_unicode
        except ImportError:
            self.skipTest("python-markdown is not installed")
        from types import SimpleNamespace
        from tools.docs import mkdocs_i18n as hook

        self.source.write_text('# 标题\n\n## 重复\n\n## 重复\n\n末尾\n----\n')
        tr.run(options(), self.root, self.client)
        config = dict(markdown_extensions=['toc', 'attr_list', 'fenced_code'],
                      mdx_configs={'toc': {'slugify': slugify_unicode}})
        class Config(dict):
            plugins = {'i18n': SimpleNamespace(current_language='en')}
        config = Config(config)
        hook.on_config(config)
        page = SimpleNamespace(file=SimpleNamespace(abs_src_path=str(tr.target_for(self.source))))
        with patch.object(hook, 'ROOT', self.root):
            hook.on_pre_build(config)
            text = hook.on_page_markdown(tr.target_for(self.source).read_text(), page=page, config=config, files=None)
            html = markdown.Markdown(extensions=config['markdown_extensions'],
                                     extension_configs=config['mdx_configs']).convert(text)
            self.assertIn('id="标题">Title', html)
            self.assertIn('id="重复_1"', html)
            self.assertIn('id="末尾"', html)
            self.source.write_text(self.source.read_text() + '\nNew current source\n')
            text = hook.on_page_markdown('Old translation', page=page, config=config, files=None)
            self.assertIn('Translation update pending', text)
            self.assertIn('New current source', text)
            self.assertNotIn('Old translation', text)
            self.assertIsNone(hook._heading_ids)


if __name__ == '__main__':
    unittest.main()
