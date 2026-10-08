#!/usr/bin/env python3
"""Conservative Git history rewrite preview engine.

Rewrites only objects in an isolated mirror. It never updates the source repository.
See ENGINE.md for the plan schema and invariants.
"""
from __future__ import annotations
import argparse, fnmatch, json, os, subprocess, sys, tempfile
from pathlib import Path
from collections import defaultdict

class RewriteError(RuntimeError): pass

def git(repo, *args, input=None, check=True):
    p=subprocess.run(['git','-C',str(repo),*args],input=input,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if check and p.returncode:
        raise RewriteError(f"git {' '.join(args)} failed: {p.stderr.decode(errors='replace').strip()}")
    return p.stdout

def oidlen(repo): return len(git(repo,'rev-parse','--show-object-format').strip())

def obj(repo, oid, typ=None):
    if typ is None: typ=git(repo,'cat-file','-t',oid).strip().decode()
    return typ, git(repo,'cat-file',typ,oid)

def hashobj(repo, typ, data): return git(repo,'hash-object','-w','-t',typ,'--stdin',input=data).strip().decode()

def split_headers(raw):
    i=raw.find(b'\n\n')
    if i<0: raise RewriteError('malformed commit/tag object: missing header separator')
    return raw[:i].split(b'\n'), raw[i+2:]

def parse_parents(headers): return [h[7:].decode() for h in headers if h.startswith(b'parent ')]
def one(headers, key):
    vals=[h[len(key)+1:] for h in headers if h.startswith(key+b' ')]
    if len(vals)!=1: raise RewriteError(f'malformed object: expected one {key.decode()} header')
    return vals[0]
def identity_key(raw):
    # Preserve raw header elsewhere; compare only identity, allowing original dates/timezones to differ.
    try: return raw.rsplit(b' ',2)[0]
    except Exception: return raw

def refs(repo):
    lines=git(repo,'for-each-ref','--format=%(refname) %(objectname)').decode().splitlines()
    out={}
    for line in lines:
        ref, oid=line.split(' ',1); out[ref]=oid
    return out

def peel_commit(repo, oid):
    typ=git(repo,'cat-file','-t',oid).strip().decode()
    while typ=='tag':
        raw=git(repo,'cat-file','tag',oid); headers,_=split_headers(raw)
        oid=one(headers,b'object').decode(); typ=git(repo,'cat-file','-t',oid).strip().decode()
    return oid if typ=='commit' else None

def reachable(repo, refmap):
    found=set()
    # Explicitly walk each endpoint so unusual refs and stash refs are included.
    stack=[]
    for oid in refmap.values():
        c=peel_commit(repo,oid)
        if c: stack.append(c)
    while stack:
        c=stack.pop()
        if c in found: continue
        found.add(c)
        typ,raw=obj(repo,c,'commit'); hs,_=split_headers(raw)
        stack.extend(parse_parents(hs))
    return found

def tag_object_order(repo, refmap):
    tags=set(); stack=[o for r,o in refmap.items() if r.startswith('refs/tags/')]
    while stack:
        oid=stack.pop()
        if oid in tags: continue
        typ=git(repo,'cat-file','-t',oid).strip().decode()
        if typ!='tag': continue
        tags.add(oid); hs,_=split_headers(git(repo,'cat-file','tag',oid))
        target=one(hs,b'object').decode()
        if git(repo,'cat-file','-t',target).strip()==b'tag': stack.append(target)
    return tags

def signed_payload(data):
    return b'gpgsig ' in data or b'mergetag ' in data or b'-----BEGIN PGP SIGNATURE-----' in data or b'-----BEGIN SSH SIGNATURE-----' in data

def normalize_purge(purge):
    if not isinstance(purge,dict): raise RewriteError('purge must be an object')
    unknown=set(purge)-{'paths','prefixes','root_globs'}
    if unknown: raise RewriteError(f'unknown purge keys: {sorted(unknown)}')
    paths=purge.get('paths',[]); prefixes=purge.get('prefixes',[]); globs=purge.get('root_globs',[])
    if not all(isinstance(x,str) for values in (paths,prefixes,globs) if isinstance(values,list) for x in values):
        raise RewriteError('purge paths, prefixes, and root_globs must contain strings')
    if not all(isinstance(values,list) for values in (paths,prefixes,globs)):
        raise RewriteError('purge paths, prefixes, and root_globs must be arrays')
    def valid_path(s): return bool(s) and '\0' not in s and not s.startswith('/') and '\\' not in s and all(p not in ('','.','..') for p in s.split('/'))
    for x in paths:
        if not valid_path(x): raise RewriteError(f'invalid exact purge path: {x!r}')
    for x in prefixes:
        if not valid_path(x[:-1]) or not x.endswith('/'): raise RewriteError(f'purge prefix must be a relative directory prefix ending in /: {x!r}')
    for x in globs:
        if not x or '\0' in x or '/' in x or '\\' in x: raise RewriteError(f'root_globs must match root entry names only: {x!r}')
    return {'paths':paths,'prefixes':prefixes,'root_globs':globs}

def purge_matches(path, root_entry, rules):
    text=os.fsdecode(path)
    return (text in rules['paths'] or any(text == x[:-1] or text.startswith(x) for x in rules['prefixes']) or
            (root_entry and any(fnmatch.fnmatchcase(os.fsdecode(path.rsplit(b'/',1)[-1]),pat) for pat in rules['root_globs'])))

def tree_entries(repo, tree, entry_cache=None):
    if entry_cache is not None and tree in entry_cache: return entry_cache[tree]
    raw=git(repo,'ls-tree','-z',tree)
    entries=[]
    for record in raw.split(b'\0'):
        if not record: continue
        meta,name=record.split(b'\t',1); mode,typ,oid=meta.split(b' ',2)
        entries.append((mode,typ,oid,name))
    result=tuple(entries)
    if entry_cache is not None: entry_cache[tree]=result
    return result

def filter_tree(repo, tree, rules, path=b'', root=True, cache=None, records=None, entry_cache=None):
    cache={} if cache is None else cache; records=[] if records is None else records
    key=(tree,path,root)
    if key in cache: return cache[key],records
    old_entries=tree_entries(repo,tree,entry_cache); kept=[]
    for mode,typ,oid,name in old_entries:
        childpath=path+((b'/' if path else b'')+name)
        if purge_matches(childpath,root,rules): continue
        childoid=oid.decode()
        if typ==b'tree':
            filtered,records=filter_tree(repo,childoid,rules,childpath,False,cache,records,entry_cache)
            childoid=filtered
        kept.append((mode,typ,childoid.encode(),name))
    unchanged=(len(kept)==len(old_entries) and all(kept[i]==old_entries[i] for i in range(len(kept))))
    if unchanged: newtree=tree
    elif not kept:
        # Empty trees are valid Git objects and must be written to retain empty commits.
        newtree=git(repo,'mktree',input=b'').strip().decode()
    else:
        data=b''.join(mode+b' '+typ+b' '+oid+b'\t'+name+b'\0' for mode,typ,oid,name in kept)
        newtree=git(repo,'mktree','--missing','-z',input=data).strip().decode()
    if newtree!=tree: records.append({'old':tree,'new':newtree,'path':os.fsdecode(path),'root':root})
    cache[key]=newtree
    return newtree,records

def collect_tree_hits(repo, tree, rules, path=b'', root=True, hits_cache=None, entry_cache=None):
    hits_cache={} if hits_cache is None else hits_cache
    key=(tree,path,root)
    if key in hits_cache: return hits_cache[key]
    hits=[]
    for mode,typ,oid,name in tree_entries(repo,tree,entry_cache):
        full=path+((b'/' if path else b'')+name)
        if purge_matches(full,root,rules): hits.append(os.fsdecode(full))
        elif typ==b'tree': hits.extend(collect_tree_hits(repo,oid.decode(),rules,full,False,hits_cache,entry_cache))
    result=tuple(sorted(set(hits)))
    hits_cache[key]=result
    return result

def validate_filtered_tree(repo, oldtree, newtree, rules, path=b'', root=True, cache=None, entry_cache=None):
    """Independent structural check: only matching entries may disappear."""
    cache=set() if cache is None else cache
    key=(oldtree,newtree,path,root)
    if key in cache: return
    cache.add(key)
    old={name:(mode,typ,oid) for mode,typ,oid,name in tree_entries(repo,oldtree,entry_cache)}
    new={name:(mode,typ,oid) for mode,typ,oid,name in tree_entries(repo,newtree,entry_cache)}
    for name,(mode,typ,oid) in old.items():
        full=path+((b'/' if path else b'')+name)
        if purge_matches(full,root,rules):
            if name in new: raise RewriteError(f'purge target remains: {os.fsdecode(full)}')
            continue
        if name not in new: raise RewriteError(f'non-purge path disappeared: {os.fsdecode(full)}')
        nm,nt,noid=new[name]
        if (mode,typ)!=(nm,nt): raise RewriteError(f'non-purge mode/type changed: {os.fsdecode(full)}')
        if typ==b'tree': validate_filtered_tree(repo,oid.decode(),noid.decode(),rules,full,False,cache,entry_cache)
        elif oid!=noid: raise RewriteError(f'non-purge object changed: {os.fsdecode(full)}')
    for name in new.keys()-old.keys():
        full=path+((b'/' if path else b'')+name)
        if purge_matches(full,root,rules): raise RewriteError(f'purge target unexpectedly recreated: {os.fsdecode(full)}')
        raise RewriteError(f'new non-purge path appeared: {os.fsdecode(full)}')

def all_paths_clean(repo, tree, rules, path=b'', root=True, cache=None, entry_cache=None):
    cache=set() if cache is None else cache
    key=(tree,path,root)
    if key in cache: return
    cache.add(key)
    for mode,typ,oid,name in tree_entries(repo,tree,entry_cache):
        full=path+((b'/' if path else b'')+name)
        if purge_matches(full,root,rules): raise RewriteError(f'purge path reachable after rewrite: {os.fsdecode(full)}')
        if typ==b'tree': all_paths_clean(repo,oid.decode(),rules,full,False,cache,entry_cache)


def plan_groups(plan):
    groups=plan.get('groups',[]); msgs=plan.get('messages',{})
    for g in groups:
        if not isinstance(g,dict) or not isinstance(g.get('commits'),list) or len(g['commits'])<2:
            raise RewriteError('each group must contain at least two commits')
        if not isinstance(g.get('message'),str): raise RewriteError('each group needs a message string')
    return groups,msgs

def rewrite(repo, out, plan_path, artifacts):
    repo=Path(repo).resolve(); out=Path(out).resolve(); artifacts=Path(artifacts).resolve()
    if repo==out or repo in out.parents or out in repo.parents: raise RewriteError('output must be a separate directory from the source repo')
    if repo==artifacts or repo in artifacts.parents: raise RewriteError('artifacts directory must not be inside the source repository')
    if artifacts==out or artifacts in out.parents or out in artifacts.parents: raise RewriteError('artifacts and output directories must not overlap')
    if out.exists() and any(out.iterdir()): raise RewriteError(f'output directory must be empty: {out}')
    # All isolation checks happen before creating directories or cloning.
    out.mkdir(parents=True,exist_ok=True); artifacts.mkdir(parents=True,exist_ok=True)
    progress('creating shared mirror clone')
    # Git's shared mirror clone stores objects in the source as an alternate; all writes are local.
    subprocess.run(['git','clone','--mirror','--shared',str(repo),str(out)],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    progress('mirror clone complete; reading refs')
    oldrefs=refs(out); before=refs(repo)
    if oldrefs!=before: raise RewriteError('mirror ref inventory differs from source')
    snapshot=Path(plan_path).resolve().parent/'refs-before.txt'
    if snapshot.exists():
        expected={}
        for line in snapshot.read_text().splitlines():
            bits=line.split()
            if len(bits)>=2:
                # Both git show-ref (OID REF) and for-each-ref (REF OID) snapshots are accepted.
                if bits[0].startswith('refs/'): expected[bits[0]]=bits[1]
                elif bits[1].startswith('refs/'): expected[bits[1]]=bits[0]
        if expected and expected!=before: raise RewriteError('stale source refs: current refs differ from refs-before.txt')
    commits=reachable(out,oldrefs)
    raw_by={}; parents={}; trees={}; author={}; committer={}; msg={}
    children=defaultdict(set)
    for c in commits:
        _,raw=obj(out,c,'commit'); hs,body=split_headers(raw)
        raw_by[c]=raw; parents[c]=parse_parents(hs); trees[c]=one(hs,b'tree').decode()
        author[c]=one(hs,b'author'); committer[c]=one(hs,b'committer'); msg[c]=body
        for p in parents[c]: children[p].add(c)
    with open(plan_path,encoding='utf-8') as plan_file:
        plan=json.load(plan_file)
    groups,msgitems=plan_groups(plan)
    purge_rules=normalize_purge(plan.get('purge',{}))
    progress(f'commit graph indexed: {len(commits)} commits; parsing annotated tags')
    tagoids=tag_object_order(out,oldrefs)
    if any(purge_rules.values()):
        for ref,oid in oldrefs.items():
            if git(out,'cat-file','-t',oid).strip()==b'blob':
                raise RewriteError(f'purge cannot safely map direct blob ref {ref}: blob has no path context')
        for tagoid in tagoids:
            th,_=split_headers(git(out,'cat-file','tag',tagoid))
            if one(th,b'type')==b'blob':
                raise RewriteError(f'purge cannot safely map annotated tag {tagoid} targeting a blob: blob has no path context')
    tagged={}
    for ref,oid in oldrefs.items():
        c=peel_commit(out,oid)
        if c and (ref.startswith('refs/tags/') or ref.endswith(('/HEAD','/main'))): tagged.setdefault(c,[]).append(ref)
    # Any ref tip, including a tag's peeled target, protects a commit from disappearing.
    tips=set()
    for oid in oldrefs.values():
        c=peel_commit(out,oid)
        if c: tips.add(c)
    # Protect the complete v0.13 tagged history (annotated or lightweight tag).
    base=set()
    tag_refs=[(r,o) for r,o in oldrefs.items() if r.startswith('refs/tags/') and r.rsplit('/',1)[-1]=='v0.13']
    if not tag_refs: raise RewriteError('required protected tag refs/tags/v0.13 not found')
    stack=[peel_commit(out,o) for _,o in tag_refs]; stack=[x for x in stack if x]
    while stack:
        c=stack.pop()
        if c in base: continue
        base.add(c); stack.extend(parents[c])
    main_ref='refs/heads/main' if 'refs/heads/main' in oldrefs else 'refs/remotes/origin/main'
    if main_ref not in oldrefs: raise RewriteError('cannot identify main-range tip (refs/heads/main or refs/remotes/origin/main)')
    main_range=set(); todo=[peel_commit(out,oldrefs[main_ref])]; todo=[x for x in todo if x]
    while todo:
        c=todo.pop()
        if c in main_range or c in base: continue
        main_range.add(c); todo.extend(parents[c])
    removed_to={}; anchors=set(); declared=set()
    for gi,g in enumerate(groups):
        seq=g['commits']; seq=[s.lower() for s in seq]
        if len(set(seq))!=len(seq): raise RewriteError(f'group {gi}: duplicate commit')
        for c in seq:
            if c not in commits: raise RewriteError(f'group {gi}: commit not reachable from refs: {c}')
            if c not in main_range: raise RewriteError(f'group {gi}: squash is outside main range: {c}')
            if c in declared: raise RewriteError(f'commit appears in multiple groups: {c}')
            declared.add(c)
        for i,c in enumerate(seq):
            if len(parents[c])!=1: raise RewriteError(f'group {gi}: commit is a merge/root, expected single parent: {c}')
            if i and parents[c][0]!=seq[i-1]: raise RewriteError(f'group {gi}: commits are not parent-adjacent at {c}')
            if c in base: raise RewriteError(f'group {gi}: protected v0.13 history cannot change: {c}')
        anchor=seq[-1]; anchors.add(anchor)
        if len(parents[anchor])!=1: raise RewriteError(f'group {gi}: anchor is a merge')
        for c in seq[:-1]:
            if c in tips: raise RewriteError(f'group {gi}: removed commit is a ref/tag tip: {c}')
            if len(children[c])!=1 or next(iter(children[c]))!=seq[seq.index(c)+1]:
                raise RewriteError(f'group {gi}: removed commit is a fork or has external child: {c}')
            if c in base: raise RewriteError(f'group {gi}: protected commit cannot be removed: {c}')
            removed_to[c]=anchor
        # Enforce common author identity while retaining exact anchor author/committer headers.
        if len({identity_key(author[c]) for c in seq})!=1: raise RewriteError(f'group {gi}: author identities differ')
        # Anchor can be a fork/tag tip, but never a merge (checked above).
    # Topological parent-first ordering independent of date ordering.
    pending=set(commits); order=[]
    while pending:
        ready=sorted(c for c in pending if all(p not in pending for p in parents[c]))
        if not ready: raise RewriteError('commit graph cycle or missing parent')
        order.extend(ready); pending.difference_update(ready)
    mapping={}; removed=set(removed_to)
    filtered_trees={}; tree_records=[]; tree_cache={}; purge_hits={}
    entry_cache={}; hits_cache={}
    filter_total=sum(1 for c in order if c not in removed); filter_done=0
    for c in order:
        if c in removed: continue
        ntree,tree_records=filter_tree(out,trees[c],purge_rules,cache=tree_cache,records=tree_records,entry_cache=entry_cache)
        filtered_trees[c]=ntree
        hits=collect_tree_hits(out,trees[c],purge_rules,hits_cache=hits_cache,entry_cache=entry_cache)
        if hits: purge_hits[c]=list(hits)
        filter_done+=1
        if filter_done%200==0: progress(f'tree filtering: {filter_done}/{filter_total} retained commits')
    progress(f'tree filtering complete: {filter_done} retained commits; {len(entry_cache)} unique tree objects read')
    group_anchor={}
    for g in groups:
        for c in g['commits'][:-1]: group_anchor[c.lower()]=g['commits'][-1].lower()
    groupmsg={g['commits'][-1].lower():g['message'].encode() for g in groups}
    group_parent={g['commits'][-1].lower():parents[g['commits'][0].lower()][0] for g in groups}
    # Explicit message edits may target retained commits in the main range only.
    if isinstance(msgitems,dict): msgitems=[{'commit':k,'message':v} for k,v in msgitems.items()]
    if not isinstance(msgitems,list): raise RewriteError('messages must be a list or object')
    explicit_messages={}
    for item in msgitems:
        if not isinstance(item,dict) or 'commit' not in item or 'message' not in item: raise RewriteError('message entries need commit and message')
        key=item['commit'].lower(); value=item['message']
        if key not in main_range: raise RewriteError(f'message rewrite outside main range: {key}')
        if key in removed: raise RewriteError(f'message rewrite targets removed commit: {key}')
        if key in explicit_messages: raise RewriteError(f'duplicate message rewrite: {key}')
        explicit_messages[key]=value.encode() if isinstance(value,str) else bytes.fromhex(value['hex'])
    for key,value in explicit_messages.items():
        if key in groupmsg and groupmsg[key]!=value: raise RewriteError(f'conflicting group and explicit message for {key}')
        groupmsg[key]=value
    progress(f'commit rewrite starting: {len(order)} commits, {len(removed)} removed')
    rewrite_done=0
    for c in order:
        if c in removed:
            continue
        hs,body=split_headers(raw_by[c]); oldp=parents[c]
        # The retained anchor takes the first removed node's parent, collapsing the whole chain.
        effective_oldp=[group_parent[c]] if c in group_parent else oldp
        newp=[mapping[p] for p in effective_oldp]
        changed_parent=newp!=oldp
        newbody=groupmsg.get(c,body)
        newtree=filtered_trees[c]
        changed_tree=newtree!=trees[c]
        changed=changed_parent or changed_tree or newbody!=body
        if changed and signed_payload(raw_by[c]): raise RewriteError(f'cannot rewrite signed commit object {c} (gpgsig/mergetag/signature detected)')
        if c in base and newbody!=body: raise RewriteError(f'protected v0.13 commit message would change: {c}')
        if changed:
            j=0; rebuilt=[]
            for h in hs:
                if h.startswith(b'tree '): rebuilt.append(b'tree '+newtree.encode())
                elif h.startswith(b'parent '): rebuilt.append(b'parent '+newp[j].encode()); j+=1
                else: rebuilt.append(h)
            newraw=b'\n'.join(rebuilt)+b'\n\n'+newbody
            mapping[c]=hashobj(out,'commit',newraw)
        else: mapping[c]=c
        rewrite_done+=1
        if rewrite_done%200==0: progress(f'commit rewrite: {rewrite_done}/{len(commits)-len(removed)} retained commits')
    progress(f'commit rewrite complete: {rewrite_done} retained commits')
    for gone,anchor in removed_to.items():
        if anchor not in mapping: raise RewriteError(f'squash anchor was not mapped: {anchor}')
        mapping[gone]=mapping[anchor]
    # Rebuild nested annotated tags from inner to outer; preserve every byte except target oid.
    # Direct tree refs and annotated tags targeting trees also need a root-context mapping.
    tree_mapping={}
    tree_object_ids={oid for oid in oldrefs.values() if git(out,'cat-file','-t',oid).strip()==b'tree'}
    for t in tagoids:
        th,_=split_headers(git(out,'cat-file','tag',t))
        if one(th,b'type')==b'tree': tree_object_ids.add(one(th,b'object').decode())
    for oldtree in sorted(tree_object_ids):
        newtree,tree_records=filter_tree(out,oldtree,purge_rules,cache=tree_cache,records=tree_records,entry_cache=entry_cache)
        tree_mapping[oldtree]=newtree
    tree_ref_hits=[]
    for ref,oldtree in sorted((r,o) for r,o in oldrefs.items() if git(out,'cat-file','-t',o).strip()==b'tree'):
        tree_ref_hits.append({'endpoint':ref,'kind':'direct-tree-ref','old_tree':oldtree,'new_tree':tree_mapping.get(oldtree,oldtree),'removed_paths':list(collect_tree_hits(out,oldtree,purge_rules,hits_cache=hits_cache,entry_cache=entry_cache))})
    for oldtag in sorted(tagoids):
        th,_=split_headers(git(out,'cat-file','tag',oldtag))
        if one(th,b'type')==b'tree':
            oldtree=one(th,b'object').decode()
            tree_ref_hits.append({'endpoint':oldtag,'kind':'annotated-tag-target-tree','old_tree':oldtree,'new_tree':tree_mapping.get(oldtree,oldtree),'removed_paths':list(collect_tree_hits(out,oldtree,purge_rules,hits_cache=hits_cache,entry_cache=entry_cache))})
    progress(f'tree refs mapped: {len(tree_mapping)} direct tree endpoints; rebuilding {len(tagoids)} annotated tag objects')
    tagmap={}; left=set(tagoids)
    while left:
        tag_progress=False
        for t in list(left):
            _,raw=obj(out,t,'tag'); hs,body=split_headers(raw); target=one(hs,b'object').decode()
            typ=one(hs,b'type').decode()
            if typ=='tag' and target not in tagmap: continue
            newtarget=tagmap.get(target,mapping.get(target,tree_mapping.get(target,target)))
            if newtarget!=target:
                if signed_payload(raw): raise RewriteError(f'cannot retarget signed annotated tag {t}')
                nh=[]
                for h in hs: nh.append(b'object '+newtarget.encode() if h.startswith(b'object ') else h)
                tagmap[t]=hashobj(out,'tag',b'\n'.join(nh)+b'\n\n'+body)
            else: tagmap[t]=t
            left.remove(t); tag_progress=True
        if not tag_progress: raise RewriteError('cyclic or unresolved annotated tag chain')
    progress(f'annotated tag mapping complete: {len(tagmap)} tags')
    def mapped(oid): return tagmap.get(oid,mapping.get(oid,tree_mapping.get(oid,oid)))
    # Independent preflight validation before atomically moving any mirror refs.
    tree_validation_cache=set(); clean_validation_cache=set(); validation_done=0
    progress('independent retained commit/tree validation starting')
    for c in commits:
        m=mapping[c]
        if c in removed:
            if c not in removed_to: raise RewriteError(f'undeclared removed commit: {c}')
            continue
        _,nr=obj(out,m,'commit'); nh,nb=split_headers(nr)
        if one(nh,b'tree').decode()!=filtered_trees[c]: raise RewriteError(f'filtered tree mismatch for retained commit {c}')
        validate_filtered_tree(out,trees[c],filtered_trees[c],purge_rules,cache=tree_validation_cache,entry_cache=entry_cache)
        all_paths_clean(out,filtered_trees[c],purge_rules,cache=clean_validation_cache,entry_cache=entry_cache)
        validation_done+=1
        if validation_done%200==0: progress(f'independent validation: {validation_done}/{len(commits)-len(removed)} retained commits')
        if one(nh,b'author')!=author[c] or one(nh,b'committer')!=committer[c]: raise RewriteError(f'identity/date changed for retained commit {c}')
        source_parents=[group_parent[c]] if c in group_parent else parents[c]
        expect=[mapping[p] for p in source_parents]
        if parse_parents(nh)!=expect: raise RewriteError(f'parent mapping mismatch for {c}')
        if len(expect)>1 and len(set(expect))!=len(expect): raise RewriteError(f'mapped parents collapse a merge at {c}')
        if len(children[c])>1 and c in removed: raise RewriteError(f'fork node was removed: {c}')
    progress(f'independent tree validation complete: {validation_done} retained commits, {len(tree_validation_cache)} old/new subtree pairs')
    for c in base:
        if c in group_anchor or c in explicit_messages: raise RewriteError(f'protected v0.13 commit message/topology edit is forbidden: {c}')
        if len(parse_parents(split_headers(git(out,'cat-file','commit',mapping[c]))[0]))!=len(parents[c]): raise RewriteError(f'v0.13 parent topology changed: {c}')
    for c in removed:
        if c not in removed_to: raise RewriteError(f'removed node lacks declared group: {c}')
    # Ref endpoints should preserve object kind, and commit endpoint trees.
    newrefs={r:mapped(o) for r,o in oldrefs.items()}
    for r,old in oldrefs.items():
        new=newrefs[r]; ot=git(out,'cat-file','-t',old).strip(); nt=git(out,'cat-file','-t',new).strip()
        if ot!=nt: raise RewriteError(f'ref object type changed: {r}')
        oc=peel_commit(out,old); nc=peel_commit(out,new)
        if oc and nc and filtered_trees[oc]!=one(split_headers(git(out,'cat-file','commit',nc))[0],b'tree').decode(): raise RewriteError(f'ref endpoint tree differs from purge result: {r}')
        if ot==b'tree' and new!=tree_mapping.get(old,old): raise RewriteError(f'direct tree ref differs from purge result: {r}')
        if ot==b'tree': all_paths_clean(out,new,purge_rules,cache=clean_validation_cache,entry_cache=entry_cache)
    # Ensure the source still matches the captured state before mutating mirror refs.
    if refs(repo)!=before: raise RewriteError('source refs changed during preview')
    # Atomic update-ref transaction with expected old OIDs. No source refs are touched.
    commands=['start']
    for r,old in oldrefs.items():
        new=newrefs[r]
        if new!=old: commands.append(f'update {r} {new} {old}')
        else: commands.append(f'verify {r} {old}')
    commands += ['prepare','commit']
    progress(f'updating {len(oldrefs)} mirror refs atomically')
    git(out,'update-ref','--stdin',input=('\n'.join(commands)+'\n').encode())
    if refs(out)!=newrefs: raise RewriteError('post-update ref inventory/OIDs differ from mapped refs')
    if refs(repo)!=before: raise RewriteError('source refs changed during preview')
    progress('mirror refs updated; writing artifacts')
    # Artifact maps and manifests.
    artifacts.mkdir(parents=True,exist_ok=True)
    with open(artifacts/'map.json','w') as f: json.dump({'commits':mapping,'tags':tagmap,'refs':newrefs},f,indent=2,sort_keys=True); f.write('\n')
    with open(artifacts/'tree-map.json','w') as f:
        json.dump({'commit_trees':{c:{'old':trees[c],'new':filtered_trees[c]} for c in sorted(filtered_trees)},'direct_tree_objects':tree_mapping,'tree_objects':tree_records},f,indent=2,sort_keys=True); f.write('\n')
    with open(artifacts/'purge-hits.json','w') as f:
        json.dump({'targets':purge_rules,'hit_commit_count':len(purge_hits),'hit_path_occurrences':sum(len(v) for v in purge_hits.values()),'commits':purge_hits,'tree_ref_hits':tree_ref_hits,'tree_ref_hit_count':sum(bool(x['removed_paths']) for x in tree_ref_hits),'tree_ref_hit_path_occurrences':sum(len(x['removed_paths']) for x in tree_ref_hits)},f,indent=2,sort_keys=True); f.write('\n')
    with open(artifacts/'map.tsv','w') as f:
        f.write('kind\told\tnew\n')
        for c,m in sorted(mapping.items()): f.write(f'commit\t{c}\t{m}\n')
        for c,m in sorted(tagmap.items()): f.write(f'tag\t{c}\t{m}\n')
    with open(artifacts/'retained-removed.json','w') as f:
        retained={}
        for c in sorted(commits-removed):
            retained[c]={'mapped':mapping[c],'tree':trees[c],'mapped_tree':filtered_trees[c],'purge_hits':purge_hits.get(c,[]),'parents':parents[c], 'author':author[c].decode('utf-8','surrogateescape'), 'committer':committer[c].decode('utf-8','surrogateescape'), 'message':msg[c].decode('utf-8','surrogateescape')}
        removed_meta={}
        for c in sorted(removed_to):
            removed_meta[c]={'anchor':removed_to[c],'tree':trees[c],'parents':parents[c], 'author':author[c].decode('utf-8','surrogateescape'), 'committer':committer[c].decode('utf-8','surrogateescape'), 'message':msg[c].decode('utf-8','surrogateescape')}
        json.dump({'retained':retained,'removed':removed_meta},f,indent=2,sort_keys=True); f.write('\n')
    # Verification material includes source ref OIDs to permit an external comparison.
    validation={'ok':True,'source_refs_unchanged_checked':True,'commit_count':len(commits),'removed_count':len(removed),'retained_count':len(commits)-len(removed),'ref_count':len(oldrefs),'tag_object_count':len(tagoids),'protected_v0.13_commit_count':len(base),'purge_hit_commit_count':len(purge_hits),'purge_hit_path_occurrences':sum(len(v) for v in purge_hits.values()),'tree_ref_hit_count':sum(bool(x['removed_paths']) for x in tree_ref_hits),'tree_ref_hit_path_occurrences':sum(len(x['removed_paths']) for x in tree_ref_hits),'checks':['ref inventory mapped atomically','ref endpoint object types and filtered trees','all retained trees independently filtered and non-target entries byte-identical','retained commit raw author/committer headers','parent mapping/order and merge cardinality','fork nodes retained','v0.13 message/topology preserved; only purge trees and propagated parent OIDs may change','removed commits declared']}
    with open(artifacts/'validation.json','w') as f: json.dump(validation,f,indent=2); f.write('\n')
    with open(artifacts/'refs-source.tsv','w') as f:
        f.write('ref\told\tnew\n')
        for r,o in sorted(oldrefs.items()): f.write(f'{r}\t{o}\t{newrefs[r]}\n')
    progress('preview complete')
    return validation

def progress(message): print(f'[engine] {message}',file=sys.stderr,flush=True)

def main():
    ap=argparse.ArgumentParser(description=__doc__); sub=ap.add_subparsers(dest='cmd',required=True)
    p=sub.add_parser('preview'); p.add_argument('--repo',required=True); p.add_argument('--output',required=True); p.add_argument('--plan',required=True); p.add_argument('--artifacts',required=True)
    a=ap.parse_args()
    try:
        result=rewrite(a.repo,a.output,a.plan,a.artifacts); print(json.dumps(result,indent=2)); return 0
    except Exception as e:
        print(f'preview failed: {e}',file=sys.stderr); return 2
if __name__=='__main__': raise SystemExit(main())
