#!/usr/bin/env python3
"""Compile and exercise the exact vfile-class functions from envelope.c.

This host oracle covers immutable per-vfile class tags and the local-fd ABA
case at the classification layer. It does not replace the kernel test for the
fdtable locked callback or end-to-end SCM_RIGHTS mediation.
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "kernel/ipc/envelope.c"
IPC = ROOT / "kernel/include/ipc/ipc.h"


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise SystemExit(f"missing production function: {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise SystemExit(f"missing body for production function: {signature}")
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start : end + 1]
    raise SystemExit(f"unterminated production function: {signature}")


def enum_value(header: str, name: str) -> int:
    match = re.search(rf"\b{name}\s*=\s*(\d+)\b", header)
    if not match:
        raise SystemExit(f"missing object type in production header: {name}")
    return int(match.group(1))


def main() -> int:
    source = SRC.read_text()
    ipc = IPC.read_text()
    invalid = enum_value(ipc, "A20_OBJ_INVALID")
    file_kind = enum_value(ipc, "A20_OBJ_FILE")
    socket_kind = enum_value(ipc, "A20_OBJ_SOCKET")
    pipe_kind = enum_value(ipc, "A20_OBJ_PIPE_ENDPOINT")
    event_kind = enum_value(ipc, "A20_OBJ_EVENT_QUEUE")
    timer_kind = enum_value(ipc, "A20_OBJ_TIMER")

    functions = "\n\n".join(
        extract_function(source, sig)
        for sig in (
            "int env_kind_tag_vfile(struct vfile *vf, uint8_t obj_type)",
            "uint8_t env_kind_of_vfile(const struct vfile *vf)",
        )
    )
    harness = f"""#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define A20_OBJ_INVALID {invalid}
#define A20_OBJ_FILE {file_kind}
#define A20_OBJ_SOCKET {socket_kind}
#define A20_OBJ_PIPE_ENDPOINT {pipe_kind}
#define A20_OBJ_EVENT_QUEUE {event_kind}
#define A20_OBJ_TIMER {timer_kind}

typedef struct vfile {{
    uint64_t identity;
    uint8_t envelope_kind;
    const void *ops;
    void *priv;
}} vfile_t;

static int g_socket_ops;
static int net_is_socket_vfile(struct vfile *vf) {{
    return vf && vf->ops == &g_socket_ops && vf->priv != NULL;
}}

{functions}

typedef struct tag_attempt {{ vfile_t *vf; uint8_t kind; int result; }} tag_attempt_t;
static void *tag_thread(void *arg) {{
    tag_attempt_t *a = (tag_attempt_t *)arg;
    a->result = env_kind_tag_vfile(a->vf, a->kind);
    return NULL;
}}

static void require(int ok, const char *case_name) {{
    if (!ok) {{ fprintf(stderr, "ENVELOPE_VFILE_KIND: FAIL %s\\n", case_name); exit(1); }}
}}

int main(void) {{
    vfile_t ordinary = {{ .identity = 1 }};
    vfile_t special = {{ .identity = 2 }};
    vfile_t socket = {{ .identity = 3, .ops = &g_socket_ops, .priv = &socket }};

    require(env_kind_of_vfile(&ordinary) == A20_OBJ_FILE, "default-file");
    require(env_kind_tag_vfile(&special, A20_OBJ_PIPE_ENDPOINT) == 0, "first-tag");
    vfile_t *publication_slot = NULL;
    require(publication_slot == NULL &&
            env_kind_of_vfile(&special) == A20_OBJ_PIPE_ENDPOINT,
            "tag-fixed-before-publication");
    publication_slot = &special;
    require(env_kind_of_vfile(publication_slot) == A20_OBJ_PIPE_ENDPOINT,
            "published-tag-visible-immediately");
    require(env_kind_tag_vfile(&special, A20_OBJ_PIPE_ENDPOINT) == 0, "same-tag-idempotent");
    require(env_kind_tag_vfile(&special, A20_OBJ_TIMER) == -EEXIST, "conflicting-tag-rejected");
    require(env_kind_of_vfile(&special) == A20_OBJ_PIPE_ENDPOINT, "tag-stays-immutable");
    vfile_t *alias = &special;
    publication_slot = NULL; /* close one alias; a second still pins this vfile */
    require(alias == &special && env_kind_of_vfile(alias) == A20_OBJ_PIPE_ENDPOINT,
            "closing-one-alias-keeps-class");
    require(env_kind_of_vfile(&socket) == A20_OBJ_SOCKET, "socket-from-pinned-ops");
    require(env_kind_tag_vfile(&ordinary, A20_OBJ_INVALID) == -EINVAL, "invalid-kind-rejected");

    /* Same local fd number may name a replacement during a numeric snapshot,
       then an alias of the original again. The original object's class follows
       its vfile identity; no slot-keyed class can overwrite it. */
    vfile_t *slot = &special;
    vfile_t *pinned = slot;
    slot = &ordinary;
    uint8_t transient_replacement_kind = env_kind_of_vfile(slot);
    slot = &special; /* dup2(original alias) restores the original vfile */
    require(transient_replacement_kind == A20_OBJ_FILE, "replacement-snapshot");
    require(slot == pinned, "aba-slot-restored");
    require(env_kind_of_vfile(pinned) == A20_OBJ_PIPE_ENDPOINT, "aba-class-bound-to-vfile");

    vfile_t raced = {{ .identity = 4 }};
    tag_attempt_t a = {{ &raced, A20_OBJ_EVENT_QUEUE, 99 }};
    tag_attempt_t b = {{ &raced, A20_OBJ_TIMER, 99 }};
    pthread_t ta, tb;
    require(pthread_create(&ta, NULL, tag_thread, &a) == 0, "thread-a-create");
    require(pthread_create(&tb, NULL, tag_thread, &b) == 0, "thread-b-create");
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    require((a.result == 0 && b.result == -EEXIST) ||
            (b.result == 0 && a.result == -EEXIST), "concurrent-conflicting-tags");
    require(env_kind_of_vfile(&raced) == (a.result == 0 ? A20_OBJ_EVENT_QUEUE : A20_OBJ_TIMER),
            "concurrent-tag-final-class");

    puts("ENVELOPE_VFILE_KIND: PASS default aliases ABA socket prepublish concurrent-tag");
    return 0;
}}
"""
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="a20-envelope-vfile-host-") as tmp:
        cfile = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        cfile.write_text(harness)
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread", str(cfile), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
