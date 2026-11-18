#!/usr/bin/env python3
"""Prepare audited core/test history mirrors without changing the source repo.

Only new repositories under --output are rewritten. Nothing is force-pushed,
no source ref/index/worktree is changed, and backups remain outside the core.
Requires git-filter-repo. Run after reviewing the test ownership manifest.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
from pathlib import Path
import subprocess
import sys


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def refs(repo):
    result = {}
    for line in git(repo, 'for-each-ref', '--format=%(refname) %(objectname) %(objecttype)',
                    'refs/heads', 'refs/tags', 'refs/remotes', 'refs/stash').splitlines():
        name, oid, kind = line.split()
        result[name] = {'object': oid, 'kind': kind,
                        'commit': git(repo, 'rev-parse', name + '^{commit}')}
    return result


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def prepare(source: Path, output: Path, manifest: Path, branch: str):
    source = source.resolve()
    output = output.resolve()
    if output.exists():
        raise ValueError(f'output must not exist: {output}')
    if output == source or source in output.parents:
        raise ValueError('put history mirrors/backups outside the source checkout')
    run('git', 'filter-repo', '--version')
    data = json.loads(manifest.read_text())
    old_refs = refs(source)
    head = git(source, 'rev-parse', f'refs/heads/{branch}')
    current = set(git(source, 'ls-tree', '-r', '--name-only', head).splitlines())
    historical = set(git(source, 'log', '--all', '--format=', '--name-only').splitlines())
    prefixes = [p.rstrip('/') + '/' for p in data['directory_prefixes']]
    historical_removes = set(data.get('historical_remove_paths', []))
    explicit = set(data['paths']) | historical_removes
    # Include prior locations of the same tools. Do not match generic names
    # such as README.md, __init__.py or a still-owned production path.
    basenames = {Path(p).name for p in data['paths'] if Path(p).name not in
                 {'README.md', '__init__.py', 'Makefile', 'sources.mk'}}
    aliases = sorted(p for p in historical
                     if p.startswith('tools/') and p not in current
                     and Path(p).name in basenames)
    explicit.update(aliases)
    # Historical host harnesses lived directly under tools before regrouping.
    explicit.update(p for p in historical if p.startswith('tools/') and
                    len(Path(p).parts) == 2 and
                    (fnmatch.fnmatch(Path(p).name, 'test-*-host.*') or
                     fnmatch.fnmatch(Path(p).name, 'test_*_host.*')))
    specs = sorted(set(prefixes) | explicit)
    if any('\n' in p or p.startswith(('glob:', 'regex:', 'literal:')) for p in specs):
        raise ValueError('unsupported manifest path')

    def is_historical_remove(p):
        return p in historical_removes or any(p.startswith(d.rstrip('/') + '/') for d in historical_removes)

    def removed(p):
        return (p in explicit or
                any(p.startswith(d) for d in prefixes) or
                any(p.startswith(d.rstrip('/') + '/') for d in explicit))

    output.mkdir(parents=True)
    write(output/'source-refs.json', old_refs)
    write(output/'ownership.json', {'paths': specs, 'historical_aliases': aliases,
                                  'source_head': head, 'source_branch': branch})
    path_list = output/'removed-paths.txt'
    path_list.write_text('\n'.join(specs) + '\n')
    # Bundle all named history before filtering. It deliberately retains old
    # objects, lives outside the core and is never a ref in the rewritten repo.
    run('git', '-C', source, 'bundle', 'create', output/'before.bundle', '--all')
    core = output/'A20OS.git'
    run('git', 'clone', '--mirror', '--no-local', source, core)
    # App diff captures may be tree refs, rather than publishable commits.
    # Keep them in the source, but do not retain their old test blobs here.
    excluded = []
    for name in git(core, 'for-each-ref', '--format=%(refname)').splitlines():
        if name not in old_refs:
            excluded.append(name)
            run('git', '-C', core, 'update-ref', '-d', name)
    write(output/'excluded-local-refs.json', excluded)
    run('git', '-C', core, 'gc', '--prune=now')
    before_size = git(core, 'count-objects', '-vH')
    # Keep empty commits: every old tagged commit has a direct new counterpart.
    # --no-local avoids shared object storage with the source or its worktrees.
    run('git', '-C', core, 'filter-repo', '--force', '--invert-paths',
        '--paths-from-file', path_list, '--partial', '--prune-empty', 'never',
        '--prune-degenerate', 'never')
    run('git', '-C', core, 'config', '--remove-section', 'remote.origin')
    run('git', '-C', core, 'reflog', 'expire', '--expire=now', '--all')
    mapping = dict(line.split() for line in (core/'filter-repo/commit-map').read_text().splitlines()[1:])
    after_refs = refs(core)
    if set(old_refs) != set(after_refs):
        raise ValueError(f'ref names changed: {set(old_refs)^set(after_refs)}')
    tag_report = []
    for name, old in old_refs.items():
        new = after_refs[name]
        expected = mapping.get(old['commit'], old['commit'])
        if new['commit'] != expected or new['kind'] != old['kind']:
            raise ValueError(f'ref did not map to its rewritten commit: {name}')
        if name.startswith('refs/tags/'):
            old_tree = git(source, 'ls-tree', '-r', '--full-tree', name).splitlines()
            expected_tree = [line for line in old_tree if not removed(line.split('\t', 1)[1])]
            actual_tree = git(core, 'ls-tree', '-r', '--full-tree', name).splitlines()
            if expected_tree != actual_tree:
                raise ValueError(f'tag tree differs beyond the removed paths: {name}')
            signed = old['kind'] == 'tag' and '-----BEGIN ' in git(source, 'cat-file', '-p', old['object'])
            tag_report.append({'tag': name[10:], 'type': old['kind'],
                               'old_commit': old['commit'], 'new_commit': new['commit'],
                               'old_object': old['object'], 'new_object': new['object'],
                               'needs_resigning': signed})
    # Inspect every surviving historical path, not just HEAD or one blob alias.
    surviving = set(git(core, 'log', '--all', '--format=', '--name-only').splitlines())
    leaked = sorted(p for p in surviving if removed(p))
    if leaked:
        raise ValueError(f'paths remain in rewritten history: {leaked[:20]}')
    run('git', '-C', core, 'fsck', '--full', '--no-reflogs')
    run('git', '-C', core, 'gc', '--prune=now')
    write(output/'tags.json', tag_report)
    write(output/'rewritten-refs.json', after_refs)

    # The test project gets the selected branch's relevant history. Internal
    # backup branches, stash tags and OS release tags are not published in it.
    suite = output/'a20-tests.git'
    run('git', 'init', '--bare', '--initial-branch=main', suite)
    run('git', '-C', suite, 'fetch', '--no-tags', str(source),
        f'refs/heads/{branch}:refs/heads/main')
    test_path_list = output/'test-paths.txt'
    test_path_list.write_text('\n'.join(p for p in specs if not is_historical_remove(p)) + '\n')
    run('git', '-C', suite, 'filter-repo', '--force', '--paths-from-file', test_path_list)
    run('git', '-C', suite, 'fsck', '--full', '--no-reflogs')
    # Prove that preparing the rewrite did not touch any source refs.
    if refs(source) != old_refs:
        raise ValueError('source refs changed while preparing; do not publish these mirrors')
    report = {'source': str(source), 'core_mirror': str(core),
              'tests_mirror': str(suite), 'source_head': head,
              'core_head': git(core, 'rev-parse', f'refs/heads/{branch}'),
              'refs_verified': len(old_refs), 'tags_verified': len(tag_report),
              'signed_tags_to_recreate': [t['tag'] for t in tag_report if t['needs_resigning']],
              'removed_historical_paths': sum(removed(p) for p in historical),
              'pack_before': before_size, 'pack_after': git(core, 'count-objects', '-vH'),
              'source_refs_unchanged': True,
              'note': 'Mirrors cover committed history only. Import reviewed working-tree changes before publishing. No remote refs were changed.'}
    write(output/'report.json', report)
    print(json.dumps(report, ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--manifest', type=Path, default=Path('tests/migration/paths.json'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--branch', default='main')
    args = parser.parse_args()
    try:
        prepare(args.source, args.output, args.manifest, args.branch)
    except (ValueError, subprocess.CalledProcessError) as exc:
        print(f'history preparation failed: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
