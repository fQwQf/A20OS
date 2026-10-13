#!/usr/bin/env python3
"""Run a pinned free OpenCode model with project-scoped, non-writing policy."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys

MODEL = "opencode/space-bunny-free"
PROVIDER = "opencode"
MODEL_SHORT = "space-bunny-free"
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def executable() -> Path:
    env_path = os.environ.get("OPENCODE_BIN")
    candidates = [Path(env_path)] if env_path else []
    path = shutil.which("opencode")
    if path:
        candidates.append(Path(path))
    candidates.append(Path.home() / ".opencode/bin/opencode")
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise RuntimeError("OpenCode executable not found; install it from https://opencode.ai/docs")


def parse_catalog(text: str) -> dict:
    marker = f"{MODEL}"
    pos = text.find(marker)
    if pos < 0:
        raise RuntimeError(f"Pinned model {MODEL} is absent from the live OpenCode catalog")
    start = text.find("{", pos)
    if start < 0:
        raise RuntimeError("Catalog entry has no metadata object")
    depth = 0
    quoted = False
    escaped = False
    end = None
    for i in range(start, len(text)):
        c = text[i]
        if quoted:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end is None:
        raise RuntimeError("Catalog entry is incomplete")
    item = json.loads(text[start:end])
    cost = item.get("cost", {})
    cache = cost.get("cache", {})
    if item.get("id") != MODEL_SHORT or item.get("providerID") != PROVIDER:
        raise RuntimeError("Live catalog resolved a different provider or model")
    if item.get("status") != "active":
        raise RuntimeError("Pinned free model is not marked active")
    if any(value != 0 for value in (cost.get("input"), cost.get("output"), cache.get("read"), cache.get("write"))):
        raise RuntimeError("Pinned model is no longer zero-cost; request was not sent")
    return {"id": item["id"], "name": item.get("name"), "providerID": item["providerID"], "status": item["status"], "cost": cost}


def session_id_from_events(raw: str) -> str | None:
    for line in raw.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        info = event.get("sessionID") or event.get("sessionId")
        if info:
            return str(info)
        part = event.get("part")
        if isinstance(part, dict):
            info = part.get("sessionID") or part.get("sessionId")
            if info:
                return str(info)
        for key in ("properties", "data"):
            obj = event.get(key)
            if isinstance(obj, dict):
                info = obj.get("sessionID") or obj.get("sessionId")
                if info:
                    return str(info)
    return None


def event_model_matches(raw: str) -> bool:
    found = False
    for line in raw.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        candidates = [item]
        for key in ("info", "message", "part", "data", "properties"):
            obj = item.get(key)
            if isinstance(obj, dict):
                candidates.append(obj)
        for obj in candidates:
            provider = obj.get("providerID") or obj.get("providerId") or obj.get("provider")
            model = obj.get("modelID") or obj.get("modelId") or obj.get("model")
            if isinstance(model, dict):
                provider = model.get("providerID") or provider
                model = model.get("modelID") or model.get("id")
            if provider or model:
                if provider == PROVIDER and model in (MODEL_SHORT, MODEL):
                    found = True
                elif provider and model:
                    raise RuntimeError(f"Response event reports unexpected provider/model: {provider}/{model}")
    return found


def error_from_events(raw: str) -> dict | None:
    """Summarize a structured API error without exposing request credentials."""
    for line in raw.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        error = item.get("error")
        if not isinstance(error, dict):
            continue
        data = error.get("data") if isinstance(error.get("data"), dict) else error
        message = data.get("message")
        status = data.get("statusCode")
        name = error.get("name") or error.get("type")
        return {"name": name, "status_code": status, "message": message}
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("smoke", "readonly"), required=True)
    parser.add_argument("--message", help="Prompt for readonly mode; smoke has a fixed one-line prompt")
    args = parser.parse_args()
    if args.mode == "smoke" and args.message:
        parser.error("smoke mode has a fixed prompt; omit --message")
    if args.mode == "readonly" and not args.message:
        parser.error("readonly mode requires a task prompt with exact paths")

    exe = executable()
    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + secrets.token_hex(4)
    run_dir = HERE / "runs" / run_id
    run_dir.mkdir(parents=True, mode=0o700)
    (run_dir / "config-home").mkdir(mode=0o700)
    (run_dir / "config-dir").mkdir(mode=0o700)
    os.chmod(HERE / "runs", 0o700)
    os.umask(0o077)

    env = os.environ.copy()
    env["OPENCODE_CONFIG"] = str(HERE / ("opencode-free-smoke.json" if args.mode == "smoke" else "opencode-free-readonly.json"))
    env["OPENCODE_CONFIG_DIR"] = str(run_dir / "config-dir")
    env["XDG_CONFIG_HOME"] = str(run_dir / "config-home")
    env["OPENCODE_DISABLE_DEFAULT_PLUGINS"] = "true"
    env.pop("OPENCODE_MODEL", None)

    # Fail closed before any completion request if the live, installed catalog has changed.
    catalog_cmd = [str(exe), "models", PROVIDER, "--verbose"]
    catalog = subprocess.run(catalog_cmd, cwd=ROOT, env=env, text=True, capture_output=True)
    if catalog.returncode:
        (run_dir / "catalog.stderr.txt").write_text(catalog.stderr, encoding="utf-8")
        print(f"catalog_check_failed exit={catalog.returncode}; details in {run_dir}", file=sys.stderr)
        return catalog.returncode
    try:
        model_meta = parse_catalog(catalog.stdout)
    except Exception as exc:
        (run_dir / "catalog.txt").write_text(catalog.stdout, encoding="utf-8")
        print(f"catalog_check_failed: {exc}; no completion request sent", file=sys.stderr)
        return 2
    (run_dir / "catalog-model.json").write_text(json.dumps(model_meta, indent=2)+"\n", encoding="utf-8")

    message = "Reply with exactly READY and no other text." if args.mode == "smoke" else args.message
    cmd = [str(exe), "run", "--pure", "--model", MODEL, "--agent", "free-smoke" if args.mode == "smoke" else "free-readonly", "--format", "json", "--dir", str(ROOT), message]
    (run_dir / "request.txt").write_text(message+"\n", encoding="utf-8")
    (run_dir / "command.json").write_text(json.dumps({"executable":str(exe),"version":subprocess.run([str(exe),"--version"],capture_output=True,text=True).stdout.strip(),"argv":cmd,"cwd":str(ROOT),"provider":PROVIDER,"model":MODEL,"mode":args.mode},indent=2)+"\n", encoding="utf-8")
    result = subprocess.run(cmd, cwd=ROOT, env=env, text=True, capture_output=True)
    (run_dir / "events.jsonl").write_text(result.stdout, encoding="utf-8")
    (run_dir / "stderr.txt").write_text(result.stderr, encoding="utf-8")
    event_stream = result.stdout + "\n" + result.stderr
    session = session_id_from_events(event_stream)
    metadata = {"run_id":run_id,"mode":args.mode,"provider":PROVIDER,"model":MODEL,"catalog":model_meta,"exit_code":result.returncode,"session_id":session,"event_model_confirmed":event_model_matches(event_stream),"api_error":error_from_events(event_stream),"run_dir":str(run_dir)}
    if session:
        exported = subprocess.run([str(exe),"export",session],cwd=ROOT,env=env,text=True,capture_output=True)
        if exported.returncode == 0:
            (run_dir / "session-export.json").write_text(exported.stdout,encoding="utf-8")
            metadata["session_exported"] = True
        else:
            (run_dir / "export.stderr.txt").write_text(exported.stderr,encoding="utf-8")
            metadata["session_exported"] = False
    else:
        metadata["session_exported"] = False
    (run_dir / "result.json").write_text(json.dumps(metadata,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(metadata,ensure_ascii=False))
    return result.returncode

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"opencode runner error: {exc}",file=sys.stderr)
        raise SystemExit(2)
