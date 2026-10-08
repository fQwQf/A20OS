#!/usr/bin/env python3
"""Read-only independent verifier for an already completed preview2."""
import argparse, fnmatch, json, os, subprocess, sys
from collections import defaultdict
from pathlib import Path

class VerifyError(RuntimeError): pass

def git(repo,*args,input=None):
    p=subprocess.run(['git','-C',str(repo),*args],input=input,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if p.returncode: raise VerifyError(f"git {' '.join(args)}: {p.stderr.decode(errors='replace').strip()}")
    return p.stdout

def refs(repo):
    result={}
    for line in git(repo,'for-each-ref','--format=%(refname) %(objectname)').decode().splitlines():
        ref,oid=line.split(' ',1); result[ref]=oid
    return result

def parse_snapshot(path):
    result={}
    for line in Path(path).read_text().splitlines():
        b=line.split()
        if len(b)<2: continue
        if b[0].startswith('refs/'): result[b[0]]=b[1]
        elif b[1].startswith('refs/'): result[b[1]]=b[0]
    return result

def typ(repo,oid): return git(repo,'cat-file','-t',oid).strip().decode()
def raw(repo,oid,t=None): return git(repo,'cat-file',t or typ(repo,oid),oid)
def sections(data):
    i=data.find(b'\n\n')
    if i<0: raise VerifyError('malformed object without header separator')
    return data[:i].split(b'\n'),data[i+2:]
def val(hs,key):
    xs=[h[len(key)+1:] for h in hs if h.startswith(key+b' ')]
    if len(xs)!=1: raise VerifyError(f'expected one {key!r} header')
    return xs[0]
def parents(hs): return [h[7:].decode() for h in hs if h.startswith(b'parent ')]
def treeoid(hs): return val(hs,b'tree').decode()
def peel(repo,oid):
    while typ(repo,oid)=='tag':
        hs,_=sections(raw(repo,oid,'tag')); oid=val(hs,b'object').decode()
    return oid,typ(repo,oid)
def reachable_commits(repo,refs_):
    seen=set(); stack=[]
    for oid in refs_.values():
        c,t=peel(repo,oid)
        if t=='commit': stack.append(c)
    while stack:
        c=stack.pop()
        if c in seen: continue
        seen.add(c); hs,_=sections(raw(repo,c,'commit')); stack.extend(parents(hs))
    return seen

def tag_objects(repo,refs_):
    seen=set(); stack=[o for r,o in refs_.items() if r.startswith('refs/tags/')]
    while stack:
        o=stack.pop()
        if o in seen or typ(repo,o)!='tag': continue
        seen.add(o); hs,_=sections(raw(repo,o,'tag')); target=val(hs,b'object').decode()
        if typ(repo,target)=='tag': stack.append(target)
    return seen

def tree_entries_batch(repo,oids):
    oids=sorted(set(oids)); result={}
    if not oids: return result
    p=subprocess.run(['git','-C',str(repo),'cat-file','--batch'],input=(''.join(x+'\n' for x in oids)).encode(),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if p.returncode: raise VerifyError(f'cat-file --batch failed: {p.stderr.decode(errors="replace")}')
    data=p.stdout; pos=0
    for requested in oids:
        end=data.find(b'\n',pos)
        if end<0: raise VerifyError('truncated cat-file batch header')
        hdr=data[pos:end].split(); pos=end+1
        if len(hdr)!=3 or hdr[1]!=b'tree': raise VerifyError(f'expected tree object {requested}; got {hdr!r}')
        size=int(hdr[2]); body=data[pos:pos+size]; pos+=size
        if data[pos:pos+1]!=b'\n': raise VerifyError('malformed cat-file batch framing')
        pos+=1
        oidbytes=40//2 if len(requested)==40 else 64//2
        entries=[]; i=0
        while i<len(body):
            sp=body.find(b' ',i); nul=body.find(b'\0',sp+1)
            if sp<0 or nul<0: raise VerifyError(f'malformed raw tree {requested}')
            mode=body[i:sp]; name=body[sp+1:nul]; start=nul+1
            endoid=start+oidbytes
            if endoid>len(body): raise VerifyError(f'truncated tree entry {requested}')
            oid=body[start:endoid].hex(); entries.append((mode,name,oid)); i=endoid
        result[requested]=entries
    return result

def clean_rules(plan):
    p=plan.get('purge',{})
    return {k:p.get(k,[]) for k in ('paths','prefixes','root_globs')}
def is_hit(path,root,rules):
    text=os.fsdecode(path)
    return (text in rules['paths'] or any(text==x[:-1] or text.startswith(x) for x in rules['prefixes']) or
            (root and any(fnmatch.fnmatchcase(os.fsdecode(path.rsplit(b'/',1)[-1]),g) for g in rules['root_globs'])))

def expected_messages(plan):
    result={}
    for g in plan.get('groups',[]): result[g['commits'][-1].lower()]=g['message'].encode()
    m=plan.get('messages',{})
    if isinstance(m,dict): m=[{'commit':k,'message':v} for k,v in m.items()]
    for x in m: result[x['commit'].lower()]=x['message'].encode() if isinstance(x['message'],str) else bytes.fromhex(x['message']['hex'])
    return result

def verify_trees(repo,mirror,tree_pairs,rules):
    """Walk each unique old/new tree pair in batches; compare exact non-target entries."""
    todo=[(a,b,b'',True) for a,b in set(tree_pairs)]
    visited=set(); cache_old={}; cache_new={}; deleted=0; checked=0; names=set()
    while todo:
        batch=todo[:512]; todo=todo[512:]
        need_old={a for a,_,_,_ in batch if a not in cache_old}
        need_new={b for _,b,_,_ in batch if b not in cache_new}
        cache_old.update(tree_entries_batch(repo,need_old)); cache_new.update(tree_entries_batch(mirror,need_new))
        for old,new,path,root in batch:
            key=(old,new,path,root)
            if key in visited: continue
            visited.add(key); checked+=1
            oe={n:(m,o) for m,n,o in cache_old[old]}; ne={n:(m,o) for m,n,o in cache_new[new]}
            for name,(mode,oid) in oe.items():
                full=path+((b'/' if path else b'')+name)
                if is_hit(full,root,rules):
                    if name in ne: raise VerifyError(f'purge hit remained in mapped tree: {os.fsdecode(full)}')
                    deleted+=1; names.add(os.fsdecode(full)); continue
                if name not in ne: raise VerifyError(f'non-target path disappeared: {os.fsdecode(full)}')
                nmode,noid=ne[name]
                if mode!=nmode: raise VerifyError(f'non-target mode changed: {os.fsdecode(full)}')
                is_tree=(mode in (b'040000',b'40000'))
                if is_tree:
                    todo.append((oid,noid,full,False))
                elif oid!=noid: raise VerifyError(f'non-target object changed: {os.fsdecode(full)}')
            for name in ne.keys()-oe.keys():
                full=path+((b'/' if path else b'')+name)
                raise VerifyError(f'new path appeared in mapped tree: {os.fsdecode(full)}')
    return {'unique_tree_pairs_checked':checked,'deleted_entry_occurrences':deleted,'deleted_paths':sorted(names)}

def verify(args):
    src=Path(args.repo).resolve(); dst=Path(args.mirror).resolve(); art=Path(args.results).resolve()
    plan=json.loads(Path(args.plan).read_text())
    baseline_refs=parse_snapshot(args.refs_before)
    oldrefs=refs(src); newrefs=refs(dst)
    check(oldrefs==baseline_refs,'current source refs differ from refs-before snapshot')
    check(newrefs==json.load(open(art/'map.json'))['refs'],'mirror ref values differ from map artifact')
    with open(art/'map.json') as f: maps=json.load(f)
    with open(art/'retained-removed.json') as f: manifest=json.load(f)
    commits=reachable_commits(src,oldrefs); tags=tag_objects(src,oldrefs)
    cmap=maps['commits']; tmap=maps['tags']
    check(set(cmap)==commits,f'commit map coverage {len(cmap)} != source reachable {len(commits)}')
    check(set(tmap)==tags,f'tag object map coverage {len(tmap)} != source annotated tags {len(tags)}')
    groups=plan.get('groups',[]); check(len(groups)==args.expected_groups,f'expected {args.expected_groups} groups, found {len(groups)}')
    removed={c.lower():g['commits'][-1].lower() for g in groups for c in g['commits'][:-1]}
    check(len(removed)==sum(len(g['commits'])-1 for g in groups),'duplicate group removed entries')
    check(set(manifest['removed'])==set(removed),'removed manifest differs from declared groups')
    check(len(commits)==args.expected_commits,f'expected {args.expected_commits} source commits, found {len(commits)}')
    check(len(removed)==args.expected_removed,f'expected {args.expected_removed} removals, found {len(removed)}')
    for old,anchor in removed.items(): check(cmap[old]==cmap[anchor],f'removed commit does not map to anchor: {old}')
    for g in groups:
        seq=[x.lower() for x in g['commits']]
        group_authors=[]
        for i,c in enumerate(seq):
            hs,_=sections(raw(src,c,'commit')); a=val(hs,b'author'); group_authors.append(a.rsplit(b' ',2)[0])
            check(len(parents(hs))==1,f'group contains non-single-parent commit: {c}')
            if i: check(parents(hs)==[seq[i-1]],f'group is not parent-adjacent: {c}')
        check(len(set(group_authors))==1,f'group author identity mismatch: {seq[0]}')
    new_reachable=reachable_commits(dst,newrefs)
    check(new_reachable<=set(cmap.values()),'mirror has reachable commits outside source commit map')
    oldmeta={}; oldparents={}; child=defaultdict(set); oldtrees={}
    for c in commits:
        hs,body=sections(raw(src,c,'commit')); oldmeta[c]=(val(hs,b'author'),val(hs,b'committer'),body,hs)
        oldparents[c]=parents(hs); oldtrees[c]=treeoid(hs)
        for p in oldparents[c]: child[p].add(c)
    base_tag=[o for r,o in oldrefs.items() if r.startswith('refs/tags/') and r.rsplit('/',1)[-1]=='v0.13']
    check(bool(base_tag),'v0.13 ref missing')
    base=set(); stack=[]
    for o in base_tag:
        c,t=peel(src,o)
        if t=='commit': stack.append(c)
    while stack:
        c=stack.pop()
        if c in base: continue
        base.add(c); stack.extend(oldparents[c])
    tips=set()
    for o in oldrefs.values():
        c,t=peel(src,o)
        if t=='commit': tips.add(c)
    for c in removed:
        check(c not in tips,f'ref or tag tip removed: {c}')
        check(len(child[c])==1,f'removed node fork/external child: {c}')
        check(c not in base,f'protected v0.13 commit removed: {c}')
    # Main graph merge count and every merge across all refs.
    main=oldrefs.get('refs/heads/main',oldrefs.get('refs/remotes/origin/main'))
    check(main is not None,'main ref missing')
    mainoid,t=peel(src,main); mainset=set(); stack=[mainoid]
    while stack:
        c=stack.pop()
        if c in mainset or c in base: continue
        mainset.add(c); stack.extend(oldparents[c])
    merges={c for c in commits if len(oldparents[c])>1}; mainmerges=merges&mainset
    check(len(merges)==args.expected_merges_all,f'all-ref merge count {len(merges)} != {args.expected_merges_all}')
    check(len(mainmerges)==args.expected_merges_main,f'main merge count {len(mainmerges)} != {args.expected_merges_main}')
    check(not (merges&set(removed)),'a merge commit was removed')
    forks={c for c in commits if len(child[c])>1}
    check(not (forks&set(removed)),f'all-ref multi-child commits removed: {sorted(forks&set(removed))[:8]}')
    topology_path=Path(args.topology) if args.topology else Path(args.plan).resolve().parent/'topology.json'
    topology=json.loads(topology_path.read_text())
    protected_forks=set(topology['forks_all_refs'])
    check(len(protected_forks)==args.expected_forks,f'topology protected fork count {len(protected_forks)} != {args.expected_forks}')
    check(protected_forks<=forks,'topology fork inventory contains a non-fork source commit')
    check(not (protected_forks&set(removed)),'a topology-listed fork was removed')
    tree_map=json.load(open(art/'tree-map.json'))
    commit_tree_map=tree_map['commit_trees']; direct_tree_map=tree_map.get('direct_tree_objects',{})
    expected_msg_by_commit=expected_messages(plan); parent_overrides={}
    for g in groups:
        seq=[x.lower() for x in g['commits']]; parent_overrides[seq[-1]]=[oldparents[seq[0]][0]]
    pairset=set(); output_headers={}; metadata_checked=0
    for c in commits:
        if c in removed: continue
        new=cmap[c]; oldauth,oldcomm,oldbody,oldhs=oldmeta[c]
        nh,nb=sections(raw(dst,new,'commit')); output_headers[c]=nh
        check(val(nh,b'author')==oldauth,f'author header changed: {c}')
        check(val(nh,b'committer')==oldcomm,f'committer header changed: {c}')
        check(nb==expected_msg_by_commit.get(c,oldbody),f'commit message mismatch: {c}')
        wantp=[cmap[p] for p in parent_overrides.get(c,oldparents[c])]
        gotp=parents(nh); check(gotp==wantp,f'parent mapping/order mismatch: {c}')
        check(len(gotp)==len(oldparents[c]) or c in parent_overrides,f'parent count changed: {c}')
        if c in merges: check(len(gotp)>1 and len(set(gotp))==len(gotp),f'merge lost/collapsed parents: {c}')
        check(treeoid(nh)==commit_tree_map[c]['new'],f'commit tree-map mismatch: {c}')
        check(commit_tree_map[c]['old']==oldtrees[c],f'old tree-map input mismatch: {c}')
        pairset.add((oldtrees[c],commit_tree_map[c]['new']))
        metadata_checked+=1
    check(set(manifest['retained'])==set(commits)-set(removed),'retained manifest differs from source graph')
    # All fork nodes remain distinct retained commit objects.
    check(not (forks&set(removed)),'fork node protection failed')
    # Protected v0.13 ancestry allows tree/parent rehash only: no message or identity edits, parent cardinality stays.
    for c in base:
        nh,nb=sections(raw(dst,cmap[c],'commit'))
        check(nb==oldmeta[c][2],f'protected history message changed: {c}')
        check(val(nh,b'author')==oldmeta[c][0] and val(nh,b'committer')==oldmeta[c][1],f'protected identity/date changed: {c}')
        check(len(parents(nh))==len(oldparents[c]),f'protected parent topology changed: {c}')
    # Annotated tag headers/taggers/messages stay exact except target OID.
    tags_checked=0
    for oldtag in tags:
        newtag=tmap[oldtag]; oh,ob=sections(raw(src,oldtag,'tag')); nh,nb=sections(raw(dst,newtag,'tag'))
        check([(x.split(b' ',1)[0],x.split(b' ',1)[1]) for x in oh if not x.startswith(b'object ')]==[(x.split(b' ',1)[0],x.split(b' ',1)[1]) for x in nh if not x.startswith(b'object ')],f'tag metadata headers changed: {oldtag}')
        check(ob==nb,f'tag message changed: {oldtag}')
        target=val(oh,b'object').decode(); targettype=val(oh,b'type')
        mappedtarget=tmap.get(target,cmap.get(target,direct_tree_map.get(target,target)))
        check(val(nh,b'object').decode()==mappedtarget,f'tag target mapping mismatch: {oldtag}')
        check(val(nh,b'type')==targettype,f'tag target type changed: {oldtag}')
        if targettype==b'tree': pairset.add((target,mappedtarget))
        tags_checked+=1
    for r,o in oldrefs.items():
        n=newrefs[r]; check(n==maps['refs'][r],f'ref artifact mismatch: {r}')
        check(typ(src,o)==typ(dst,n),f'ref endpoint object type changed: {r}')
        oc,ot=peel(src,o); nc,nt=peel(dst,n)
        if ot=='commit': check(nc==cmap[oc],f'ref peeled commit does not map: {r}')
        elif ot=='tree': pairset.add((oc,direct_tree_map.get(oc,oc)))
        elif ot=='blob' and any(clean_rules(plan).values()): raise VerifyError(f'purge with direct blob endpoint must have failed closed: {r}')
    tree_result=verify_trees(src,dst,pairset,clean_rules(plan))
    result={'ok':True,'source_ref_snapshot_match':True,'source_commit_count':len(commits),'retained_commit_count':len(commits)-len(removed),'removed_commit_count':len(removed),'main_merge_count':len(mainmerges),'all_ref_merge_count':len(merges),'source_multi_child_commit_count':len(forks),'topology_protected_fork_count':len(protected_forks),'protected_v0.13_ancestry_count':len(base),'ref_count':len(oldrefs),'annotated_tag_count':tags_checked,'retained_metadata_checked':metadata_checked,'tree_verification':tree_result,'checks':['all-source-reachable commit map coverage','declared removed map and anchor mapping','ref snapshot and complete ref inventory','all raw author/committer headers and expected messages','parent order/count including every merge','all main/all-ref merge and fork preservation','annotated tag message/tagger/target mapping','all unique old/new tree pairs recursively checked for exact non-target entries','all source ref endpoints resolve to mapped output objects']}
    Path(args.validation).write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
    return result

def check(condition,message):
    if not condition: raise VerifyError(message)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo',required=True); p.add_argument('--mirror',required=True); p.add_argument('--results',required=True); p.add_argument('--plan',required=True); p.add_argument('--refs-before',required=True); p.add_argument('--validation',required=True)
    p.add_argument('--topology'); p.add_argument('--expected-commits',type=int,default=2356); p.add_argument('--expected-groups',type=int,default=74); p.add_argument('--expected-removed',type=int,default=162); p.add_argument('--expected-merges-main',type=int,default=57); p.add_argument('--expected-merges-all',type=int,default=92); p.add_argument('--expected-forks',type=int,default=35)
    a=p.parse_args()
    try: print(json.dumps(verify(a),indent=2)); return 0
    except Exception as e: print(f'independent verification failed: {e}',file=sys.stderr); return 2
if __name__=='__main__': raise SystemExit(main())
