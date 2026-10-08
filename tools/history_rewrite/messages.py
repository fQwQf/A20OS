#!/usr/bin/env python3
"""Preview message-only Git history changes without touching the source refs."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from collections import Counter
from pathlib import Path


class RewriteError(RuntimeError):
    pass


def git(repo, *args, data=None):
    proc = subprocess.run(['git', '-C', str(repo), *args], input=data,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode:
        raise RewriteError(proc.stderr.decode(errors='replace').strip())
    return proc.stdout


def refs(repo):
    return dict(line.split(' ', 1) for line in git(
        repo, 'for-each-ref', '--format=%(refname) %(objectname)').decode().splitlines())


def raw_object(repo, oid):
    kind = git(repo, 'cat-file', '-t', oid).decode().strip()
    return kind, git(repo, 'cat-file', kind, oid)


def split(raw):
    head, sep, body = raw.partition(b'\n\n')
    if not sep:
        raise RewriteError('commit/tag missing header separator')
    return head.split(b'\n'), body


def values(headers, key):
    return [h[len(key) + 1:].decode() for h in headers if h.startswith(key + b' ')]


def signed(raw):
    return any(marker in raw for marker in (
        b'gpgsig ', b'mergetag ', b'-----BEGIN PGP SIGNATURE-----',
        b'-----BEGIN SSH SIGNATURE-----'))


TITLE = re.compile(r'^(feat|fix|refactor|perf|docs|test|build|ci|chore|revert|merge)\([a-z0-9][a-z0-9._-]*\): [a-z][\x20-\x7e]*$')
FALSE_COAUTHOR = re.compile(r'(?im)^co-authored-by\s*:.*(?:claude|anthropic|opus|sisyphus).*$')
COAUTHOR = re.compile(r'(?im)^co-authored-by\s*:.*$')


def check_message(message, parent_count):
    if not isinstance(message, str) or not message:
        raise RewriteError('message must be a nonempty UTF-8 string')
    lines = message.splitlines()
    title = lines[0]
    if not TITLE.fullmatch(title) or len(title) > 100 or title.endswith('.'):
        raise RewriteError(f'invalid normalized title: {title}')
    if (parent_count > 1) != title.startswith('merge('):
        raise RewriteError(f'merge type and parent count disagree: {title}')
    if message != message.rstrip() + '\n' or '\r' in message or '\0' in message:
        raise RewriteError(f'noncanonical message ending: {title}')
    if len(lines) > 1 and lines[1] != '':
        raise RewriteError(f'missing blank line after subject: {title}')
    if any(line.rstrip() != line for line in lines):
        raise RewriteError(f'trailing whitespace: {title}')
    if FALSE_COAUTHOR.search(message):
        raise RewriteError(f'false coauthor remains: {title}')


def validate(source, preview, mapping, messages, oldrefs):
    """Read both repositories afresh; verify exact headers, bodies and graph edges."""
    cmap = mapping['commits']
    tmap = mapping['tags']
    if len(set(cmap.values())) != len(cmap):
        collisions = {}
        for old, new in cmap.items():
            collisions.setdefault(new, []).append(old)
        raise RewriteError(f'commit identities collapsed: {[v for v in collisions.values() if len(v) > 1]}')
    order = git(source, 'rev-list', '--all', '--topo-order', '--reverse').decode().splitlines()
    new_order = set(git(preview, 'rev-list', '--all').decode().splitlines())
    if set(order) != set(cmap) or new_order != set(cmap.values()):
        raise RewriteError('commit coverage mismatch')
    oldchildren = Counter()
    newchildren = Counter()
    merge_count = 0
    removed_trailers = 0
    body_edits = 0
    subject_edits = 0
    for old in order:
        kind, oldraw = raw_object(source, old)
        newkind, newraw = raw_object(preview, cmap[old])
        if kind != 'commit' or newkind != 'commit':
            raise RewriteError('commit type changed')
        oh, ob = split(oldraw)
        nh, nb = split(newraw)
        expected = [b'parent ' + cmap[h[7:].decode()].encode() if h.startswith(b'parent ') else h for h in oh]
        if nh != expected:
            raise RewriteError(f'non-message headers changed: {old}')
        # Headers include exact author/committer identity, Unix times, timezone,
        # tree, encoding, and all other original metadata, in original order.
        if nb != messages[old].encode():
            raise RewriteError(f'message differs from reviewed plan: {old}')
        parents = values(oh, b'parent')
        check_message(messages[old], len(parents))
        merge_count += len(parents) > 1
        oldchildren.update(parents)
        newchildren.update(values(nh, b'parent'))
        before = ob.decode('utf-8')
        removed_trailers += len(FALSE_COAUTHOR.findall(before))
        allowed = [line for line in COAUTHOR.findall(before) if not FALSE_COAUTHOR.fullmatch(line)]
        if Counter(allowed) != Counter(COAUTHOR.findall(messages[old])):
            raise RewriteError(f'unrelated coauthors changed: {old}')
        subject_edits += before.split('\n', 1)[0] != messages[old].split('\n', 1)[0]
        body_edits += before.partition('\n')[2] != messages[old].partition('\n')[2]
    if {cmap[o]: n for o, n in oldchildren.items()} != dict(newchildren):
        raise RewriteError('fork graph changed')
    for old, new in tmap.items():
        _, oldraw = raw_object(source, old)
        _, newraw = raw_object(preview, new)
        oh, ob = split(oldraw)
        nh, nb = split(newraw)
        expected = [b'object ' + mapping['objects'][h[7:].decode()].encode() if h.startswith(b'object ') else h for h in oh]
        if nh != expected or ob != nb:
            raise RewriteError(f'tag metadata/message changed: {old}')
    expected_refs = {ref: mapping['objects'].get(oid, oid) for ref, oid in oldrefs.items()}
    if refs(preview) != expected_refs or refs(source) != oldrefs:
        raise RewriteError('reference inventory changed unexpectedly')
    git(preview, 'fsck', '--full', '--no-dangling')
    return {'ok': True, 'commits': len(cmap), 'merges': merge_count,
            'forks': sum(n > 1 for n in oldchildren.values()),
            'annotated_tags': len(tmap), 'refs': len(oldrefs),
            'subjects_reworded': subject_edits, 'bodies_changed': body_edits,
            'false_coauthor_lines_removed': removed_trailers,
            'trees_identical': True, 'raw_author_committer_headers_identical': True,
            'parent_order_identical_under_mapping': True,
            'tag_metadata_and_messages_identical': True,
            'other_coauthors_preserved': True, 'source_refs_unchanged': True}


def preview(source, output, plan, artifacts):
    source, output, artifacts = (Path(p).resolve() for p in (source, output, artifacts))
    for a, b in ((source, output), (source, artifacts), (output, artifacts)):
        if a == b or a in b.parents or b in a.parents:
            raise RewriteError('source, preview and artifacts must be separate directories')
    if output.exists():
        raise RewriteError('preview output must not exist')
    oldrefs = refs(source)
    messages = json.loads(Path(plan).read_text())
    order = git(source, 'rev-list', '--all', '--topo-order', '--reverse').decode().splitlines()
    if set(messages) != set(order):
        raise RewriteError('message plan must cover each reachable commit exactly once')
    for oid in order:
        _, raw = raw_object(source, oid)
        headers, body = split(raw)
        check_message(messages[oid], len(values(headers, b'parent')))
        body.decode('utf-8')  # Refuse ambiguous legacy encodings rather than silently altering text.
        if signed(raw):
            raise RewriteError(f'signed commit cannot be rewritten without invalidating its signature: {oid}')
    # Copy objects rather than depending on alternates or hard links to the source.
    git(source, 'clone', '--mirror', '--no-hardlinks', str(source), str(output))
    if refs(output) != oldrefs:
        raise RewriteError('clone refs do not match source snapshot')
    mapping = {'commits': {}, 'tags': {}, 'objects': {}}
    for oid in order:
        _, raw = raw_object(source, oid)
        headers, _ = split(raw)
        newheaders = [b'parent ' + mapping['commits'][h[7:].decode()].encode() if h.startswith(b'parent ') else h for h in headers]
        newraw = b'\n'.join(newheaders) + b'\n\n' + messages[oid].encode()
        newoid = git(output, 'hash-object', '-w', '-t', 'commit', '--stdin', data=newraw).decode().strip()
        mapping['commits'][oid] = newoid
        mapping['objects'][oid] = newoid
    def map_object(oid):
        if oid in mapping['objects']:
            return mapping['objects'][oid]
        kind, raw = raw_object(source, oid)
        if kind == 'tag':
            if signed(raw):
                raise RewriteError(f'signed tag cannot be rewritten: {oid}')
            headers, body = split(raw)
            target, = values(headers, b'object')
            newtarget = map_object(target)
            newheaders = [b'object ' + newtarget.encode() if h.startswith(b'object ') else h for h in headers]
            newraw = b'\n'.join(newheaders) + b'\n\n' + body
            newoid = git(output, 'hash-object', '-w', '-t', 'tag', '--stdin', data=newraw).decode().strip()
            mapping['tags'][oid] = newoid
        elif kind in ('tree', 'blob'):
            newoid = oid
        else:
            raise RewriteError(f'unexpected endpoint type {kind}: {oid}')
        mapping['objects'][oid] = newoid
        return newoid
    newrefs = {ref: map_object(oid) for ref, oid in oldrefs.items()}
    transaction = 'start\n' + ''.join(f'update {ref} {newrefs[ref]} {oid}\n' for ref, oid in oldrefs.items()) + 'prepare\ncommit\n'
    git(output, 'update-ref', '--stdin', data=transaction.encode())
    artifacts.mkdir(parents=True)
    (artifacts / 'map.json').write_text(json.dumps(mapping, indent=2) + '\n')
    (artifacts / 'refs-before.json').write_text(json.dumps(oldrefs, indent=2) + '\n')
    (artifacts / 'refs-after.json').write_text(json.dumps(newrefs, indent=2) + '\n')
    result = validate(source, output, mapping, messages, oldrefs)
    (artifacts / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--plan', required=True)
    parser.add_argument('--artifacts', required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(preview(args.repo, args.output, args.plan, args.artifacts), indent=2))
    except (RewriteError, OSError, ValueError) as exc:
        parser.exit(1, f'{exc}\n')
