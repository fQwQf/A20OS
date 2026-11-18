# Documentation & MkDocs Tooling (`tools/docs/`)

This directory centralizes tools, validation gates, formatting utilities, and publishing hooks for A20OS project documentation:

| Tool | Purpose | Invoked By |
|---|---|---|
| `check_doc_citations.py` | Validates in-tree source code line-number citations (`file.c:123`) across markdown documentation. | `make check-docs`, CI |
| `check_doc_nav.py` | Verifies 100% MkDocs navigation coverage, anchor resolution, and internal link integrity. | `make check-docs`, CI |
| `format_markdown.py` | Formats and unwraps paragraphs while preserving tables, code fences, and CJK boundaries. | `make check-docs`, pre-commit |
| `gen_docs_pdf.sh`, `build_pdf.py` | Checks dependencies; builds curated books with translation gates, local anchors and provenance reports. | `make docs DOC_LANG=zh DOC_BOOK=reference` |
| `layout.py` | Shared document IDs, safe locale paths, navigation and book validation. | Website, PDF and translation tooling |
| `mkdocs_repo_links.py` | MkDocs build hook translating internal repository links for web publication. | `mkdocs.yml`, `site/mkdocs.yml`, Pages CI |
| `doc_translator.py` | Incremental translation, protected Markdown, cache validation and reviewed-translation adoption. | Translation CI, `make check-docs` |
| `mkdocs_i18n.py` | English navigation, fallback notices and stable source heading IDs. | MkDocs builds |

Translation setup and review instructions: [documentation internationalization](../../docs/zh/dev/documentation-i18n.md).

## Subdirectories

- `pandoc/`: XeLaTeX layout templates (`a20os.latex`), Lua filters (`a20os_pdf.lua`), shared by every language and book. The shared logo is in `docs/assets/`.
