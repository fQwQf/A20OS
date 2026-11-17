"""Language-neutral document identities and the shared documentation catalog.

Path helpers use only the standard library, so translation does not require MkDocs.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
LANGUAGES = ('zh', 'en')
EXCLUDED = {'archive', 'research', 'paper', 'locales'}
REPOSITORY_README = '@repository-readme'


def validate_id(doc_id):
    if doc_id == REPOSITORY_README:
        return doc_id
    if (not isinstance(doc_id, str) or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*(?:/[A-Za-z0-9_][A-Za-z0-9_.-]*)*', doc_id)
            or EXCLUDED.intersection(Path(doc_id).parts)):
        raise ValueError(f'Invalid document ID: {doc_id!r}')
    return doc_id


def document_path(root, doc_id, language='zh'):
    validate_id(doc_id)
    if language not in LANGUAGES:
        raise ValueError(f'Unsupported language: {language}')
    path = (root / ('README.md' if language == 'zh' else 'README.en.md')
            if doc_id == REPOSITORY_README else root / 'docs' / language / (doc_id + '.md'))
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f'Document path escapes checkout: {doc_id}')
    return path


def document_id(path, root=ROOT):
    rel = path.relative_to(root)
    if rel.as_posix() in ('README.md', 'README.en.md'):
        return REPOSITORY_README
    if len(rel.parts) < 3 or rel.parts[0] != 'docs' or rel.parts[1] not in LANGUAGES or rel.suffix != '.md':
        raise ValueError(f'Not a localized document: {path}')
    return validate_id(Path(*rel.parts[2:]).with_suffix('').as_posix())


def translated_path(source):
    for parent in source.parents:
        if parent.name == 'zh' and parent.parent.name == 'docs':
            return parent.parent / 'en' / source.relative_to(parent)
    if source.name == 'README.md':
        return source.with_name('README.en.md')
    raise ValueError(f'Not a Chinese document: {source}')


def load_yaml(path):
    import yaml
    try:
        value = yaml.safe_load(path.read_text(encoding='utf-8'))
    except (OSError, yaml.YAMLError) as exc:
        raise ValueError(f'Cannot read documentation catalog {path}: {exc}') from exc
    if not isinstance(value, dict):
        raise ValueError(f'Expected a mapping in {path}')
    return value


def locale(root, language):
    if language not in LANGUAGES:
        raise ValueError(f'Unsupported language: {language}')
    return load_yaml(root / 'docs/_meta/languages' / f'{language}.yml')


def navigation(root=ROOT, language='zh'):
    labels = locale(root, language)
    seen = set()
    def walk(nodes):
        if not isinstance(nodes, list) or not nodes:
            raise ValueError('Navigation must contain a non-empty list')
        result = []
        for node in nodes:
            if isinstance(node, str):
                validate_id(node)
                if node == REPOSITORY_README or node in seen or not document_path(root, node).is_file():
                    raise ValueError(f'Duplicate or missing navigation document: {node}')
                seen.add(node)
                path = node + '.md'
                result.append({labels['pages'][node]: path} if node in labels.get('pages', {}) else path)
            elif isinstance(node, dict) and set(node) == {'section', 'children'}:
                section = node['section']
                if section not in labels['sections']:
                    raise ValueError(f'Missing {language} section label: {section}')
                result.append({labels['sections'][section]: walk(node['children'])})
            else:
                raise ValueError(f'Invalid navigation entry: {node!r}')
        return result
    nav = walk(load_yaml(root / 'docs/_meta/navigation.yml').get('nav'))
    return nav, seen


def book(root, name):
    if not re.fullmatch(r'[a-z][a-z0-9-]*', name):
        raise ValueError(f'Invalid book name: {name}')
    data = load_yaml(root / 'docs/_meta/books' / f'{name}.yml')
    chapters = data.get('chapters')
    if not isinstance(chapters, list) or not chapters or not all(isinstance(x, str) for x in chapters):
        raise ValueError(f'Book {name} needs a non-empty chapter list')
    if len(chapters) != len(set(chapters)):
        raise ValueError(f'Duplicate chapter in book {name}')
    _, indexed = navigation(root)
    for doc in chapters:
        validate_id(doc)
        if doc not in indexed:
            raise ValueError(f'Book {name} references unindexed document: {doc}')
    for language in LANGUAGES:
        if name not in locale(root, language).get('books', {}):
            raise ValueError(f'Missing {language} title for book {name}')
    return chapters
