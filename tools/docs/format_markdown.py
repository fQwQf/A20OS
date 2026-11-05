#!/usr/bin/env python3
"""Format and unwrap unnecessary line breaks in Markdown files.

Preserves:
- Code fences (``` or ~~~) and all code contents
- Tables (| ... |)
- Headers (# ..., and Setext === / ---)
- Explicit Markdown line breaks (lines ending with two or more spaces or \\)
- List markers (- , * , + , 1. )
- Blockquotes (> )
- HTML blocks and comments (<!-- ... -->)
- Frontmatter (--- ... ---)

Joins hard-wrapped text within paragraphs, list items, and blockquotes,
handling CJK character boundaries without inserting erroneous whitespace.
"""

import argparse
import difflib
import os
import re
import sys

def is_cjk(ch):
    code = ord(ch)
    return (0x4e00 <= code <= 0x9fff or       # CJK Unified Ideographs
            0x3400 <= code <= 0x4dbf or       # CJK Extension A
            0x3000 <= code <= 0x303f or       # CJK Symbols and Punctuation
            0xff00 <= code <= 0xffef)         # Halfwidth and Fullwidth Forms

def join_two_lines(s1, s2):
    s1_r = s1.rstrip()
    s2_stripped = s2.strip()
    if not s1_r:
        return s2_stripped
    if not s2_stripped:
        return s1_r
        
    last = s1_r[-1]
    first = s2_stripped[0]
    
    # If line1 already had intentional space at the end
    if s1.endswith(' ') or s1.endswith('\t'):
        return s1_r + ' ' + s2_stripped
        
    if is_cjk(last) or is_cjk(first):
        if is_cjk(last) and is_cjk(first):
            return s1_r + s2_stripped
        if last in ('*', '`', '_', '[', '(', '<', '“', '‘', '《') or first in ('*', '`', '_', ']', ')', '>', '”', '’', '》'):
            return s1_r + s2_stripped
        if is_cjk(last) and first.isalnum():
            return s1_r + ' ' + s2_stripped
        if last.isalnum() and is_cjk(first):
            return s1_r + ' ' + s2_stripped
        return s1_r + s2_stripped
    else:
        return s1_r + ' ' + s2_stripped

def is_table_line(line):
    s = line.strip()
    return s.startswith('|') or (s.endswith('|') and '|' in s)

def is_setext_underline(line):
    s = line.strip()
    return bool(re.match(r'^(?:={3,}|-{3,})$', s))

def is_list_item_start(line):
    return bool(re.match(r'^\s*(?:[-*+]|\d+\.)\s+', line))

def is_explicit_break(line):
    return line.endswith('  ') or line.rstrip('\r\n').endswith('\\')

def unwrap_markdown(text):
    lines = text.splitlines()
    out = []
    in_code = False
    code_fence = ""
    code_indent = 0
    in_html_comment = False
    
    i = 0
    n = len(lines)
    
    # Frontmatter handling (YAML)
    if n > 0 and lines[0].strip() == '---':
        out.append(lines[0])
        i = 1
        while i < n:
            out.append(lines[i])
            if lines[i].strip() == '---':
                i += 1
                break
            i += 1
            
    while i < n:
        line = lines[i]
        stripped = line.strip()
        
        # HTML comment block
        if in_html_comment:
            out.append(line)
            if '-->' in stripped:
                in_html_comment = False
            i += 1
            continue
        if stripped.startswith('<!--') and '-->' not in stripped:
            in_html_comment = True
            out.append(line)
            i += 1
            continue
            
        # Code fence handling
        fence_match = re.match(r'^(\s*)(```+|~~~+)', line)
        if fence_match:
            fence = fence_match.group(2)
            if not in_code:
                in_code = True
                code_fence = fence[:3]
                code_indent = len(fence_match.group(1))
                out.append(line)
                i += 1
                continue
            elif stripped.startswith(code_fence):
                in_code = False
                code_fence = ""
                out.append(line)
                i += 1
                continue
                
        if in_code:
            out.append(line)
            i += 1
            continue
            
        # Blank line
        if not stripped:
            out.append(line)
            i += 1
            continue
            
        # Heading
        if stripped.startswith('#'):
            out.append(line)
            i += 1
            continue
            
        # Setext heading detection (line followed by === or ---)
        if i + 1 < n and is_setext_underline(lines[i + 1]) and not is_list_item_start(line) and not stripped.startswith('>'):
            out.append(line)
            out.append(lines[i + 1])
            i += 2
            continue
            
        # Horizontal rules
        if re.match(r'^(?:-{3,}|\*{3,}|_{3,})$', stripped):
            out.append(line)
            i += 1
            continue
            
        # Tables
        if is_table_line(line):
            out.append(line)
            i += 1
            continue
            
        # Explicit line break (ending with 2 spaces or \)
        if is_explicit_break(line):
            out.append(line)
            i += 1
            continue
            
        # Blockquotes
        if stripped.startswith('>'):
            bq_lines = []
            while i < n:
                cur = lines[i]
                cur_s = cur.strip()
                if not cur_s or not cur_s.startswith('>'):
                    break
                if re.match(r'^>\s*\[!|^>\s*#', cur_s) or '|' in cur_s or is_explicit_break(cur):
                    if bq_lines:
                        break
                    bq_lines.append(cur)
                    i += 1
                    break
                bq_lines.append(cur)
                i += 1
            if len(bq_lines) == 1:
                out.append(bq_lines[0])
            else:
                m1 = re.match(r'^(>\s*)+', bq_lines[0])
                prefix = m1.group(0) if m1 else "> "
                content = bq_lines[0][len(prefix):]
                for nxt in bq_lines[1:]:
                    m2 = re.match(r'^(>\s*)+', nxt)
                    p2 = m2.group(0) if m2 else "> "
                    content = join_two_lines(content, nxt[len(p2):])
                out.append(prefix + content)
            continue
            
        # List items
        list_match = re.match(r'^(\s*(?:[-*+]|\d+\.)\s+)(.*)$', line)
        if list_match:
            prefix = list_match.group(1)
            content = list_match.group(2)
            i += 1
            while i < n:
                nxt = lines[i]
                nxt_s = nxt.strip()
                if not nxt_s:
                    break
                # Stop if next line is a list item, heading, table, quote, code fence, HR
                if (is_list_item_start(nxt) or
                    nxt_s.startswith(('#', '>', '|', '```', '~~~')) or
                    re.match(r'^(?:-{3,}|\*{3,}|_{3,})$', nxt_s) or
                    is_setext_underline(nxt)):
                    break
                # If current line ended with explicit break
                if is_explicit_break(lines[i - 1]):
                    break
                content = join_two_lines(content, nxt_s)
                i += 1
            out.append(prefix + content)
            continue
            
        # Plain paragraph lines
        para_lines = [line]
        i += 1
        while i < n:
            nxt = lines[i]
            nxt_s = nxt.strip()
            if not nxt_s:
                break
            if (nxt_s.startswith(('#', '>', '|', '```', '~~~')) or
                is_list_item_start(nxt) or
                re.match(r'^(?:-{3,}|\*{3,}|_{3,})$', nxt_s) or
                is_setext_underline(nxt)):
                break
            if is_explicit_break(lines[i - 1]):
                break
            para_lines.append(nxt)
            i += 1
            
        if len(para_lines) == 1:
            out.append(para_lines[0])
        else:
            res = para_lines[0]
            for nxt in para_lines[1:]:
                res = join_two_lines(res, nxt)
            out.append(res)
            
    result = '\n'.join(out)
    if text.endswith('\n') and not result.endswith('\n'):
        result += '\n'
    return result

def process_file(path, write=False, show_diff=False):
    with open(path, 'r', encoding='utf-8', errors='ignore') as f:
        orig = f.read()
    formatted = unwrap_markdown(orig)
    if orig == formatted:
        return False
    if show_diff:
        diff = difflib.unified_diff(
            orig.splitlines(keepends=True),
            formatted.splitlines(keepends=True),
            fromfile=path,
            tofile=path
        )
        sys.stdout.writelines(diff)
    if write:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(formatted)
    return True

def main():
    parser = argparse.ArgumentParser(description="Format markdown by unwrapping unnecessary line breaks.")
    parser.add_argument("paths", nargs="*", default=["docs"], help="Files or directories to format (default: docs)")
    parser.add_argument("-w", "--write", action="store_true", help="Write changes to files in place")
    parser.add_argument("-d", "--diff", action="store_true", help="Show unified diff of changes")
    parser.add_argument("-c", "--check", action="store_true", help="Check if files need formatting (exit 1 if modified)")
    args = parser.parse_args()

    files = []
    for p in args.paths:
        if os.path.isfile(p):
            files.append(p)
        elif os.path.isdir(p):
            for root, dirs, fnames in os.walk(p):
                # Skip external/vendored directories
                if any(skip in root for skip in [".git", "node_modules", "external"]):
                    continue
                for fn in fnames:
                    if fn.endswith('.md'):
                        files.append(os.path.join(root, fn))

    modified_count = 0
    for f in sorted(files):
        mod = process_file(f, write=args.write, show_diff=args.diff)
        if mod:
            modified_count += 1
            if not args.diff:
                print(f"{'Formatted' if args.write else 'Needs format'}: {f}")

    print(f"\nTotal markdown files checked: {len(files)}")
    print(f"Files {'formatted' if args.write else 'needing formatting'}: {modified_count}")

    if args.check and modified_count > 0:
        sys.exit(1)

if __name__ == "__main__":
    main()
