import json, os, subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).parent))
import engine

def run(repo,*args,input=None):
    return subprocess.run(['git','-C',str(repo),*args],input=input,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True).stdout.decode().strip()

def commit(repo,msg,parents=(),date='1700000000 +0800',author='Test User <test@example.com>',committer=None):
    committer=committer or author
    run(repo,'add','-A')
    run(repo,'update-index','--add','--cacheinfo','160000,'+'deadbeef'*5+',vendor/submodule')
    tree=run(repo,'write-tree')
    env=os.environ.copy(); env['GIT_AUTHOR_NAME'],env['GIT_AUTHOR_EMAIL']=author[:-1].split(' <'); env['GIT_COMMITTER_NAME'],env['GIT_COMMITTER_EMAIL']=committer[:-1].split(' <'); env['GIT_AUTHOR_DATE']=date; env['GIT_COMMITTER_DATE']=date
    raw=f'tree {tree}\n'+''.join(f'parent {p}\n' for p in parents)+f'author {author} {date}\ncommitter {committer} {date}\n\n{msg}\n'
    p=subprocess.run(['git','-C',str(repo),'hash-object','-t','commit','-w','--stdin'],input=raw.encode(),stdout=subprocess.PIPE,check=True,env=env)
    return p.stdout.decode().strip()

class EngineTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.root=Path(self.tmp.name); self.repo=self.root/'src'; self.repo.mkdir()
        run(self.repo,'init','-q'); run(self.repo,'config','user.name','Test User'); run(self.repo,'config','user.email','test@example.com')
        (self.repo/'.zcode/plans').mkdir(parents=True); (self.repo/'.zcode/plans/old.md').write_text('remove me\n')
        (self.repo/'root-temp.d').write_text('build output\n'); (self.repo/'nested').mkdir()
        (self.repo/'nested/root-temp.d').write_text('keep nested same-name\n')
        (self.repo/'required.bin').write_bytes(b'\x00\xffrequired\x00')
        (self.repo/'file').write_text('pre-v0.13\n'); self.pre=commit(self.repo,'pre-v0.13 ancestor')
        (self.repo/'file').write_text('base\n'); self.base=commit(self.repo,'base',[self.pre])
        # v0.13 immutable annotated tag and its history.
        run(self.repo,'tag','-a','v0.13','-m','protected release',self.base)
        # Empty commits are retained by the rewrite and are never pruned.
        self.empty=commit(self.repo,'empty commit',[self.base],'1700000000 +0800')
        run(self.repo,'update-ref','refs/heads/empty',self.empty)
        (self.repo/'file').write_text('a\n'); self.a=commit(self.repo,'first', [self.base], '1700000001 +0800')
        (self.repo/'file').write_text('anchor\n'); self.b=commit(self.repo,'anchor original', [self.a], '1700000002 -0700')
        # Branch descendant and main descendant reconverge through a merge.
        (self.repo/'side').write_text('side\n'); self.side=commit(self.repo,'side descendant', [self.b], '1700000003 +0800')
        (self.repo/'main').write_text('main\n'); self.main=commit(self.repo,'main descendant', [self.b], '1700000004 +0800')
        (self.repo/'merge').write_text('merge\n'); self.merge=commit(self.repo,'merge preserved', [self.main,self.side], '1700000005 +0800')
        # stash ref is a real reachable ref and must map along with every other ref.
        run(self.repo,'update-ref','refs/stash',self.side)
        run(self.repo,'update-ref','refs/heads/main',self.merge)
        run(self.repo,'update-ref','refs/heads/side',self.side)
        run(self.repo,'tag','-a','v0.14','-m','later annotated release',self.b)
        base_tree=run(self.repo,'rev-parse',self.base+'^{tree}')
        run(self.repo,'tag','-a','tree-v0.14','-m','tree target tag',base_tree)
        run(self.repo,'update-ref','refs/trees/base-tree',base_tree)
        self.plan=self.root/'plan.json'; self.art=self.root/'artifacts'; self.out=self.root/'mirror'
    def tearDown(self): self.tmp.cleanup()
    def write_plan(self, groups): self.plan.write_text(json.dumps({'groups':groups},indent=2))
    def test_dag_tag_timestamp_message_and_stash(self):
        self.write_plan([{'commits':[self.a,self.b],'message':'squashed message'}])
        result=engine.rewrite(self.repo,self.out,self.plan,self.art)
        self.assertTrue(result['ok'])
        newrefs=engine.refs(self.out)
        with open(self.art/'map.json') as f: cmap=json.load(f)['commits']
        self.assertNotEqual(cmap[self.b],self.b); self.assertEqual(cmap[self.a],cmap[self.b])
        _,anchor_raw=engine.obj(self.out,cmap[self.b],'commit'); anchor_hs,_=engine.split_headers(anchor_raw)
        self.assertEqual(engine.parse_parents(anchor_hs),[self.base])
        self.assertEqual(newrefs['refs/stash'],cmap[self.side])
        # Side descendant was rehashed only for parent mapping; message and raw dates survive.
        _,raw=engine.obj(self.out,cmap[self.side],'commit'); hs,body=engine.split_headers(raw)
        self.assertEqual(engine.parse_parents(hs),[cmap[self.b]])
        self.assertEqual(body,b'side descendant\n')
        _,braw=engine.obj(self.out,cmap[self.b],'commit'); bhs,bbody=engine.split_headers(braw)
        self.assertEqual(bbody,b'squashed message')
        self.assertEqual(engine.one(bhs,b'author'),b'Test User <test@example.com> 1700000002 -0700')
        self.assertEqual(engine.one(bhs,b'committer'),b'Test User <test@example.com> 1700000002 -0700')
        # Merge parent order/cardinality is preserved after mapping.
        _,mraw=engine.obj(self.out,cmap[self.merge],'commit'); mhs,_=engine.split_headers(mraw)
        self.assertEqual(engine.parse_parents(mhs),[cmap[self.main],cmap[self.side]])
        # Annotated tag target is remapped while tagger and message are byte-identical.
        oldtag=run(self.repo,'rev-parse','refs/tags/v0.14'); newtag=newrefs['refs/tags/v0.14']
        oh=engine.split_headers(engine.obj(self.repo,oldtag,'tag')[1]); nh=engine.split_headers(engine.obj(self.out,newtag,'tag')[1])
        self.assertEqual(engine.one(nh[0],b'object').decode(),cmap[self.b])
        self.assertEqual([x for x in oh[0] if not x.startswith(b'object ')],[x for x in nh[0] if not x.startswith(b'object ')])
        self.assertEqual(oh[1],nh[1])
    def test_purge_fails_closed_for_annotated_blob_tag(self):
        blob=run(self.repo,'rev-parse',self.base+':required.bin')
        run(self.repo,'tag','-a','unscoped-blob','-m','blob target',blob)
        old_tag=run(self.repo,'rev-parse','refs/tags/unscoped-blob')
        self.write_plan([])
        plan=json.loads(self.plan.read_text()); plan['purge']={'paths':['required.bin']}; self.plan.write_text(json.dumps(plan))
        with self.assertRaisesRegex(engine.RewriteError,'annotated tag .* targeting a blob'):
            engine.rewrite(self.repo,self.out,self.plan,self.art)
        self.assertEqual(run(self.repo,'rev-parse','refs/tags/unscoped-blob'),old_tag)

    def test_purge_rewrites_all_refs_and_preserves_nonmatching_trees(self):
        self.write_plan([])
        plan=json.loads(self.plan.read_text())
        plan['purge']={'paths':['.zcode/plans/old.md'],'prefixes':['.zcode/plans/'],'root_globs':['*.d']}
        self.plan.write_text(json.dumps(plan))
        result=engine.rewrite(self.repo,self.out,self.plan,self.art)
        self.assertTrue(result['ok'])
        with open(self.art/'map.json') as f: maps=json.load(f)
        with open(self.art/'tree-map.json') as f: tm=json.load(f)
        self.assertNotEqual(maps['commits'][self.pre],self.pre)
        self.assertNotEqual(maps['commits'][self.base],self.base)  # v0.13 tag target changed only for purge/tree propagation
        self.assertEqual(engine.peel_commit(self.out,engine.refs(self.out)['refs/tags/v0.13']),maps['commits'][self.base])
        _,new_base_raw=engine.obj(self.out,maps['commits'][self.base],'commit')
        new_base_headers,new_base_msg=engine.split_headers(new_base_raw)
        self.assertEqual(new_base_msg,b'base\n')
        self.assertEqual(engine.parse_parents(new_base_headers),[maps['commits'][self.pre]])
        old_base_headers,_=engine.split_headers(engine.obj(self.repo,self.base,'commit')[1])
        self.assertEqual(engine.one(new_base_headers,b'author'),engine.one(old_base_headers,b'author'))
        self.assertEqual(engine.one(new_base_headers,b'committer'),engine.one(old_base_headers,b'committer'))
        # Empty commit has its own retained mapped commit object, though its tree is empty after filtering.
        self.assertNotEqual(maps['commits'][self.empty],maps['commits'][self.base])
        etree=engine.one(engine.split_headers(engine.obj(self.out,maps['commits'][self.empty],'commit')[1])[0],b'tree').decode()
        btree=engine.one(engine.split_headers(engine.obj(self.out,maps['commits'][self.base],'commit')[1])[0],b'tree').decode()
        # The empty commit keeps its position and message even when its filtered tree equals its parent.
        self.assertEqual(etree,btree)
        for c,trees in tm['commit_trees'].items():
            engine.validate_filtered_tree(self.out,trees['old'],trees['new'],plan['purge'])
        # Root glob removes root-temp.d but does not match nested/root-temp.d; binary bytes stay exact.
        base_tree=tm['commit_trees'][self.base]['new']
        paths=run(self.out,'ls-tree','-r','--name-only',base_tree).splitlines()
        self.assertNotIn('root-temp.d',paths)
        self.assertIn('nested/root-temp.d',paths)
        blob=run(self.out,'rev-parse',base_tree+':required.bin')
        raw=subprocess.run(['git','-C',str(self.out),'cat-file','blob',blob],stdout=subprocess.PIPE,check=True).stdout
        self.assertEqual(raw,b'\x00\xffrequired\x00')
        with open(self.art/'purge-hits.json') as f: hits=json.load(f)
        self.assertGreater(hits['hit_commit_count'],0)
        self.assertTrue(any('.zcode/plans' in p for vals in hits['commits'].values() for p in vals))
        self.assertNotEqual(maps['refs']['refs/trees/base-tree'],run(self.repo,'rev-parse',self.base+'^{tree}'))
        with open(self.art/'purge-hits.json') as f: audit=json.load(f)
        self.assertTrue(any(x['endpoint']=='refs/trees/base-tree' and x['removed_paths'] for x in audit['tree_ref_hits']))
        tree_tag=maps['refs']['refs/tags/tree-v0.14']
        tree_tag_headers,_=engine.split_headers(engine.obj(self.out,tree_tag,'tag')[1])
        self.assertEqual(engine.one(tree_tag_headers,b'object').decode(),maps['refs']['refs/trees/base-tree'])
        # Both commits share the original .zcode subtree; per-commit hit accounting is not cache-dependent.
        self.assertIn(self.pre,hits['commits'])
        self.assertIn(self.base,hits['commits'])
        vendor_tree=run(self.out,'ls-tree',base_tree,'vendor').split()[2]
        gitlink=run(self.out,'ls-tree',vendor_tree,'submodule')
        self.assertIn('160000 commit deadbeefdeadbeefdeadbeefdeadbeefdeadbeef',gitlink)

    def test_different_committers_keep_anchor_header(self):
        author='Test User <test@example.com>'
        first=commit(self.repo,'first committer identity',[self.merge],'1700000011 +0800',author=author,committer='Build Bot <build@example.com>')
        anchor=commit(self.repo,'anchor committer identity',[first],'1700000012 -0600',author=author,committer='Release Bot <release@example.com>')
        run(self.repo,'update-ref','refs/heads/main',anchor)
        self.write_plan([{'commits':[first,anchor],'message':'same author, anchor committer'}])
        engine.rewrite(self.repo,self.out,self.plan,self.art)
        with open(self.art/'map.json') as f: cmap=json.load(f)['commits']
        _,raw=engine.obj(self.out,cmap[anchor],'commit')
        headers,_=engine.split_headers(raw)
        self.assertEqual(engine.one(headers,b'author'),b'Test User <test@example.com> 1700000012 -0600')
        self.assertEqual(engine.one(headers,b'committer'),b'Release Bot <release@example.com> 1700000012 -0600')

    def test_artifact_path_rejected_before_source_pollution(self):
        self.write_plan([])
        source_ref=run(self.repo,'rev-parse','refs/heads/main')
        nested=self.repo/'rewrite-artifacts'
        with self.assertRaisesRegex(engine.RewriteError,'inside the source repository'):
            engine.rewrite(self.repo,self.out,self.plan,nested)
        self.assertFalse(nested.exists())
        self.assertFalse(self.out.exists())
        self.assertEqual(run(self.repo,'rev-parse','refs/heads/main'),source_ref)
        overlap=self.root/'overlap-output'
        nested_artifacts=overlap/'artifacts'
        with self.assertRaisesRegex(engine.RewriteError,'must not overlap'):
            engine.rewrite(self.repo,overlap,self.plan,nested_artifacts)
        self.assertFalse(overlap.exists())

    def test_purge_fails_closed_for_direct_blob_ref(self):
        blob=run(self.repo,'rev-parse',self.base+':required.bin')
        run(self.repo,'update-ref','refs/blobs/unscoped',blob)
        self.write_plan([])
        plan=json.loads(self.plan.read_text()); plan['purge']={'paths':['required.bin']}; self.plan.write_text(json.dumps(plan))
        with self.assertRaisesRegex(engine.RewriteError,'direct blob ref'):
            engine.rewrite(self.repo,self.out,self.plan,self.art)
        self.assertEqual(run(self.repo,'rev-parse','refs/blobs/unscoped'),blob)

    def test_reject_cross_fork_group(self):
        self.write_plan([{'commits':[self.a,self.b],'message':'bad'}])
        # Make removed node a have a second child.
        fork=commit(self.repo,'extra child',[self.a],'1700000010 +0800')
        run(self.repo,'update-ref','refs/heads/extra',fork)
        with self.assertRaisesRegex(engine.RewriteError,'fork or has external child'):
            engine.rewrite(self.repo,self.out,self.plan,self.art)
    def test_reject_merge_in_group(self):
        self.write_plan([{'commits':[self.main,self.merge],'message':'bad'}])
        with self.assertRaisesRegex(engine.RewriteError,'merge/root'):
            engine.rewrite(self.repo,self.out,self.plan,self.art)

if __name__=='__main__': unittest.main(verbosity=2)
