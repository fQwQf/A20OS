#!/usr/bin/env python3
"""Preview exact Git identity corrections, preserving dates, trees and messages."""
from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path

from messages import RewriteError, git, raw_object, refs, signed, split, values


def identity_header(header, corrections):
    key, sep, raw = header.partition(b' ')
    if key not in (b'author', b'committer'):
        return header
    identity, timestamp, zone = raw.rsplit(b' ', 2)
    replacement = corrections.get(identity)
    return key + sep + (replacement if replacement is not None else identity) + b' ' + timestamp + b' ' + zone


def load_plan(path):
    plan = json.loads(Path(path).read_text())
    if set(plan) != {'identities'} or not isinstance(plan['identities'], dict) or not plan['identities']:
        raise RewriteError('plan must contain a nonempty identities mapping')
    corrections = {}
    for old, new in plan['identities'].items():
        if not all(isinstance(x, str) and re.fullmatch(r'[^<>\r\n\0]+ <[^<>\r\n\0]+>', x) for x in (old, new)):
            raise RewriteError('identities must be exact Name <email> strings without dates')
        corrections[old.encode()] = new.encode()
    return corrections


def preview(source, output, plan, artifacts):
    source, output, artifacts = (Path(p).resolve() for p in (source, output, artifacts))
    for a, b in ((source, output), (source, artifacts), (output, artifacts)):
        if a == b or a in b.parents or b in a.parents:
            raise RewriteError('source, preview and artifacts must be separate directories')
    if output.exists():
        raise RewriteError('preview output must not exist')
    if artifacts.exists():
        raise RewriteError('artifacts output must not exist')
    corrections = load_plan(plan)
    before = refs(source)
    order = git(source, 'rev-list', '--all', '--topo-order', '--reverse').decode().splitlines()
    originals = {}
    matches = Counter()
    for oid in order:
        _, raw = raw_object(source, oid)
        headers, body = split(raw)
        if signed(raw):
            raise RewriteError(f'signed commit cannot be rewritten: {oid}')
        originals[oid] = (headers, body)
        for header in headers:
            if identity_header(header, corrections) != header:
                matches[header.split(b' ', 1)[0].decode()] += 1
    if not matches:
        raise RewriteError('identity plan matches no author or committer headers')
    git(source, 'clone', '--mirror', '--no-hardlinks', str(source), str(output))
    if refs(output) != before:
        raise RewriteError('source refs changed during clone')
    mapping = {'commits': {}, 'tags': {}, 'objects': {}}
    for oid in order:
        headers, body = originals[oid]
        replaced = []
        for header in headers:
            if header.startswith(b'parent '):
                replaced.append(b'parent ' + mapping['commits'][header[7:].decode()].encode())
            else:
                replaced.append(identity_header(header, corrections))
        raw = b'\n'.join(replaced) + b'\n\n' + body
        new = git(output, 'hash-object', '-w', '-t', 'commit', '--stdin', data=raw).decode().strip()
        mapping['commits'][oid] = mapping['objects'][oid] = new
    if len(set(mapping['commits'].values())) != len(order):
        raise RewriteError('identity correction would collapse existing commit nodes')
    def map_object(oid):
        if oid in mapping['objects']:
            return mapping['objects'][oid]
        kind, raw = raw_object(source, oid)
        if kind == 'tag':
            if signed(raw):
                raise RewriteError(f'signed tag cannot be rewritten: {oid}')
            headers, body = split(raw)
            target, = values(headers, b'object')
            replaced = [b'object ' + map_object(target).encode() if h.startswith(b'object ') else h for h in headers]
            new = git(output, 'hash-object', '-w', '-t', 'tag', '--stdin', data=b'\n'.join(replaced)+b'\n\n'+body).decode().strip()
            mapping['tags'][oid] = new
        elif kind in ('tree', 'blob'):
            new = oid
        else:
            raise RewriteError(f'unknown endpoint {oid} of type {kind}')
        mapping['objects'][oid] = new
        return new
    after = {ref: map_object(oid) for ref, oid in before.items()}
    command = 'start\n' + ''.join(f'update {ref} {after[ref]} {old}\n' for ref, old in before.items()) + 'prepare\ncommit\n'
    git(output, 'update-ref', '--stdin', data=command.encode())
    result = validate(source, output, mapping, corrections, before)
    artifacts.mkdir(parents=True)
    for name, data in [('map', mapping), ('refs-before', before), ('refs-after', after), ('validation', result)]:
        (artifacts / (name + '.json')).write_text(json.dumps(data, indent=2) + '\n')
    return result


def validate(source, output, mapping, corrections, before):
    cmap = mapping['commits']
    if len(set(cmap.values())) != len(cmap):
        raise RewriteError('commit mapping is not one-to-one')
    if set(git(source, 'rev-list', '--all').decode().splitlines()) != set(cmap):
        raise RewriteError('source commit coverage changed')
    if set(git(output, 'rev-list', '--all').decode().splitlines()) != set(cmap.values()):
        raise RewriteError('rewritten graph coverage differs')
    counts = Counter()
    children = Counter()
    for oid, new in cmap.items():
        _, oldraw = raw_object(source, oid)
        _, newraw = raw_object(output, new)
        oh, ob = split(oldraw)
        nh, nb = split(newraw)
        if ob != nb or len(oh) != len(nh):
            raise RewriteError(f'message or header cardinality changed: {oid}')
        parents = values(oh, b'parent')
        children.update(parents)
        counts['merges'] += len(parents) > 1
        for oldheader, newheader in zip(oh, nh):
            key, _, oldvalue = oldheader.partition(b' ')
            newkey, _, newvalue = newheader.partition(b' ')
            if key != newkey:
                raise RewriteError(f'header order/type changed: {oid}')
            if key == b'parent':
                if newvalue.decode() != cmap[oldvalue.decode()]:
                    raise RewriteError(f'parent order/mapping changed: {oid}')
            elif key in (b'author', b'committer'):
                oldidentity, oldtime, oldzone = oldvalue.rsplit(b' ', 2)
                newidentity, newtime, newzone = newvalue.rsplit(b' ', 2)
                if (oldtime, oldzone) != (newtime, newzone):
                    raise RewriteError(f'original timestamp/timezone changed: {oid}')
                if newidentity != corrections.get(oldidentity, oldidentity):
                    raise RewriteError(f'unplanned identity changed: {oid}')
                counts[key.decode() + '_corrected'] += newidentity != oldidentity
            elif oldheader != newheader:
                raise RewriteError(f'non-identity header changed: {oid}')
    for oid, new in mapping['tags'].items():
        _, oldraw = raw_object(source, oid)
        _, newraw = raw_object(output, new)
        oh, ob = split(oldraw)
        nh, nb = split(newraw)
        expected = [b'object '+mapping['objects'][h[7:].decode()].encode() if h.startswith(b'object ') else h for h in oh]
        if expected != nh or ob != nb:
            raise RewriteError(f'tag metadata or message changed: {oid}')
    if refs(output) != {ref: mapping['objects'][old] for ref, old in before.items()} or refs(source) != before:
        raise RewriteError('reference mapping or source snapshot differs')
    git(output, 'fsck', '--full', '--no-dangling')
    return {'ok': True, 'commits': len(cmap), 'commits_rehashed': sum(a != b for a, b in cmap.items()),
            **dict(counts), 'forks': sum(n > 1 for n in children.values()),
            'annotated_tags': len(mapping['tags']), 'refs': len(before),
            'author_and_committer_dates_and_timezones_identical': True,
            'messages_and_trees_identical': True, 'other_identities_unchanged': True,
            'parent_order_and_topology_preserved': True, 'tag_metadata_preserved': True,
            'source_refs_unchanged': True}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'output', 'plan', 'artifacts'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(preview(args.repo, args.output, args.plan, args.artifacts), indent=2))
    except (RewriteError, ValueError, OSError) as exc:
        parser.exit(1, str(exc) + '\n')
