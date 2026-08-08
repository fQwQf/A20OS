#!/usr/bin/env bash
# ================================================================
# standard-reference.pdf generator
#
#   make docs
#
# Builds a single combined PDF from every tracked Markdown file under
# docs/. The script first validates that every required tool
# and font is present and prints the exact install commands for
# the host platform when something is missing.
# It never modifies tracked source documents.
# ================================================================
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

OUT=standard-reference.pdf
UNAME="$(uname -s)"

case "$UNAME" in
    Darwin)
        PLATFORM=macos
        ;;
    MINGW* | MSYS* | CYGWIN*)
        PLATFORM=windows
        ;;
    Linux)
        PLATFORM=linux
        ;;
    *)
        PLATFORM=other
        ;;
esac

# --- helper: pretty print -------------------------------------------------
say()  { printf '%s\n' "$*"; }
die()  { say "ERROR: $*" >&2; exit 1; }

# --- tool detection --------------------------------------------------------
missing_tools=()
have_cmd() { command -v "$1" >/dev/null 2>&1; }

for t in git pandoc xelatex; do
    if ! have_cmd "$t"; then
        missing_tools+=("$t")
    fi
done

# SVG conversion is required: README.md embeds shields.io SVG badges and
# docs/poster contains SVG artwork.
RSVG=rsvg-convert
if ! have_cmd "$RSVG"; then
    missing_tools+=("$RSVG")
fi

# --- font detection (needs fontconfig's fc-list) --------------------------
missing_fonts=()
font_present() {
    # returns 0 if a font whose family name equals $1 exists.
    # fc-list exits 0 even for unknown families, so we must check output.
    [[ -n "$(fc-list ":family=$1" family 2>/dev/null)" ]]
}

# Latin/serif main font covering math and Greek symbols (↔ ≈ ≥ ⊆ μ σ).
MAIN_FONT="DejaVu Serif"
# CJK serif used for the body text.
CJK_FONT="Noto Serif CJK SC"
# Latin sans-serif used for headings and code blocks.
SANS_FONT="DejaVu Sans"
# CJK sans-serif used for headings and code blocks.
CJK_SANS_FONT="Noto Sans CJK SC"

if have_cmd fc-list; then
    if ! font_present "$MAIN_FONT"; then
        missing_fonts+=("$MAIN_FONT")
    fi
    if ! font_present "$CJK_FONT"; then
        missing_fonts+=("$CJK_FONT")
    fi
    if ! font_present "$SANS_FONT"; then
        missing_fonts+=("$SANS_FONT")
    fi
    if ! font_present "$CJK_SANS_FONT"; then
        missing_fonts+=("$CJK_SANS_FONT")
    fi
else
    # No fontconfig. On Windows the TeX engine (MiKTeX) reads system fonts
    # directly and fc-list is normally absent, so font checks are skipped.
    # On every other platform fontconfig is required to render CJK reliably.
    if [[ "$PLATFORM" != "windows" ]]; then
        missing_tools+=("fc-list")
    fi
fi

# --- report missing dependencies ------------------------------------------
if [[ ${#missing_tools[@]} -gt 0 || ${#missing_fonts[@]} -gt 0 ]]; then
    say "standard-reference.pdf build requires the following dependencies:" >&2
    if [[ ${#missing_tools[@]} -gt 0 ]]; then
        say "" >&2
        say "Missing tools:" >&2
        for t in "${missing_tools[@]}"; do
            say "  - $t" >&2
        done
    fi
    if [[ ${#missing_fonts[@]} -gt 0 ]]; then
        say "" >&2
        say "Missing fonts:" >&2
        for f in "${missing_fonts[@]}"; do
            say "  - $f" >&2
        done
    fi
    say "" >&2
    say "Install commands:" >&2
    case "$PLATFORM" in
        macos)
            say "  brew install git pandoc rsvg-convert" >&2
            say "  brew install --cask mactex-no-gui" >&2
            say "  brew install font-dejavu font-noto-sans-cjk font-noto-serif-cjk" >&2
            ;;
        linux)
            if have_cmd apt-get; then
                say "  sudo apt-get install git pandoc texlive-xetex texlive-latex-extra texlive-lang-chinese librsvg2-bin fonts-dejavu fonts-noto-cjk fontconfig" >&2
                say "  sudo fc-cache -f   # after installing fonts" >&2
            elif have_cmd dnf; then
                say "  sudo dnf install git pandoc texlive-xetex texlive-collection-latexextra texlive-lang-chinese librsvg2 librsvg2-tools dejavu-sans-fonts dejavu-serif-fonts google-noto-sans-cjk-fonts google-noto-serif-cjk-fonts fontconfig" >&2
                say "  sudo fc-cache -f   # after installing fonts" >&2
            elif have_cmd pacman; then
                say "  sudo pacman -S git pandoc texlive-xetex texlive-latexextra texlive-langchinese librsvg dejavu noto-fonts-cjk fontconfig" >&2
                say "  sudo fc-cache -f   # after installing fonts" >&2
            else
                say "  Install git, pandoc, a TeX distribution with xelatex, librsvg (rsvg-convert)," >&2
                say "  fontconfig, and the fonts 'DejaVu Serif', 'DejaVu Sans'," >&2
                say "  'Noto Serif CJK SC', 'Noto Sans CJK SC'." >&2
            fi
            ;;
        windows)
            # winget covers the tools; librsvg (rsvg-convert), make and the
            # DejaVu font are pulled from MSYS2; Noto CJK fonts have no winget
            # package and must be installed by hand or via MSYS2.
            say "  winget install Git.Git JohnMacFarlane.Pandoc MiKTeX.MiKTeX MSYS2.MSYS2" >&2
            say "  # in an MSYS2 MINGW64 shell:" >&2
            say "  pacman -S --needed make mingw-w64-librsvg mingw-w64-ttf-dejavu mingw-w64-fontconfig" >&2
            say "  # Noto CJK fonts are not packaged in winget. Install them by hand:" >&2
            say "  #   download NotoSansCJK SC + NotoSerifCJK SC OTFs from" >&2
            say "  #   https://fonts.google.com/noto and install into C:\\Windows\\Fonts" >&2
            ;;
        *)
            say "  Install git, pandoc, a TeX distribution with xelatex, rsvg-convert," >&2
            say "  fontconfig, and the fonts 'DejaVu Serif', 'DejaVu Sans'," >&2
            say "  'Noto Serif CJK SC', 'Noto Sans CJK SC'." >&2
            ;;
    esac
    say "" >&2
    say "Then re-run: make docs" >&2
    exit 1
fi

# --- assemble the document list -------------------------------------------
# Every tracked Markdown file under docs/, in lexicographic order. Files are read from git so the build works on a clean checkout without relying on untracked scratch files.
mapfile -t DOCS < <(
    git ls-files 'docs/**/*.md' 'docs/*.md' | sort
)

# --- build ----------------------------------------------------------------
# A20OS LaTeX template renders the documentation as a book: each source
# document begins a new chapter after a distinct cover and front-matter TOC.
# Override the template with A20OS_LATEX_TEMPLATE if needed.
TEMPLATE="${A20OS_LATEX_TEMPLATE:-tools/a20os.latex}"
BUILD_DATE="$(date +%Y-%m-%d)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
LOGO_PDF="$WORK/a20os-logo.pdf"
rsvg-convert --format=pdf --output="$LOGO_PDF" tools/a20os-logo.svg

say "Generating $OUT from $((${#DOCS[@]})) documents ..."
pandoc "${DOCS[@]}" \
    --pdf-engine=xelatex \
    --template="$TEMPLATE" \
    --lua-filter=tools/a20os_pdf.lua \
    --toc \
    --top-level-division=chapter \
    --number-sections \
    -V documentclass=report \
    -V classoption=openany \
    -V title="A20OS" \
    -V subtitle="标准参考手册" \
    -V subject="架构、内核与工程实现" \
    -V author="A20OS Project" \
    -V date="$BUILD_DATE" \
    -V mainfont="$MAIN_FONT" \
    -V CJKmainfont="$CJK_FONT" \
    -V sansfont="$SANS_FONT" \
    -V CJKsansfont="$CJK_SANS_FONT" \
    -V monofont="$SANS_FONT" \
    -V CJKmonofont="$CJK_SANS_FONT" \
    -V a20-styling \
    -V a20-logo="$LOGO_PDF" \
    -V geometry:top=2.4cm,bottom=2.3cm,left=2.6cm,right=2.6cm,headheight=15pt \
    -V toc-title="目录" \
    -V toc-depth=0 \
    -o "$OUT"
say "done: $OUT"
