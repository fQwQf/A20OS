"""Tab completion for the a20 CLI (`a20 completion`, wired via a hidden
`a20 complete` command).

The split of responsibilities mirrors the rest of the toolchain: tools/a20 owns
the argparse tree, this module reads it.  Candidates are derived from the live
parser -- subcommand names, per-command flags, and the instance names the
command takes -- so they cannot drift from the CLI they complete.  The bash
shim (BASH_COMPLETION, printed by `a20 completion`) calls back into the same
executable the user typed:

    a20 complete <words...>     # words = COMP_WORDS[1:]; last word is the
                                # (possibly empty) one under the cursor

and gets one candidate per line, already filtered by that last word.  Because
the shim also passes -o default, an empty reply falls through to filename
completion -- which is exactly right for `a20 check <manifest>` and for make
arguments after `--`.
"""

from __future__ import annotations

import argparse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
INSTANCES_DIR = REPO_ROOT / "instances"

# `complete` is plumbing, not a user-facing command; it stays out of the
# suggestions the way argparse.SUPPRESS keeps it out of --help.
HIDDEN_COMMANDS = frozenset({"complete"})

# Commands whose positional(s) are instance names.  check's positional is a
# manifest *path*, so it gets no candidates and falls back to filenames.
INSTANCE_COMMANDS = frozenset({
    "console", "deploy", "ledger", "show", "show-vars",
    "build", "run", "debug", "test", "flash", "package", "ports",
})


def instance_names() -> list[str]:
    return sorted(p.stem for p in INSTANCES_DIR.glob("*.toml"))


def _known_arches() -> list[str]:
    # parse_instance, not a regex: this must stay true to what actually parses.
    from a20_instance import InstanceError, parse_instance
    arches: set[str] = set()
    for path in INSTANCES_DIR.glob("*.toml"):
        try:
            arches.add(parse_instance(path).arch)
        except (InstanceError, OSError):
            continue
    return sorted(arches)


def _subcommands(parser: argparse.ArgumentParser) -> dict[str, argparse.ArgumentParser]:
    action = next(a for a in parser._actions
                  if isinstance(a, argparse._SubParsersAction))
    return action.choices


def _visible_commands(subs: dict[str, argparse.ArgumentParser],
                      prefix: str) -> list[str]:
    return [c for c in sorted(subs)
            if c not in HIDDEN_COMMANDS and c.startswith(prefix)]


def _flags_of(sub: argparse.ArgumentParser) -> list[str]:
    return sorted({opt for a in sub._actions for opt in a.option_strings})


def _arches() -> list[str]:
    return _known_arches()


# Flag arguments whose values we can enumerate, keyed on (command, flag).
_FLAG_VALUES: dict[tuple[str, str], object] = {
    ("list", "--arch"): _arches,
    ("list", "--action"): lambda: ("build", "run", "debug", "test", "flash",
                                   "package", "console", "deploy"),
    ("boards", "--arch"): _arches,
    ("check", "--require-arch"): _arches,
    ("ledger", "--format"): lambda: ("table", "json", "markdown"),
}


def _positional_filled(sub: argparse.ArgumentParser, tokens: list[str]) -> bool:
    """Whether any non-flag token has already occupied a positional slot.

    A value-taking flag swallows the word after it, so in
    `a20 list --arch riscv64` the "riscv64" is --arch's value, not a positional.
    Variadic flags (*, +) make that undecidable; assume the rest belongs to the
    flag, which errs toward fewer suggestions rather than wrong ones.
    """
    by_opt = {o: a for a in sub._actions for o in a.option_strings}
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        action = by_opt.get(tok)
        if action is not None and action.nargs != 0:
            if action.nargs in (None, 1, "?"):
                i += 2
                continue
            return True
        if not tok.startswith("-"):
            return True
        i += 1
    return False


def candidates(parser: argparse.ArgumentParser, words: list[str]) -> list[str]:
    """Completions for the argument list `words` (command word excluded, the
    word under the cursor last, possibly empty)."""
    subs = _subcommands(parser)
    if not words:
        return _visible_commands(subs, "")
    cmd = words[0]
    if cmd not in subs or cmd in HIDDEN_COMMANDS:
        # Mid-typing the command word (or a typo): suggest commands.
        return _visible_commands(subs, cmd)
    if cmd == "complete":
        return []
    if len(words) == 1:
        # Cursor sits on the command word itself.
        return _visible_commands(subs, cmd)
    sub = subs[cmd]
    rest, cur = words[1:-1], words[-1]

    provider = _FLAG_VALUES.get((cmd, rest[-1])) if rest else None
    if provider is not None:
        return [v for v in provider() if v.startswith(cur)]
    if cur.startswith("-"):
        return [f for f in _flags_of(sub) if f.startswith(cur)]
    if cmd not in INSTANCE_COMMANDS:
        return []
    # ports takes repeatable instances; every other command here stops once
    # the instance slot is filled.
    if cmd != "ports" and _positional_filled(sub, rest):
        return []
    return [n for n in instance_names() if n.startswith(cur)]


BASH_COMPLETION = """\
# bash completion for a20 -- install with:  eval "$(tools/a20 completion)"
# zsh: run  autoload -U +X bashcompinit && bashcompinit  before the eval line.
# Completing another spelling of the command (an alias, an absolute path) just
# needs one more registration:  complete -F _a20_completions <word>
_a20_completions() {
    local reply
    reply=$("${COMP_WORDS[0]}" complete "${COMP_WORDS[@]:1}" 2>/dev/null) || return 0
    COMPREPLY=($reply)
}
complete -o bashdefault -o default -F _a20_completions \\
    a20 tools/a20 ./tools/a20 2>/dev/null
"""
