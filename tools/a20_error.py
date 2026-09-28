#!/usr/bin/env python3
"""Operator-facing error reporting and the exit-code convention for a20.

Every failure a20 can produce used to escape as whatever the underlying Python
exception happened to be, so `a20 ledger` on a manifest whose make query failed
printed a traceback ending in `a20_make.MakeQueryError`, and a missing `make`
printed `FileNotFoundError: [Errno 2] ... 'make'`.  Both are operator errors with
known remedies, so they are raised as `A20Error` and rendered as one line.

Exit codes are a contract; scripts and CI branch on them.

    0    success
    1    a20 refused, or the instance failed its declared check
    2    command-line usage error
    3    a delegated tool failed (make, QEMU, OpenOCD, a helper script)
    124  timed out

Tool exit statuses are *not* forwarded verbatim any more.  make exits 2 for a
missing rule, which used to be indistinguishable from a mistyped a20 flag.
"""

from __future__ import annotations

import sys

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_USAGE = 2
EXIT_TOOL = 3
EXIT_TIMEOUT = 124


class A20Error(Exception):
    """Base for failures that are the operator's to fix."""

    exit_code = EXIT_FAIL

    def __init__(self, message: str, *, hint: str | None = None) -> None:
        super().__init__(message)
        self.hint = hint


class UsageError(A20Error):
    """The invocation itself was wrong."""

    exit_code = EXIT_USAGE


class ToolError(A20Error):
    """A delegated build/run/flash tool exited non-zero."""

    exit_code = EXIT_TOOL

    def __init__(self, message: str, *, hint: str | None = None,
                 status: int | None = None) -> None:
        super().__init__(message, hint=hint)
        self.status = status

    def exit_code_for(self) -> int:
        # 124 already means "timed out" in the convention; do not shadow it.
        return EXIT_TIMEOUT if self.status == 124 else EXIT_TOOL


class ResourceShortage(A20Error):
    """The host cannot host this guest right now."""


class InstanceBusy(A20Error):
    """Another a20 process already owns this instance."""


class BoardUnreachable(A20Error):
    """A physical board could not be opened or did not answer."""


def report(err: A20Error) -> int:
    """Print an error and its hint to stderr; return the process exit code."""
    print(f"error: {err}", file=sys.stderr)
    if err.hint:
        print(f"hint: {err.hint}", file=sys.stderr)
    code = err.exit_code
    if isinstance(err, ToolError):
        code = err.exit_code_for()
    return code


def fail(message: str, *, hint: str | None = None) -> int:
    """Report a plain failure without raising.  Returns the exit code."""
    return report(A20Error(message, hint=hint))
