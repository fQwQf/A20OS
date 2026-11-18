# A20OS documentation / A20OS 文档

- [简体中文源文档](zh/README.md) · [中文网站](https://fqwqf.github.io/A20OS/docs/)
- [English documentation](en/README.md) · [English website](https://fqwqf.github.io/A20OS/docs/en/)

中文是源版本，英文按变更增量同步。网站会明确标出尚未翻译或需要更新的页面。 Chinese is the source language. English updates are incremental; the website labels missing or stale translations.

## PDF manuals / PDF 手册

```sh
make docs                                      # Chinese reference manual
make docs DOC_LANG=en                          # English; requires current translations
make docs DOC_LANG=all DOC_BOOK=developer       # Both languages; preflight checks both
make docs DOC_LANG=en DOC_BOOK=user DOC_MODE=preview
```

PDF 输出至 `build/docs/pdf/<language>/<book>.pdf`；预览文件名为 `<book>-preview.pdf`，中文回退章节会明确标记。构建报告位于 `build/docs/reports/<language>/`。默认中文参考手册另复制到仓库根目录 `standard-reference.pdf`，兼容原有用法。 PDFs are written to `build/docs/pdf/<language>/<book>.pdf`; previews use `<book>-preview.pdf` and explicitly mark Chinese fallback chapters. Reports are under `build/docs/reports/<language>/`. The default Chinese reference manual is also copied to `standard-reference.pdf` at the repository root.

网站、PDF 与翻译使用同一文档 ID（如 `dev/build`）。维护方法见 [文档国际化](zh/dev/documentation-i18n.md)。 The website, PDF books and translator share document IDs such as `dev/build`. See the [maintenance guide (Chinese)](zh/dev/documentation-i18n.md).
