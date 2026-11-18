# Shared PDF templates

- `a20os.latex`: common XeLaTeX layout; labels come from `docs/_meta/languages/<lang>.yml`.
- `a20os_pdf.lua`: bounds display mathematics and permits line breaks in long inline code and comma-separated mathematics.

`../build_pdf.py` selects chapters, validates translations, namespaces anchors and resolves links and images before invoking Pandoc. The shared cover logo lives at `docs/assets/a20os-logo.svg`. See [the build guide](../../../docs/zh/dev/documentation-i18n.md).
