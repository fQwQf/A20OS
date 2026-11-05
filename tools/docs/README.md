# Documentation & MkDocs Tooling (`tools/docs/`)

This directory centralizes tools, validation gates, formatting utilities, and publishing hooks for A20OS project documentation:

| Tool | Purpose | Invoked By |
|---|---|---|
| `check_doc_citations.py` | Validates in-tree source code line-number citations (`file.c:123`) across markdown documentation. | `make check-docs`, CI |
| `check_doc_nav.py` | Verifies 100% MkDocs navigation coverage, anchor resolution, and internal link integrity. | `make check-docs`, CI |
| `format_markdown.py` | Formats and unwraps paragraphs while preserving tables, code fences, and CJK boundaries. | `make check-docs`, pre-commit |
| `gen_docs_pdf.sh` | Compiles a unified reference manual PDF via Pandoc and XeLaTeX. | `make docs` |
| `mkdocs_repo_links.py` | MkDocs build hook translating internal repository links for web publication. | `mkdocs.yml`, `site/mkdocs.yml`, Pages CI |

## Subdirectories

- `pandoc/`: XeLaTeX layout templates (`a20os.latex`), Lua filters (`a20os_pdf.lua`), and visual assets used exclusively by `gen_docs_pdf.sh`.
