#!/usr/bin/env python3
"""Run local shell, pip and npm workloads in the existing pynode guest world.

No network dependency or system installation: packages, caches and outputs
stay in a temporary directory. Also usable on Linux as a behavior reference.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(argv, *, cwd=None, env=None):
    result = subprocess.run(argv, cwd=cwd, env=env, capture_output=True,
                            text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f"{argv[0]} exited {result.returncode}: "
                           + (result.stdout + result.stderr)[-4000:])
    return result.stdout


def shell_pipeline():
    output = run(["/bin/sh", "-c",
                  "set -e; x=$(printf 'one\\ntwo\\n' | cat); "
                  "test \"$x\" = \"$(printf 'one\\ntwo')\"; "
                  "sh -c 'exit 7' & p=$!; "
                  "if wait \"$p\"; then exit 9; else test $? = 7; fi; "
                  "printf 'SHELL_PIPE_WAIT_OK\\n'"])
    assert output == "SHELL_PIPE_WAIT_OK\n", repr(output)


def python_install():
    with tempfile.TemporaryDirectory(prefix="a20-pip-qa-") as directory:
        root = Path(directory)
        package = root / "package"
        package.mkdir()
        (package / "pyproject.toml").write_text(
            '[build-system]\nrequires=["setuptools"]\n'
            'build-backend="setuptools.build_meta"\n')
        (package / "setup.py").write_text(
            'from pathlib import Path\nfrom setuptools import setup\n'
            'Path(__file__).with_name("build-ran").write_text("ok")\n'
            'setup(name="a20-runtime-qa",version="1.0",py_modules=["a20_runtime_qa"])\n')
        (package / "a20_runtime_qa.py").write_text('def value():\n    return 42\n')
        target = root / "installed"
        env = dict(os.environ, PIP_CONFIG_FILE=os.devnull,
                   PIP_DISABLE_PIP_VERSION_CHECK="1", PIP_NO_INDEX="1",
                   PIP_CACHE_DIR=str(root / "pip-cache"),
                   PYTHONPYCACHEPREFIX=str(root / "pycache"))
        run([sys.executable, "-m", "pip", "install", "--no-index", "--no-deps",
             "--no-build-isolation", "--target", str(target), str(package)], env=env)
        assert (package / "build-ran").read_text() == "ok"
        env["PYTHONPATH"] = str(target)
        output = run([sys.executable, "-c",
                      "import a20_runtime_qa; assert a20_runtime_qa.value()==42; "
                      "print('PYTHON_INSTALLED_IMPORT_OK')"], env=env)
        assert output == "PYTHON_INSTALLED_IMPORT_OK\n", repr(output)


def node_install():
    with tempfile.TemporaryDirectory(prefix="a20-npm-qa-") as directory:
        root = Path(directory)
        (root / "package.json").write_text(json.dumps({
            "name": "a20-runtime-qa", "version": "1.0.0", "private": True,
            "scripts": {"postinstall": "node install.cjs"}}))
        (root / "install.cjs").write_text(r'''
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');
(async () => {
    const child = await promisify(execFile)(process.execPath,
        ['-e', 'process.stdout.write("NODE_CHILD_OK")']);
    assert.equal(child.stdout, 'NODE_CHILD_OK');
    await new Promise(resolve => setTimeout(resolve, 10));
    await fs.writeFile('installed-marker', '42');
    assert.equal(await fs.readFile('installed-marker', 'utf8'), '42');
    console.log('NODE_INSTALL_CHILD_TIMER_IO_OK');
})().catch(error => { console.error(error); process.exitCode = 1; });
''')
        userconfig, globalconfig = root / "npm-user.rc", root / "npm-global.rc"
        userconfig.write_text(""); globalconfig.write_text("")
        env = dict(os.environ, npm_config_cache=str(root / "npm-cache"),
                   npm_config_userconfig=str(userconfig),
                   npm_config_globalconfig=str(globalconfig))
        output = run(["npm", "install", "--offline", "--no-audit", "--no-fund",
                      "--ignore-scripts=false"], cwd=root, env=env)
        assert "NODE_INSTALL_CHILD_TIMER_IO_OK" in output.splitlines(), output
        assert (root / "installed-marker").read_text() == "42"


def desktop_abi():
    import desktop_abi_probe
    desktop_abi_probe.exec_scripts()
    desktop_abi_probe.shm_after_rmid()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="append",
                        choices=("shell", "python-install", "node-install", "desktop-abi"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    checks = {"shell": shell_pipeline, "python-install": python_install,
              "node-install": node_install, "desktop-abi": desktop_abi}
    results = {}
    for name in args.check or checks:
        try:
            checks[name]()
            results[name] = {"status": "PASS"}
        except Exception as error:
            results[name] = {"status": "FAIL", "error": repr(error)}
        print("APPLICATION_RUNTIME:", name, results[name]["status"], flush=True)
    report = json.dumps(results, indent=2)
    if args.output:
        args.output.write_text(report + "\n")
    print(report, flush=True)
    passed = all(item["status"] == "PASS" for item in results.values())
    print("APPLICATION_RUNTIME: " + ("PASS" if passed else "FAIL"), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
