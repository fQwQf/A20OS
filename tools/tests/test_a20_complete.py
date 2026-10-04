"""Tests for tab completion (tools/a20_complete.py + the `a20 complete`
and `a20 completion` subcommands in tools/a20).

The candidates must come from the same argparse tree main() parses with --
a hardcoded list in the completion module would drift from the CLI exactly
when a new command is added.  Most tests therefore load the real tools/a20
script (same trick as test_a20._load_cli) and introspect its parser; the
instance directory is pointed at a temp tree so editing instances/*.toml
cannot break the tool's own tests.  The one subprocess test reads the real
instances/ on purpose: it pins the end-to-end shim contract
(`a20 complete <words...>` prints candidates, one per line).

Stdlib unittest, matching tools/tests/test_a20.py.
"""

from __future__ import annotations

import io
import contextlib
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import a20_complete  # noqa: E402
from a20_make import REPO_ROOT  # noqa: E402


def _load_cli():
    """Import tools/a20 (a script, not a module) so build_parser is callable."""
    import importlib.machinery
    import importlib.util
    loader = importlib.machinery.SourceFileLoader(
        "a20_cli_complete_test", str(REPO_ROOT / "tools" / "a20"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


class TestCandidates(unittest.TestCase):
    """candidates() against the real parser, a synthetic instances/ tree."""

    def setUp(self) -> None:
        self.a20 = _load_cli()
        self.parser = self.a20.build_parser()
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        for name, arch in (("qemu-x", "riscv64"), ("xfce-y", "x86_64")):
            (self.tmp / f"{name}.toml").write_text(
                f'name = "{name}"\narch = "{arch}"\n', encoding="utf-8")

    def complete(self, *words: str) -> list[str]:
        with patch.object(a20_complete, "INSTANCES_DIR", self.tmp):
            return a20_complete.candidates(self.parser, list(words))

    def test_bare_cursor_lists_commands_and_hides_plumbing(self) -> None:
        got = self.complete("")
        self.assertIn("run", got)
        self.assertIn("completion", got)
        self.assertNotIn("complete", got, "the plumbing command must not be suggested")

    def test_partial_command_word(self) -> None:
        self.assertEqual(self.complete("ru"), ["run"])

    def test_cursor_on_command_word_suggests_commands(self) -> None:
        self.assertEqual(self.complete("run"), ["run"])

    def test_instance_positional_lists_instances(self) -> None:
        self.assertEqual(self.complete("run", "xf"), ["xfce-y"])

    def test_filled_instance_stops_suggestions(self) -> None:
        self.assertEqual(self.complete("run", "xfce-y", ""), [])

    def test_boolean_flag_does_not_eat_the_instance_slot(self) -> None:
        self.assertEqual(self.complete("run", "--no-wait", "xf"), ["xfce-y"])

    def test_flag_completion_is_per_command(self) -> None:
        self.assertEqual(self.complete("run", "--no-w"), ["--no-wait"])
        # --no-reset belongs to console, not run.
        self.assertNotIn("--no-reset", self.complete("run", "--no"))

    def test_flag_value_completion(self) -> None:
        self.assertEqual(self.complete("list", "--arch", "ris"), ["riscv64"])
        self.assertEqual(self.complete("ledger", "--format", "j"), ["json"])
        self.assertEqual(self.complete("list", "--action", "ru"), ["run"])

    def test_arches_come_from_parsed_instances(self) -> None:
        self.assertEqual(self.complete("boards", "--arch", ""), ["riscv64", "x86_64"])

    def test_path_positional_falls_back_to_filenames(self) -> None:
        # check takes manifest paths; no candidates means the shim's
        # -o default lets bash fall through to filename completion.
        self.assertEqual(self.complete("check", ""), [])

    def test_unknown_first_word_suggests_commands(self) -> None:
        got = self.complete("chec")
        self.assertEqual(got, ["check", "check-flash-backends", "check-registry"])

    def test_empty_words_list_all_commands(self) -> None:
        self.assertIn("boards", self.complete())


class TestShimContract(unittest.TestCase):
    """What the bash shim actually sees, through the real CLI."""

    def test_complete_subcommand_prints_candidates(self) -> None:
        r = subprocess.run(
            [str(REPO_ROOT / "tools" / "a20"), "complete", "run", "xfce"],
            capture_output=True, text=True, check=True)
        self.assertIn("xfce-riscv64", r.stdout.splitlines())

    def test_completion_subcommand_prints_script(self) -> None:
        r = subprocess.run(
            [str(REPO_ROOT / "tools" / "a20"), "completion"],
            capture_output=True, text=True, check=True)
        self.assertIn("complete -o bashdefault -o default", r.stdout)
        self.assertIn("_a20_completions() {", r.stdout)

    def test_hidden_complete_not_in_help(self) -> None:
        r = subprocess.run(
            [str(REPO_ROOT / "tools" / "a20"), "-h"],
            capture_output=True, text=True, check=True)
        self.assertNotIn("complete\n", r.stdout)
        self.assertIn("completion", r.stdout)

    def test_bash_shim_completes_instances(self) -> None:
        # The full loop, driven the way bash drives it: COMP_WORDS set, the
        # shim sourced, COMPREPLY read back.
        script = subprocess.run(
            [str(REPO_ROOT / "tools" / "a20"), "completion"],
            capture_output=True, text=True, check=True).stdout
        probe = (
            script
            + 'COMP_WORDS=(tools/a20 run xfce); COMP_CWORD=2; _a20_completions\n'
            + 'echo "${COMPREPLY[*]}"\n')
        r = subprocess.run(["bash", "-c", probe],
                           capture_output=True, text=True, check=True)
        self.assertIn("xfce-riscv64", r.stdout.split())


if __name__ == "__main__":
    unittest.main()
