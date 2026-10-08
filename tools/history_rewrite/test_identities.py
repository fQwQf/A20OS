import json
import tempfile
import unittest
from pathlib import Path
import identities
from messages import git, raw_object, refs, split, values


class IdentityRewriteTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.repo = self.root / 'source'
        self.repo.mkdir()
        git(self.repo, 'init', '-q', '-b', 'main')
        self.tree = git(self.repo, 'mktree', data=b'').decode().strip()
        self.plan = self.root / 'plan.json'
        self.plan.write_text(json.dumps({'identities': {'sisyphus <sisyphus@local>': 'fQwQf <user@example.com>'}}))
        self.output = self.root / 'preview'
        self.artifacts = self.root / 'artifacts'

    def tearDown(self):
        self.temp.cleanup()

    def commit(self, body, parents=(), author='sisyphus <sisyphus@local>', committer='sisyphus <sisyphus@local>', extra=''):
        raw = (f'tree {self.tree}\n' + ''.join(f'parent {x}\n' for x in parents) +
               f'author {author} 1700000001 +0530\ncommitter {committer} 1700000017 -0700\n{extra}\n{body}')
        return git(self.repo, 'hash-object', '-w', '-t', 'commit', '--stdin', data=raw.encode()).decode().strip()

    def test_mixed_headers_dates_dag_tags_and_direct_tree_refs(self):
        human = 'Original Human <human@example.com>'
        base = self.commit('Base message\n\nMention sisyphus without changing this text.\n', committer=human)
        main = self.commit('Main branch\n', [base], author=human)
        side = self.commit('Other branch\n', [base], author=human, committer=human)
        merge = self.commit('Merge branches\n', [main, side], author=human, committer=human)
        git(self.repo, 'update-ref', 'refs/heads/main', merge)
        git(self.repo, 'update-ref', 'refs/stash', main)
        git(self.repo, 'update-ref', 'refs/trees/base', self.tree)
        tag = f'object {main}\ntype commit\ntag v1\ntagger Human <human@example.com> 1700000088 +0900\n\nUnchanged release notes\n'
        tagoid = git(self.repo, 'hash-object', '-w', '-t', 'tag', '--stdin', data=tag.encode()).decode().strip()
        git(self.repo, 'update-ref', 'refs/tags/v1', tagoid)
        before = refs(self.repo)
        result = identities.preview(self.repo, self.output, self.plan, self.artifacts)
        self.assertTrue(result['ok'])
        self.assertEqual(result['author_corrected'], 1)
        self.assertEqual(result['committer_corrected'], 1)
        self.assertEqual(result['merges'], 1)
        self.assertEqual(result['forks'], 1)
        self.assertEqual(refs(self.repo), before)
        mapping = json.loads((self.artifacts / 'map.json').read_text())
        headers, body = split(raw_object(self.output, mapping['commits'][base])[1])
        self.assertEqual(values(headers, b'author'), ['fQwQf <user@example.com> 1700000001 +0530'])
        self.assertEqual(values(headers, b'committer'), [human+' 1700000017 -0700'])
        self.assertIn(b'Mention sisyphus', body)
        mh, _ = split(raw_object(self.output, mapping['commits'][main])[1])
        self.assertEqual(values(mh, b'author'), [human+' 1700000001 +0530'])
        self.assertEqual(values(mh, b'committer'), ['fQwQf <user@example.com> 1700000017 -0700'])
        gh, _ = split(raw_object(self.output, mapping['commits'][merge])[1])
        self.assertEqual(values(gh, b'parent'), [mapping['commits'][main], mapping['commits'][side]])
        self.assertEqual(refs(self.output)['refs/trees/base'], self.tree)
        newtag = raw_object(self.output, mapping['tags'][tagoid])[1]
        self.assertEqual(newtag.replace(mapping['commits'][main].encode(), main.encode(), 1), tag.encode())

    def test_rejects_identity_collision(self):
        a = self.commit('Same message\n')
        b = self.commit('Same message\n', author='fQwQf <user@example.com>', committer='fQwQf <user@example.com>')
        git(self.repo, 'update-ref', 'refs/heads/main', a)
        git(self.repo, 'update-ref', 'refs/heads/other', b)
        before = refs(self.repo)
        with self.assertRaisesRegex(identities.RewriteError, 'collapse existing commit nodes'):
            identities.preview(self.repo, self.output, self.plan, self.artifacts)
        self.assertEqual(refs(self.repo), before)

    def test_no_match_and_signed_objects_fail_closed(self):
        human = 'Human <human@example.com>'
        oid = self.commit('Ordinary message\n', author=human, committer=human)
        git(self.repo, 'update-ref', 'refs/heads/main', oid)
        with self.assertRaisesRegex(identities.RewriteError, 'matches no'):
            identities.preview(self.repo, self.output, self.plan, self.artifacts)
        self.assertFalse(self.output.exists())
        signed = self.commit('Signed message\n', extra='gpgsig -----BEGIN PGP SIGNATURE-----\n fake\n')
        git(self.repo, 'update-ref', 'refs/heads/main', signed)
        with self.assertRaisesRegex(identities.RewriteError, 'signed commit'):
            identities.preview(self.repo, self.output, self.plan, self.artifacts)
        self.assertFalse(self.output.exists())

    def test_plan_and_directory_isolation(self):
        self.artifacts.mkdir()
        with self.assertRaisesRegex(identities.RewriteError, 'artifacts output must not exist'):
            identities.preview(self.repo, self.output, self.plan, self.artifacts)
        self.assertFalse(self.output.exists())
        self.artifacts.rmdir()
        self.plan.write_text(json.dumps({'identities': {'sisyphus <sisyphus@local>': 'Bad\nName <user@example.com>'}}))
        with self.assertRaisesRegex(identities.RewriteError, 'exact Name'):
            identities.preview(self.repo, self.output, self.plan, self.artifacts)
        with self.assertRaisesRegex(identities.RewriteError, 'separate directories'):
            identities.preview(self.repo, self.repo / 'nested', self.plan, self.artifacts)
        self.assertFalse((self.repo / 'nested').exists())


if __name__ == '__main__':
    unittest.main()
