#!/usr/bin/env python3
"""Run one zero-tool OpenCode v2 Step 5 connectivity probe after free-price checks."""
from __future__ import annotations

import datetime as dt
from html.parser import HTMLParser
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import urllib.request

VERSION = "2.0.28"
PROVIDER = "opencode"
MODEL_ID = "step-5-preview-free"
MODEL = f"{PROVIDER}/{MODEL_ID}"
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PRICING_URL = "https://dev.opencode.ai/v2/docs/console/models/"
MODELS_URL = "https://opencode.ai/inference/v1/models"
DEFAULT_BIN = Path.home() / ".opencode/versions/v2.0.28/node_modules/.bin/opencode2"
ERROR_TEXT = re.compile(
    r"FreeTierError|provider\.auth|HTTP\s+4\d\d|status(?:_code)?[\"':= ]+4\d\d",
    re.IGNORECASE,
)


class PricingRows(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.rows: list[list[str]] = []
        self._row: list[str] | None = None
        self._cell = False
        self.text: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "tr":
            self._row = []
        elif tag in ("td", "th") and self._row is not None:
            self._cell = True
            self._row.append("")

    def handle_data(self, data: str) -> None:
        self.text.append(data)
        if self._cell and self._row:
            self._row[-1] += data

    def handle_endtag(self, tag: str) -> None:
        if tag in ("td", "th"):
            self._cell = False
        elif tag == "tr" and self._row is not None:
            self.rows.append([" ".join(cell.split()) for cell in self._row])
            self._row = None


def live_price_check(catalog_raw: bytes, pricing_raw: bytes) -> dict:
    catalog = json.loads(catalog_raw)
    models = catalog.get("data")
    if not isinstance(models, list):
        raise RuntimeError("Official model catalog has no data list")
    matches = [item for item in models if item.get("id") == MODEL_ID]
    if len(matches) != 1 or matches[0].get("owned_by") != PROVIDER:
        raise RuntimeError("Step 5 is absent or ambiguous in the current OpenCode model catalog")

    parser = PricingRows()
    parser.feed(pricing_raw.decode("utf-8", errors="strict"))
    rows = [row for row in parser.rows
            if len(row) >= 5 and row[0] == "Step 5 Preview Free"]
    if len(rows) != 1:
        raise RuntimeError("Official pricing page has no unique complete Step 5 row")
    row = rows[0]
    if row[1:4] != ["Free", "Free", "Free"] or row[4] not in ("Free", "-"):
        raise RuntimeError("Step 5 pricing is no longer explicitly zero; request was not sent")
    page_text = " ".join(" ".join(parser.text).split())
    if "Step 5 Preview Free is free on OpenCode for a limited time" not in page_text:
        raise RuntimeError("Official page no longer confirms Step 5's limited-time free status")
    return {
        "model": matches[0],
        "pricing_row": row,
        "limited_time_free": True,
        "pricing_url": PRICING_URL,
        "catalog_url": MODELS_URL,
        "pricing_sha256": hashlib.sha256(pricing_raw).hexdigest(),
        "catalog_sha256": hashlib.sha256(catalog_raw).hexdigest(),
        "checked_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
    }


def structured_errors(raw: str) -> list[dict]:
    errors = []
    for line in raw.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(item, dict):
            continue
        error = item.get("error")
        if item.get("type") == "error" or isinstance(error, dict):
            info = error if isinstance(error, dict) else item
            errors.append({
                "type": info.get("type") or info.get("name"),
                "message": info.get("message"),
                "status": info.get("status") or info.get("statusCode") or info.get("status_code"),
            })
    return errors


def completion_success(exit_code: int, stdout: str, stderr: str) -> tuple[bool, list[dict]]:
    combined = stdout + "\n" + stderr
    errors = structured_errors(combined)
    if ERROR_TEXT.search(combined):
        errors.append({"type": "service_error_text", "message": "provider/service error text found"})
    success = exit_code == 0 and "STEP5_OK" in stdout and not errors
    return success, errors


def executable() -> Path:
    candidate = Path(os.environ.get("OPENCODE_V2_BIN", str(DEFAULT_BIN)))
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        raise RuntimeError(f"OpenCode v{VERSION} binary not found at {candidate}")
    version = subprocess.run([str(candidate), "--version"], text=True, capture_output=True, check=True)
    if f"v{VERSION}" not in version.stdout and f" {VERSION}" not in version.stdout:
        raise RuntimeError(f"Expected OpenCode v{VERSION}; found {version.stdout.strip()}")
    return candidate.resolve()


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": f"A20OS-OpenCode-Preflight/{VERSION}"})
    with urllib.request.urlopen(request, timeout=20) as response:
        if response.status != 200:
            raise RuntimeError(f"Official preflight source returned HTTP {response.status}")
        return response.read()


def session_id_from_events(raw: str) -> str | None:
    for line in raw.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            session = event.get("sessionID") or event.get("sessionId")
            if session:
                return str(session)
    return None


def main() -> int:
    if len(sys.argv) != 1:
        print("This runner accepts no prompt or file arguments; it performs one fixed, zero-tool probe.", file=sys.stderr)
        return 2

    exe = executable()
    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + secrets.token_hex(4)
    run_dir = HERE / "runs" / run_id
    run_dir.mkdir(parents=True, mode=0o700)
    os.chmod(HERE / "runs", 0o700)
    os.umask(0o077)
    (run_dir / "config-dir").mkdir(mode=0o700)
    for name in ("config", "data", "state", "cache"):
        (run_dir / "xdg" / name).mkdir(parents=True, mode=0o700)

    try:
        catalog_raw = fetch(MODELS_URL)
        pricing_raw = fetch(PRICING_URL)
        preflight = live_price_check(catalog_raw, pricing_raw)
    except Exception as exc:
        (run_dir / "preflight-error.txt").write_text(f"{type(exc).__name__}: {exc}\n", encoding="utf-8")
        print(f"preflight_failed; no completion request sent; details in {run_dir}", file=sys.stderr)
        return 2

    config = HERE / "opencode-v2-step5-smoke.json"
    (run_dir / "pricing-source.html").write_bytes(pricing_raw)
    (run_dir / "live-model-catalog.json").write_bytes(catalog_raw)
    (run_dir / "preflight.json").write_text(json.dumps(preflight, indent=2) + "\n", encoding="utf-8")
    (run_dir / "config.json").write_text(config.read_text(encoding="utf-8"), encoding="utf-8")

    isolated_home = run_dir / "home"
    isolated_home.mkdir(mode=0o700)
    # Do not inherit API-key/token variables or the user's existing OpenCode
    # environment. Keep only the executable search path and non-secret locale
    # / trust-store settings needed to run Node and validate TLS certificates.
    env = {key: value for key, value in os.environ.items()
           if key in ("PATH", "LANG", "LC_ALL", "TZ", "SSL_CERT_FILE", "SSL_CERT_DIR")}
    env.update({
        "HOME": str(isolated_home),
        "OPENCODE_CONFIG": str(config),
        "OPENCODE_CONFIG_DIR": str(run_dir / "config-dir"),
        "OPENCODE_DISABLE_DEFAULT_PLUGINS": "true",
        "XDG_CONFIG_HOME": str(run_dir / "xdg" / "config"),
        "XDG_DATA_HOME": str(run_dir / "xdg" / "data"),
        "XDG_STATE_HOME": str(run_dir / "xdg" / "state"),
        "XDG_CACHE_HOME": str(run_dir / "xdg" / "cache"),
    })
    env.pop("OPENCODE_MODEL", None)
    prompt = "Respond with exactly: STEP5_OK"
    with tempfile.TemporaryDirectory(prefix="opencode-v2-empty-") as empty_cwd:
        model_check = subprocess.run([str(exe), "models", "--standalone"], cwd=empty_cwd, env=env,
                                     text=True, capture_output=True, timeout=60)
        (run_dir / "models.stdout").write_text(model_check.stdout, encoding="utf-8")
        (run_dir / "models.stderr").write_text(model_check.stderr, encoding="utf-8")
        if model_check.returncode or MODEL not in model_check.stdout:
            (run_dir / "preflight-error.txt").write_text(
                "Configured v2 CLI did not expose the required model; request not sent.\n", encoding="utf-8")
            print(f"model_check_failed; no completion request sent; details in {run_dir}", file=sys.stderr)
            return 2
        command = [str(exe), "run", "--standalone", "--model", MODEL,
                   "--agent", "free-probe", "--format", "json", prompt]
        (run_dir / "request.txt").write_text(prompt + "\n", encoding="utf-8")
        (run_dir / "command.json").write_text(json.dumps({
            "argv": command,
            "binary": str(exe),
            "version": VERSION,
            "provider": PROVIDER,
            "model": MODEL,
            "config": str(config),
            "cwd": empty_cwd,
            "tools": "all denied",
        }, indent=2) + "\n", encoding="utf-8")
        result = subprocess.run(command, cwd=empty_cwd, env=env, text=True, capture_output=True, timeout=180)

    (run_dir / "events.jsonl").write_text(result.stdout, encoding="utf-8")
    (run_dir / "stderr.txt").write_text(result.stderr, encoding="utf-8")
    session = session_id_from_events(result.stdout + "\n" + result.stderr)
    success, errors = completion_success(result.returncode, result.stdout, result.stderr)
    metadata = {
        "run_id": run_id,
        "version": VERSION,
        "provider": PROVIDER,
        "model": MODEL,
        "exit_code": result.returncode,
        "session_id": session,
        "success": success,
        "errors": errors,
        "preflight": preflight,
        "run_dir": str(run_dir),
    }
    (run_dir / "result.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, ensure_ascii=False))
    return 0 if success else (result.returncode or 1)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"OpenCode v2 runner error: {type(exc).__name__}: {exc}", file=sys.stderr)
        raise SystemExit(2)
