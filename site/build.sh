#!/usr/bin/env bash
# 组装 A20OS 官网静态产物：
#   site/_pages/           <- 最终 GitHub Pages 部署根目录
#   site/_pages/index.html <- 手工设计的首页 (site/landing/)
#   site/_pages/docs/      <- MkDocs Material 文档站 (渲染仓库 docs/)
set -euo pipefail
cd "$(dirname "$0")"

command -v mkdocs >/dev/null 2>&1 || {
    echo "error: mkdocs not found; install with: pip install mkdocs-material" >&2
    exit 1
}

rm -rf _pages
mkdir -p _pages

# 1. 构建文档站到 _pages/docs/（site_url 指向 /docs/ 子路径，链接自洽）
mkdocs build -f mkdocs.yml -d _pages/docs

# 2. 首页放在站点根目录
cp -a landing/. _pages/

echo "site assembled at site/_pages/ ($(find _pages -type f | wc -l) files)"
