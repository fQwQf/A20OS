#!/usr/bin/env python3
"""Host differential for the WAIT4_IMPL slice (C reference vs Rust controller).

Builds two binaries from the same harness (tools/tests/wait4_host.c):

  - variant "c":    the retained C reference engine extracted textually from
                    kernel/proc/wait.c (WAIT4_COMMON + WAIT4_C_ENGINE regions);
  - variant "rust": the WAIT4_RUST adapter extracted from the same file,
                    linked against the production Rust controller object
                    built from kernel/proc/wait4_state.rs.

Both variants run the identical generated scenario table against the same
deterministic stub world (locks, chain/field sampling, pin/detach/destroy/put,
group/current predicates, park protocol, yield).  The driver requires
byte-identical stdout (results, status, ordered stub-call traces, final world
digests) and checks the expected result of every scripted run.
"""
import difflib
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
WAIT_SOURCE = ROOT / "kernel/proc/wait.c"

WNOHANG = 1
WUNTRACED = 2
WCONTINUED = 8
WNOWAIT = 0x1000000
WNOTHREAD = 0x20000000


def region(source: str, begin: str, end: str) -> str:
    start = source.index(begin) + len(begin)
    stop = source.index(end)
    if stop <= start:
        raise ValueError(f"empty region {begin}..{end}")
    return source[start:stop]


def self_and_child(state: str, pid: int = 11, exit_code: int = 7,
                   pgid: int = 50) -> str:
    return f"""
    task_t *self = wt(world, 0); wcfg(self, 100, 100, 0, 50, W_PROC_RUNNING);
    task_t *c = wt(world, 1); wcfg(c, {pid}, {pid}, 0, {pgid}, {state});
    c->exit_code = {exit_code};
    wlink(self, c);
    """


SCENARIOS = [
    {
        "name": "reap_zombie_any",
        "setup": """
    task_t *self = wt(world, 0); wcfg(self, 100, 100, 0, 50, W_PROC_RUNNING);
    task_t *c = wt(world, 1); wcfg(c, 11, 11, 0, 50, W_PROC_ZOMBIE);
    c->exit_code = 7;
    c->total_time = 111;
    c->stime_ticks = 22;
    wlink(self, c);
    """,
        "runs": [{
            "pid": -1, "options": "0", "status": True,
            "expect": (11, 0x700),
            "events": ["tl_lock", "state:11=5", "gdead:11=1",
                       "current_cpu:11=0", "pl_lock:11", "get:11",
                       "pl_unlock:11", "detach:11", "tl_unlock",
                       "destroy:11", "put:11"],
        }],
    },
    {
        "name": "echild_pid_mismatch",
        "setup": self_and_child("W_PROC_ZOMBIE"),
        "runs": [
            {"pid": 12, "options": "0", "status": True,
             "expect": (-10, -777), "absent": ["detach:", "destroy:", "put:"]},
            {"pid": -1, "options": "0", "status": True,
             "expect": (11, 0x700)},
        ],
    },
    {
        "name": "selector_zero_pgid",
        "setup": self_and_child("W_PROC_ZOMBIE"),
        "runs": [{"pid": 0, "options": "0", "status": True,
                  "expect": (11, 0x700)}],
    },
    {
        "name": "selector_pgid_mismatch",
        "setup": self_and_child("W_PROC_ZOMBIE", pgid=51),
        "runs": [
            {"pid": 0, "options": "0", "status": True, "expect": (-10, -777)},
            {"pid": -50, "options": "0", "status": True,
             "expect": (-10, -777)},
            {"pid": -51, "options": "0", "status": True,
             "expect": (11, 0x700)},
        ],
    },
    {
        "name": "selector_pid_positive",
        "setup": self_and_child("W_PROC_ZOMBIE"),
        "runs": [{"pid": 11, "options": "0", "status": True,
                  "expect": (11, 0x700)}],
    },
    {
        "name": "wnohang_running",
        "setup": self_and_child("W_PROC_RUNNING"),
        "runs": [{"pid": -1, "options": "WNOHANG", "status": True,
                  "expect": (0, -777)}],
    },
    {
        "name": "wnohang_no_children_echild_precedence",
        "setup": """
    task_t *self = wt(world, 0); wcfg(self, 100, 100, 0, 50, W_PROC_RUNNING);
    """,
        "runs": [
            {"pid": -1, "options": "WNOHANG", "status": True,
             "expect": (-10, -777)},
            {"pid": -1, "options": "0", "status": True,
             "expect": (-10, -777), "events": ["tl_lock", "tl_unlock"]},
        ],
    },
    {
        "name": "group_dying_then_dead",
        "setup": """
    task_t *self = wt(world, 0); wcfg(self, 100, 100, 0, 50, W_PROC_RUNNING);
    task_t *c = wt(world, 1); wcfg(c, 11, 11, 0, 50, W_PROC_ZOMBIE);
    c->exit_code = 7;
    wlink(self, c);
    world->group_dead = 0;
    """,
        "runs": [
            {"pid": -1, "options": "WNOHANG", "status": True,
             "expect": (0, -777), "absent": ["get:", "detach:"]},
            {"pid": -1, "options": "WNOHANG", "status": True,
             "expect": (11, 0x700), "pre": "pre_group_dead"},
        ],
    },
    {
        "name": "reap_pending_yield",
        "setup": self_and_child("W_PROC_ZOMBIE") + """
    world->current_hits = 1;
    """,
        "runs": [{"pid": -1, "options": "0", "status": True,
                  "expect": (11, 0x700),
                  "events": ["current_cpu:11=1", "yield",
                             "current_cpu:11=0", "get:11"]}],
    },
    {
        "name": "reap_pending_two_hits",
        "setup": self_and_child("W_PROC_ZOMBIE") + """
    world->current_hits = 2;
    """,
        "runs": [{"pid": -1, "options": "0", "status": True,
                  "expect": (11, 0x700),
                  "events": ["yield", "yield", "get:11", "detach:11"]}],
    },
    {
        "name": "wnowait_then_reap",
        "setup": self_and_child("W_PROC_ZOMBIE", exit_code=3),
        "runs": [
            {"pid": -1, "options": "WNOWAIT", "status": True,
             "expect": (11, 0x300),
             "absent": ["get:", "detach:", "destroy:", "put:"],
             "events": ["pl_lock:11", "pl_unlock:11", "tl_unlock"]},
            {"pid": -1, "options": "0", "status": True,
             "expect": (11, 0x300),
             "events": ["get:11", "detach:11", "destroy:11", "put:11"]},
        ],
    },
    {
        "name": "wnowait_status_null",
        "setup": self_and_child("W_PROC_ZOMBIE", exit_code=3),
        "runs": [
            {"pid": -1, "options": "WNOWAIT", "status": False,
             "expect": (11, None)},
            {"pid": -1, "options": "0", "status": True,
             "expect": (11, 0x300)},
        ],
    },
    {
        "name": "pin_fail_echild_then_reap",
        "setup": self_and_child("W_PROC_ZOMBIE"),
        "runs": [
            {"pid": -1, "options": "0", "status": True,
             "expect": (-10, 0x700), "pre": "pre_get_fail",
             "events": ["pl_lock:11", "get_fail:11", "pl_unlock:11",
                        "tl_unlock"],
             "absent": ["detach:", "destroy:", "put:"]},
            {"pid": -1, "options": "0", "status": True,
             "expect": (11, 0x700), "events": ["get:11", "detach:11"]},
        ],
    },
    {
        "name": "stop_wuntraced_report",
        "setup": self_and_child("W_PROC_STOPPED", exit_code=19) + """
    c->stop_report_pending = 1;
    """,
        "runs": [
            {"pid": -1, "options": "WUNTRACED", "status": True,
             "expect": (11, 0x137F),
             "events": ["state:11=4", "pl_lock:11", "pl_unlock:11", "tl_unlock"],
             "absent": ["detach:", "destroy:", "put:"]},
            {"pid": -1, "options": "WNOHANG | WUNTRACED", "status": True,
             "expect": (0, -777)},
        ],
    },
    {
        "name": "stop_ignored_parks_then_exit",
        "setup": self_and_child("W_PROC_STOPPED", exit_code=19) + """
    c->stop_report_pending = 1;
    g_hook_slot = 1;
    """,
        "runs": [{
            "pid": -1, "options": "0", "status": True,
            "expect": (11, 0x400), "pre": "pre_event_exit_child",
            "events": ["pl_lock:100", "prepare:1", "sigchk:100=0",
                       "pl_unlock:100", "tl_unlock", "commit:1", "finish",
                       "pl_lock:100", "pl_unlock:100", "state:11=5",
                       "get:11", "detach:11", "tl_unlock", "destroy:11",
                       "put:11"],
        }],
    },
    {
        "name": "stop_wnohang_ignored",
        "setup": self_and_child("W_PROC_STOPPED", exit_code=19) + """
    c->stop_report_pending = 1;
    """,
        "runs": [{"pid": -1, "options": "WNOHANG", "status": True,
                  "expect": (0, -777), "absent": ["destroy:", "put:"]}],
    },
    {
        "name": "ptrace_stop_without_wuntraced",
        "setup": self_and_child("W_PROC_STOPPED", exit_code=19) + """
    c->stop_report_pending = 1;
    c->ptrace_stop_active = 1;
    c->ptrace_event = 7;
    """,
        "runs": [{"pid": -1, "options": "0", "status": True,
                  "expect": (11, (7 << 16) | (5 << 8) | 0x7F)}],
    },
    {
        "name": "cont_report",
        "setup": self_and_child("W_PROC_READY", exit_code=0) + """
    c->continue_report_pending = 1;
    """,
        "runs": [
            {"pid": -1, "options": "WCONTINUED", "status": True,
             "expect": (11, 0xFFFF), "absent": ["destroy:", "put:"]},
            {"pid": -1, "options": "WCONTINUED | WNOHANG", "status": True,
             "expect": (0, -777)},
        ],
    },
    {
        "name": "stop_beats_cont",
        "setup": self_and_child("W_PROC_STOPPED", exit_code=19) + """
    c->stop_report_pending = 1;
    c->continue_report_pending = 1;
    """,
        "runs": [{"pid": -1, "options": "WUNTRACED | WCONTINUED",
                  "status": True, "expect": (11, 0x137F)}],
    },
    {
        "name": "stop_wnowait_keeps_pending",
        "setup": self_and_child("W_PROC_STOPPED", exit_code=19) + """
    c->stop_report_pending = 1;
    """,
        "runs": [
            {"pid": -1, "options": "WUNTRACED | WNOWAIT", "status": True,
             "expect": (11, 0x137F)},
            {"pid": -1, "options": "WUNTRACED", "status": True,
             "expect": (11, 0x137F)},
        ],
    },
    {
        "name": "signal_before_park",
        "setup": self_and_child("W_PROC_RUNNING"),
        "runs": [{
            "pid": -1, "options": "0", "status": True,
            "expect": (-512, -777), "pre": "pre_signal_pending",
            "events": ["pl_lock:100", "prepare:1", "sigchk:100=1",
                       "wake:100", "pl_unlock:100", "tl_unlock", "commit:1",
                       "finish", "pl_lock:100", "pl_unlock:100"],
        }],
    },
    {
        "name": "task_exit_wake",
        "setup": self_and_child("W_PROC_RUNNING"),
        "runs": [{
            "pid": -1, "options": "0", "status": True,
            "expect": (-512, -777), "pre": "pre_task_exit_wake",
            "events": ["commit:6", "finish"],
        }],
    },
    {
        "name": "park_event_child_exit",
        "setup": self_and_child("W_PROC_RUNNING") + """
    g_hook_slot = 1;
    """,
        "runs": [{
            "pid": -1, "options": "0", "status": True,
            "expect": (11, 0x400), "pre": "pre_event_exit_child",
            "events": ["commit:1", "finish",
                       "state:11=5", "get:11", "detach:11", "destroy:11",
                       "put:11"],
        }],
    },
    {
        "name": "group_member_scope",
        "setup": """
    task_t *self = wt(world, 0); wcfg(self, 100, 100, 0, 50, W_PROC_RUNNING);
    task_t *m = wt(world, 2); wcfg(m, 101, 100, 0, 50, W_PROC_RUNNING);
    wmember(self, m);
    task_t *c = wt(world, 1); wcfg(c, 11, 11, 101, 50, W_PROC_ZOMBIE);
    c->exit_code = 9;
    wlink(m, c);
    """,
        "runs": [
            {"pid": -1, "options": "__WNOTHREAD", "status": True,
             "expect": (-10, -777), "absent": ["detach:"]},
            {"pid": -1, "options": "0", "status": True,
             "expect": (11, 0x900), "events": ["state:11=5", "get:11"]},
        ],
    },
    {
        "name": "target_identity",
        "setup": """
    task_t *self = wt(world, 0); wcfg(self, 100, 100, 0, 50, W_PROC_RUNNING);
    task_t *c1 = wt(world, 1); wcfg(c1, 11, 11, 0, 50, W_PROC_ZOMBIE);
    c1->exit_code = 7;
    task_t *c2 = wt(world, 2); wcfg(c2, 12, 12, 0, 50, W_PROC_ZOMBIE);
    c2->exit_code = 8;
    wlink(self, c1);
    wlink(self, c2);
    """,
        "runs": [
            {"pid": -1, "options": "0", "target": 2, "status": True,
             "expect": (12, 0x800), "events": ["get:12", "detach:12"]},
            {"pid": -1, "options": "0", "target": 2, "status": True,
             "expect": (-10, -777), "absent": ["get:11", "detach:11"]},
            {"pid": -1, "options": "0", "target": 1, "status": True,
             "expect": (11, 0x700), "events": ["get:11", "detach:11"]},
        ],
    },
    {
        "name": "unused_child_skipped",
        "setup": self_and_child("W_PROC_UNUSED"),
        "runs": [
            {"pid": -1, "options": "WNOHANG", "status": True,
             "expect": (-10, -777)},
            {"pid": -1, "options": "0", "status": True,
             "expect": (-10, -777)},
        ],
    },
    {
        "name": "wnothread_own_child",
        "setup": self_and_child("W_PROC_ZOMBIE"),
        "runs": [{"pid": -1, "options": "__WNOTHREAD", "status": True,
                  "expect": (11, 0x700)}],
    },
    {
        "name": "scan_past_running_child",
        "setup": """
    task_t *self = wt(world, 0); wcfg(self, 100, 100, 0, 50, W_PROC_RUNNING);
    task_t *c1 = wt(world, 1); wcfg(c1, 11, 11, 0, 50, W_PROC_RUNNING);
    task_t *c2 = wt(world, 2); wcfg(c2, 12, 12, 0, 50, W_PROC_ZOMBIE);
    c2->exit_code = 8;
    wlink(self, c2);
    wlink(self, c1);
    """,
        "runs": [{"pid": -1, "options": "WNOHANG", "status": True,
                  "expect": (12, 0x800),
                  "events": ["state:11=2", "state:12=5", "get:12"]}],
    },
    {
        "name": "stop_pending_zero_ptrace_active",
        "setup": self_and_child("W_PROC_STOPPED", exit_code=19) + """
    c->ptrace_stop_active = 1;
    """,
        "runs": [{"pid": -1, "options": "WUNTRACED | WNOHANG", "status": True,
                  "expect": (0, -777)}],
    },
    {
        "name": "cont_pending_without_wcontinued",
        "setup": self_and_child("W_PROC_READY", exit_code=0) + """
    c->continue_report_pending = 1;
    """,
        "runs": [{"pid": -1, "options": "WNOHANG", "status": True,
                  "expect": (0, -777)}],
    },
    {
        "name": "status_null_zombie",
        "setup": self_and_child("W_PROC_ZOMBIE"),
        "runs": [{"pid": -1, "options": "0", "status": False,
                  "expect": (11, None)}],
    },
]


def emit_scenarios(scenarios):
    parts = []
    for i, sc in enumerate(scenarios):
        parts.append(f"static void sc_{i}_setup(w_world_t *world)\n{{\n"
                     f"    (void)world;\n{sc['setup']}\n}}")
    rows = []
    for i, sc in enumerate(scenarios):
        runs = []
        for r in sc["runs"]:
            target = r.get("target", None)
            runs.append("        { %d, %s, %d, %d, %s }," % (
                r["pid"], r["options"], -1 if target is None else target,
                1 if r["status"] else 0, r.get("pre", "NULL")))
        rows.append(
            "    {\n        .name = \"%s\",\n        .setup = sc_%d_setup,"
            "\n        .run_count = %d,\n        .runs = {\n%s\n        },\n"
            "    }," % (sc["name"], i, len(sc["runs"]), "\n".join(runs)))
    return ("\n".join(parts) + "\n\nstatic const wscenario_t g_scenarios[] = "
            "{\n" + "\n".join(rows) + "\n};\n"
            "static const int g_scenario_count = "
            "(int)(sizeof(g_scenarios) / sizeof(g_scenarios[0]));\n")


RUN_RE = re.compile(r"^RUN (\S+)\[(\d+)\] rc=(-?\d+) status=(-?\d+)$")


def parse_runs(output: str):
    runs = {}
    current = None
    for line in output.splitlines():
        m = RUN_RE.match(line)
        if m:
            current = (m.group(1), int(m.group(2)))
            runs[current] = {"rc": int(m.group(3)), "status": int(m.group(4)),
                             "events": []}
        elif line.startswith("  ev ") and current is not None:
            runs[current]["events"].append(line[5:])
        elif line and not line.startswith("  "):
            current = None
    return runs


def check_expectations(runs, scenarios):
    errors = []
    for sc in scenarios:
        for idx, run in enumerate(sc["runs"]):
            key = (sc["name"], idx)
            got = runs.get(key)
            if got is None:
                errors.append(f"{key}: missing run output")
                continue
            exp = run.get("expect")
            if exp is not None:
                rc_e, st_e = exp
                if got["rc"] != rc_e:
                    errors.append(
                        f"{key}: rc {got['rc']} != expected {rc_e}")
                if run["status"]:
                    if st_e is not None and got["status"] != st_e:
                        errors.append(
                            f"{key}: status {got['status']} != expected "
                            f"{st_e}")
                elif got["status"] != -777:
                    errors.append(
                        f"{key}: status written for a NULL slot: "
                        f"{got['status']}")
            pos = 0
            needles = list(run.get("events", []))
            for event in got["events"]:
                if pos < len(needles) and event.startswith(needles[pos]):
                    pos += 1
            if pos != len(needles):
                errors.append(
                    f"{key}: event order missing {needles[pos:]!r} in "
                    f"{got['events']!r}")
            for needle in run.get("absent", []):
                if any(e.startswith(needle) for e in got["events"]):
                    errors.append(f"{key}: unexpected event {needle} in "
                                  f"{got['events']!r}")
    return errors


def main() -> None:
    source = WAIT_SOURCE.read_text()
    common = region(source, "/* WAIT4_COMMON_BEGIN */",
                    "/* WAIT4_COMMON_END */")
    adapter = region(source, "/* WAIT4_ADAPTER_BEGIN */",
                     "/* WAIT4_ADAPTER_END */")
    c_engine = region(source, "/* WAIT4_C_ENGINE_BEGIN */",
                      "/* WAIT4_C_ENGINE_END */")
    for text, needle in ((common, "wait_accumulate_child_time"),
                         (common, "wait_task_tgid"),
                         (adapter, "wait4_adapter_child_view"),
                         (adapter, "a20_wait4_run"),
                         (c_engine, "proc_wait4_internal"),
                         (c_engine, "wait_child_matches_locked")):
        if needle not in text:
            raise ValueError(f"extraction lost {needle!r}")

    with tempfile.TemporaryDirectory(prefix="a20-wait4-host-") as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "engine_c.inc").write_text(common + c_engine)
        (tmp / "engine_rust.inc").write_text(common + adapter)
        (tmp / "scenario.inc").write_text(emit_scenarios(SCENARIOS))

        rust_obj = tmp / "wait4_state_host.o"
        subprocess.run([
            "rustc", "--edition=2021", "--crate-type", "lib", "--emit=obj",
            "--target", "x86_64-unknown-linux-gnu",
            "-C", "panic=abort", "-C", "opt-level=2",
            "-C", "codegen-units=1",
            str(ROOT / "kernel/proc/wait4_state.rs"), "-o", str(rust_obj),
        ], check=True, cwd=ROOT)

        base_cc = [
            "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            f"-I{ROOT / 'kernel'}", f"-I{tmp}",
            str(ROOT / "tools/tests/wait4_host.c"),
        ]
        outputs = {}
        for variant in ("c", "rust"):
            binary = tmp / f"wait4_host_{variant}"
            cmd = base_cc + [
                f'-DWAIT4_ENGINE_INC="engine_{variant}.inc"',
                '-DWAIT4_SCENARIO_INC="scenario.inc"',
                f'-DWAIT4_VARIANT="{variant}"',
                "-o", str(binary),
            ]
            if variant == "rust":
                cmd.append(str(rust_obj))
            subprocess.run(cmd, check=True, cwd=ROOT)
            result = subprocess.run([str(binary)], check=True, text=True,
                                    capture_output=True)
            outputs[variant] = result.stdout

        if outputs["c"] != outputs["rust"]:
            diff = list(difflib.unified_diff(
                outputs["c"].splitlines(), outputs["rust"].splitlines(),
                "variant-c", "variant-rust", lineterm=""))
            raise AssertionError(
                "C/Rust traces differ:\n" + "\n".join(diff[:80]))

        runs = parse_runs(outputs["c"])
        errors = check_expectations(runs, SCENARIOS)
        if errors:
            raise AssertionError("expectation failures:\n" + "\n".join(errors))

        total = sum(len(sc["runs"]) for sc in SCENARIOS)
        print(f"WAIT4_HOST: PASS ({len(SCENARIOS)} scenarios, {total} runs, "
              f"identical C/Rust traces, expectations held)")


if __name__ == "__main__":
    main()
