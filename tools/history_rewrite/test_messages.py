import json
import tempfile
import unittest
from pathlib import Path
import messages


class MessageRewriteTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.repo = self.root / 'source'
        self.repo.mkdir()
        messages.git(self.repo, 'init', '-q', '-b', 'main')
        self.tree = messages.git(self.repo, 'mktree', data=b'').decode().strip()
        self.plan = self.root / 'plan.json'
        self.out = self.root / 'preview'
        self.evidence = self.root / 'evidence'

    def tearDown(self):
        self.tmp.cleanup()

    def commit(self, body, parents=(), extra=b''):
        raw = (f'tree {self.tree}\n' + ''.join(f'parent {p}\n' for p in parents) +
               'author Original Author <author@example.com> 1700000001 +0530\n' +
               'committer Original Committer <committer@example.com> 1700000017 -0700\n').encode() + extra + b'\n' + body.encode()
        return messages.git(self.repo, 'hash-object', '-w', '-t', 'commit', '--stdin', data=raw).decode().strip()

    def run_preview(self, plan):
        self.plan.write_text(json.dumps(plan))
        return messages.preview(self.repo, self.out, self.plan, self.evidence)

    def test_exact_identity_times_order_trees_tags_and_unusual_refs(self):
        base = self.commit('initial\n')
        main = self.commit('main branch\n', [base])
        side = self.commit('side branch\n\nCo-Authored-By: Claude Opus <noreply@anthropic.com>\nCo-Authored-By: Human <human@example.com>\n', [base])
        merge = self.commit('merge\n', [main, side])
        empty = self.commit('empty\n', [merge])
        messages.git(self.repo, 'update-ref', 'refs/heads/main', empty)
        messages.git(self.repo, 'update-ref', 'refs/stash', merge)
        messages.git(self.repo, 'update-ref', 'refs/trees/base', self.tree)
        blob = messages.git(self.repo, 'hash-object', '-w', '--stdin', data=b'\x00required\xff').decode().strip()
        messages.git(self.repo, 'update-ref', 'refs/blobs/fixture', blob)
        tag = (f'object {side}\ntype commit\ntag release\ntagger Release Author <tag@example.com> 1700000021 +0900\n\nOriginal release notes\n').encode()
        tagoid = messages.git(self.repo, 'hash-object', '-w', '-t', 'tag', '--stdin', data=tag).decode().strip()
        messages.git(self.repo, 'update-ref', 'refs/tags/release', tagoid)
        tree_tag = (f'object {self.tree}\ntype tree\ntag tree\ntagger Tagger <tag@example.com> 1700000022 -0300\n\nTree fixture\n').encode()
        toid = messages.git(self.repo, 'hash-object', '-w', '-t', 'tag', '--stdin', data=tree_tag).decode().strip()
        messages.git(self.repo, 'update-ref', 'refs/tags/tree', toid)
        nested = (f'object {tagoid}\ntype tag\ntag nested\ntagger Tagger <tag@example.com> 1700000023 +0000\n\nNested release\n').encode()
        noid = messages.git(self.repo, 'hash-object', '-w', '-t', 'tag', '--stdin', data=nested).decode().strip()
        messages.git(self.repo, 'update-ref', 'refs/tags/nested', noid)
        before = messages.refs(self.repo)
        plan = {base: 'feat(kernel): initialize the kernel\n', main: 'fix(mm): handle anonymous faults\n',
                side: 'test(mm): cover shared faults\n\nCo-Authored-By: Human <human@example.com>\n',
                merge: 'merge(mm): integrate shared-fault coverage\n', empty: 'chore(history): retain the empty revision\n'}
        result = self.run_preview(plan)
        self.assertTrue(result['ok'])
        self.assertEqual(result['commits'], 5)
        self.assertEqual(result['merges'], 1)
        self.assertEqual(result['forks'], 1)
        self.assertEqual(result['false_coauthor_lines_removed'], 1)
        self.assertEqual(result['annotated_tags'], 3)
        self.assertEqual(messages.refs(self.repo), before)
        self.assertEqual(messages.refs(self.out)['refs/trees/base'], self.tree)
        self.assertEqual(messages.refs(self.out)['refs/blobs/fixture'], blob)
        self.assertFalse((self.out / 'objects/info/alternates').exists())

    def test_rejects_incomplete_plan_before_clone(self):
        oid = self.commit('old\n')
        messages.git(self.repo, 'update-ref', 'refs/heads/main', oid)
        with self.assertRaisesRegex(messages.RewriteError, 'cover each reachable commit'):
            self.run_preview({})
        self.assertFalse(self.out.exists())

    def test_rejects_signed_commit_and_isolation(self):
        oid = self.commit('old\n', extra=b'gpgsig -----BEGIN PGP SIGNATURE-----\n fake\n')
        messages.git(self.repo, 'update-ref', 'refs/heads/main', oid)
        with self.assertRaisesRegex(messages.RewriteError, 'signed commit'):
            self.run_preview({oid: 'fix(kernel): correct the trap state\n'})
        self.assertFalse(self.out.exists())
        with self.assertRaisesRegex(messages.RewriteError, 'separate directories'):
            messages.preview(self.repo, self.repo / 'nested', self.plan, self.evidence)
        self.assertFalse((self.repo / 'nested').exists())

    def test_rejects_identity_collapse(self):
        a = self.commit('one\n')
        b = self.commit('two\n')
        messages.git(self.repo, 'update-ref', 'refs/heads/main', a)
        messages.git(self.repo, 'update-ref', 'refs/heads/side', b)
        with self.assertRaisesRegex(messages.RewriteError, 'identities collapsed'):
            self.run_preview({a: 'feat(kernel): initialize the kernel\n', b: 'feat(kernel): initialize the kernel\n'})

    def test_preserves_real_coauthors(self):
        oid = self.commit('old\n\nCo-Authored-By: Human <human@example.com>\n')
        messages.git(self.repo, 'update-ref', 'refs/heads/main', oid)
        with self.assertRaisesRegex(messages.RewriteError, 'unrelated coauthors changed'):
            self.run_preview({oid: 'fix(kernel): correct the trap state\n'})

    def test_rejects_invalid_style_and_false_trailers(self):
        for body, count in [('fix: missing scope\n', 1), ('fix(mm): Correct faults\n', 1),
                            ('fix(mm): correct faults\n', 2), ('merge(mm): integrate fixes\n', 1),
                            ('fix(mm): correct faults\nbody\n', 1),
                            ('fix(mm): correct faults\n\nCo-Authored-By: Claude Opus <bot@example.com>\n', 1)]:
            with self.subTest(body=body), self.assertRaises(messages.RewriteError):
                messages.check_message(body, count)


if __name__ == '__main__':
    unittest.main()
