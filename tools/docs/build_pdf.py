#!/usr/bin/env python3
"""Build offline, language-aware PDF books from the shared documentation tree."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import quote, unquote, urlsplit, urlunsplit

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.docs.layout import book, document_id, document_path, locale, navigation, REPOSITORY_README
from tools.docs.doc_translator import digest, is_current, load_cache, policy_hash

SITE_URL = 'https://fqwqf.github.io/A20OS/docs/'
REPO_URL = 'https://github.com/fQwQf/A20OS'


def plan(root, name, language, mode):
    chapters = book(root, name)
    cache, policy = load_cache(root), policy_hash(root)
    result, missing = [], []
    for key in chapters:
        source, translated = document_path(root, key), document_path(root, key, 'en')
        current = is_current(source, translated, cache.get(key, {}), policy)
        fallback = language == 'en' and not current
        if fallback:
            missing.append(key + (' (missing)' if not translated.exists() else ' (stale or unreviewed)'))
        selected = translated if language == 'en' and current else source
        result.append({'id': key, 'path': selected.relative_to(root).as_posix(),
                       'language': 'zh' if fallback else language, 'fallback': fallback,
                       'source_sha256': digest(source.read_text()),
                       'content_sha256': digest(selected.read_text())})
    if missing and mode == 'strict':
        raise ValueError(f'Cannot build formal English book {name}; translation required:\n  '
                         + '\n  '.join(missing)
                         + '\nUse DOC_MODE=preview for an explicitly marked preview, or synchronize these translations.')
    return result


def walk(node):
    if isinstance(node, dict):
        yield node
        for value in node.values():
            yield from walk(value)
    elif isinstance(node, list):
        for item in node:
            yield from walk(item)


def anchor(key, fragment):
    return 'doc-' + hashlib.sha256((key + '#' + fragment).encode()).hexdigest()[:24]


def heading_ids(text, config):
    import markdown
    from tools.docs.mkdocs_i18n import HeadingCollector
    parser = HeadingCollector()
    parser.feed(markdown.Markdown(extensions=config['markdown_extensions'],
                                 extension_configs=config['mdx_configs']).convert(text))
    return parser.headings


def parse_chapter(root, row, config):
    path = root / row['path']
    ast = json.loads(subprocess.check_output(['pandoc', '--from=markdown', '--to=json', str(path)]))
    headers = [n for n in walk(ast['blocks']) if n.get('t') == 'Header']
    source_ids = heading_ids(document_path(root, row['id']).read_text(), config)
    if not headers or [h['c'][0] for h in headers] != [int(tag[1:]) for tag, _ in source_ids]:
        raise ValueError(f"PDF/site heading structure differs in {row['id']}")
    anchors = {}
    for h, (_, fragment) in zip(headers, source_ids):
        new_id = anchor(row['id'], fragment)
        anchors[h['c'][1][0]] = new_id
        anchors[fragment] = new_id
        h['c'][1][0] = new_id
    anchors[''] = headers[0]['c'][1][0]
    if row['fallback']:
        notice = {'t': 'BlockQuote', 'c': [{'t': 'Para', 'c': [{'t': 'Strong', 'c': [
            {'t': 'Str', 'c': f"CHINESE FALLBACK: {row['id']} (translation missing or stale)."}]}]}]}
        ast['blocks'].insert(1, notice)
    return ast, anchors


def resolve_link(url, source, root, language, anchors, indexed):
    parsed = urlsplit(url)
    if parsed.scheme or parsed.netloc:
        return url
    if parsed.path.startswith('/'):
        return urlunsplit(('https', 'fqwqf.github.io', parsed.path, parsed.query, parsed.fragment))
    target = (source.parent / unquote(parsed.path)).resolve() if parsed.path else source
    if not target.is_relative_to(root):
        raise ValueError(f'Link leaves checkout: {source}: {url}')
    if target.is_relative_to(root / 'docs/en') and not target.exists():
        target = root / 'docs/zh' / target.relative_to(root / 'docs/en')
    if target.is_dir() and (target / 'README.md').is_file():
        target /= 'README.md'
    if not target.exists():
        raise ValueError(f'Missing PDF link target: {source}: {url}')
    try:
        key = document_id(target, root)
    except ValueError:
        key = None
    fragment = unquote(parsed.fragment)
    if key in anchors:
        if fragment not in anchors[key]:
            raise ValueError(f'Missing book anchor: {source}: {url}')
        return '#' + anchors[key][fragment]
    if key in indexed:
        page = key.removesuffix('/README') if key.endswith('/README') else ('' if key == 'README' else key)
        path = ('en/' if language == 'en' else '') + (page + '/' if page else '')
        return SITE_URL + path + ('?' + parsed.query if parsed.query else '') + ('#' + quote(fragment) if fragment else '')
    kind = 'tree' if target.is_dir() else 'blob'
    return (REPO_URL + '/' + kind + '/main/' + quote(target.relative_to(root).as_posix())
            + ('#' + quote(fragment) if fragment else ''))


def compile_book(root, rows, language, config):
    parsed = [parse_chapter(root, row, config) for row in rows]
    anchors = {row['id']: item[1] for row, item in zip(rows, parsed)}
    _, indexed = navigation(root)
    blocks = []
    for row, (ast, _) in zip(rows, parsed):
        source = root / row['path']
        for node in walk(ast['blocks']):
            if node.get('t') == 'Link':
                node['c'][2][0] = resolve_link(node['c'][2][0], source, root, language, anchors, indexed)
            elif node.get('t') == 'Image':
                url = node['c'][2][0]
                if urlsplit(url).scheme or url.startswith('//'):
                    raise ValueError(f'Offline PDF requires a local image: {row["id"]}: {url}')
                image = (source.parent / unquote(urlsplit(url).path)).resolve()
                if not image.is_relative_to(root) or not image.is_file():
                    raise ValueError(f'Missing or unsafe PDF image: {row["id"]}: {url}')
                node['c'][2][0] = str(image)
        blocks.extend(ast['blocks'])
    return dict(parsed[0][0], blocks=blocks)


def tool_version(tool):
    return subprocess.check_output([tool, '--version'], text=True).splitlines()[0]


def build(root, name, language, mode, rows):
    from mkdocs.config import load_config
    template = Path(os.environ.get('A20OS_LATEX_TEMPLATE', str(root / 'tools/docs/pandoc/a20os.latex'))).resolve()
    inputs = [root / 'docs/_meta/navigation.yml', root / 'docs/_meta/books' / f'{name}.yml',
              root / 'docs/_meta/languages/zh.yml', root / 'docs/_meta/languages/en.yml', template,
              root / 'tools/docs/pandoc/a20os_pdf.lua', root / 'docs/assets/a20os-logo.svg',
              root / 'tools/docs/build_pdf.py', root / 'tools/docs/layout.py',
              root / 'tools/docs/mkdocs_i18n.py', root / 'site/requirements.txt',
              root / 'mkdocs.yml', root / 'site/mkdocs.yml']
    input_hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    config = load_config(config_file=str(root / 'mkdocs.yml'))
    labels = locale(root, language)
    suffix = '-preview' if mode == 'preview' else ''
    stem = name + suffix
    output = root / 'build/docs/pdf' / language / (stem + '.pdf')
    report_path = root / 'build/docs/reports' / language / (stem + '.json')
    output.parent.mkdir(parents=True, exist_ok=True)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', datetime.now(timezone.utc).timestamp()))
    built = datetime.fromtimestamp(epoch, timezone.utc)
    title = labels['books'][name] + (' — PREVIEW' if mode == 'preview' else '')
    print(f'Building {language}/{stem}: {len(rows)} chapters', flush=True)
    ast = compile_book(root, rows, language, config)
    work_root = root / 'build/docs/.tmp'
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work_root) as scratch:
        work = Path(scratch)
        logo = work / 'a20os-logo.pdf'
        subprocess.run(['rsvg-convert', '--format=pdf', '--output=' + str(logo),
                        str(root / 'docs/assets/a20os-logo.svg')], check=True)
        source = work / 'book.json'
        source.write_text(json.dumps(ast, ensure_ascii=False))
        pdf = work / 'book.pdf'
        variables = {
            'documentclass': 'report', 'classoption': 'openany', 'title': 'A20OS',
            'subtitle': title, 'subject': labels['subject'], 'author': 'A20OS Project',
            'date': built.strftime('%Y-%m-%d'), 'mainfont': os.environ.get('DOC_MAIN_FONT', 'DejaVu Serif'),
            'sansfont': os.environ.get('DOC_SANS_FONT', 'DejaVu Sans'),
            'monofont': os.environ.get('DOC_SANS_FONT', 'DejaVu Sans'),
            'CJKmainfont': os.environ.get('DOC_CJK_FONT', 'Noto Serif CJK SC'),
            'CJKsansfont': os.environ.get('DOC_CJK_SANS_FONT', 'Noto Sans CJK SC'),
            'CJKmonofont': os.environ.get('DOC_CJK_SANS_FONT', 'Noto Sans CJK SC'),
            'a20-styling': 'true', 'a20-logo': str(logo), 'toc-title': labels['contents'], 'toc-depth': '0',
            'chapter-prefix': labels['chapter_prefix'], 'chapter-suffix': labels['chapter_suffix'],
            'figure-name': labels['figure'], 'table-name': labels['table'], 'doc-language': labels['language_tag'],
            'geometry': 'top=2.4cm,bottom=2.3cm,left=2.6cm,right=2.6cm,headheight=15pt',
        }
        for key in ('document_type_label', 'document_type', 'date_label', 'author_label'):
            variables[key.replace('_', '-')] = labels[key]
        command = ['pandoc', str(source), '--from=json', '--fail-if-warnings', '--pdf-engine=xelatex',
                   '--template=' + str(template), '--lua-filter=' + str(root / 'tools/docs/pandoc/a20os_pdf.lua'),
                   '--toc', '--top-level-division=chapter', '--number-sections',
                   '--metadata', 'title=A20OS — ' + title, '--metadata', 'author=A20OS Project', '-o', str(pdf)]
        for key, value in variables.items():
            command.extend(['-V', f'{key}={value}'])
        subprocess.run(command, check=True, cwd=root)
        # Validate the captured input hashes before publishing the artifact.
        for row in rows:
            if (digest((root / row['path']).read_text()) != row['content_sha256']
                    or digest(document_path(root, row['id']).read_text()) != row['source_sha256']):
                raise ValueError(f'Source changed during PDF build: {row["id"]}')
        for path, expected in input_hashes.items():
            if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                raise ValueError(f'Build configuration changed during PDF build: {path}')
        report = {'book': name, 'language': language, 'mode': mode, 'built_at': built.isoformat(),
                  'git_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
                  'worktree_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root)),
                  'tools': {tool: tool_version(tool) for tool in ('pandoc', 'xelatex', 'rsvg-convert')},
                  'translation_policy_sha256': policy_hash(root), 'python': sys.version.split()[0],
                  'chapters': rows, 'inputs': {str(p.relative_to(root)) if p.is_relative_to(root) else str(p): input_hashes[p] for p in inputs},
                  'pdf_sha256': hashlib.sha256(pdf.read_bytes()).hexdigest()}
        pending_report = work / 'report.json'
        pending_report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        pdf.replace(output)
        pending_report.replace(report_path)
    if language == 'zh' and name == 'reference' and mode == 'strict':
        shutil.copyfile(output, root / 'standard-reference.pdf')
    print(f'Built {output.relative_to(root)}; report: {report_path.relative_to(root)}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lang', choices=('zh', 'en', 'all'), default='zh')
    parser.add_argument('--book', default='reference')
    parser.add_argument('--mode', choices=('strict', 'preview'), default='strict')
    parser.add_argument('--plan', action='store_true', help='Validate chapters/translations without building a PDF')
    args = parser.parse_args()
    try:
        languages = ('zh', 'en') if args.lang == 'all' else (args.lang,)
        # Check every requested language before producing any artifacts.
        plans = {language: plan(ROOT, args.book, language, args.mode) for language in languages}
        if args.plan:
            print(json.dumps(plans, ensure_ascii=False, indent=2))
        else:
            for language, rows in plans.items():
                build(ROOT, args.book, language, args.mode, rows)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print(f'docs-pdf: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
