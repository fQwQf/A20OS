#!/usr/bin/env python3
"""Stage the mlibc LLVM compatibility change without altering its submodule."""
from pathlib import Path
import shutil
import sys

root = Path(__file__).resolve().parents[2]
source = root / 'user/external/mlibc'
out = Path(sys.argv[1])
if out.exists():
    shutil.rmtree(out)
shutil.copytree(source, out, symlinks=True, ignore=shutil.ignore_patterns('.git', 'build*', '__pycache__'))
meson = out / 'meson.build'
meson.write_text(meson.read_text().replace("add_languages('c', 'cpp', native: true, required: false)", "# A20 cross-only port: no build-machine programs or Linux headers."))
path = out / 'sysdeps/a20/rtld.cpp'
text = path.read_text()
marker = 'namespace mlibc {'
if text.count(marker) != 1:
    raise SystemExit('mlibc rtld layout changed: review static sysdep adaptation')
text = text.replace(marker, '''/* Static rtld is merged into libc.a and uses libc's full sysdeps.
 * Shared ld.so needs this isolated subset. */
#ifndef MLIBC_STATIC_BUILD
namespace mlibc {''')
text += '\n#endif /* !MLIBC_STATIC_BUILD */\n'
path.write_text(text)
(out / '.prepared').touch()
