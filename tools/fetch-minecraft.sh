#!/bin/sh
# Fetch the Minecraft Java Edition client files from Mojang's own endpoints.
#
# Minecraft Java is not a single jar: a version is a client jar plus a set of
# libraries (LWJGL and friends) plus an asset store.  This script performs
# exactly the downloads the official launcher performs, from the same public
# manifest (piston-meta.mojang.com), and verifies every file against the SHA1
# that the manifest publishes.
#
# Playing requires a Minecraft account (sign in with the official launcher);
# this script only prepares the files for a licensed user and downloads
# nothing that Mojang does not already serve publicly.
#
#   tools/fetch-minecraft.sh [version] [outdir] [arch]
#   tools/fetch-minecraft.sh                     # default 1.21.11, x86_64
#   tools/fetch-minecraft.sh 26.3 build/mc/26.3  # needs Java 25
#
# Version choice matters for the JVM: the 1.21.x line needs Java 21 (what the
# xfce image ships), while the 26.x line already needs Java 25.
#
# arch picks the LWJGL natives unpacked into <outdir>/natives.  Mojang's
# manifest ships Linux natives for x86_64 only; aarch64 and riscv64 take the
# same LWJGL builds from Maven Central instead (riscv64 needs the 3.3.6
# release line — the first with riscv64 natives — which replaces the pinned
# 3.3.3 jars outright because LWJGL matches native and Java versions at load
# time).  loongarch64 fails here: LWJGL publishes no natives for it.
set -eu

VERSION=${1:-1.21.11}
OUT=${2:-build/minecraft/$VERSION}
ARCH=${3:-${A20_MC_ARCH:-x86_64}}
WITH_ASSETS=${WITH_ASSETS:-1}
MANIFEST=${MANIFEST:-https://piston-meta.mojang.com/mc/game/version_manifest_v2.json}

case "$ARCH" in
x86_64 | aarch64 | riscv64) ;;
loongarch64)
    echo "[minecraft] no LWJGL natives exist for loongarch64 — the client cannot run" >&2
    exit 1
    ;;
*)
    echo "[minecraft] unknown arch: $ARCH (want x86_64 | aarch64 | riscv64)" >&2
    exit 1
    ;;
esac

python3 - "$VERSION" "$OUT" "$WITH_ASSETS" "$MANIFEST" "$ARCH" <<'PY'
import concurrent.futures, hashlib, json, os, sys, threading, time, urllib.request
import urllib.error

version, out, with_assets, manifest, arch = (
    sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4], sys.argv[5])

def get(url):
    for attempt in range(5):
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                return r.read()
        except (OSError, urllib.error.URLError) as error:
            # Transient network and server conditions retry; anything else —
            # a missing artifact, say — fails with the URL named.
            if isinstance(error, urllib.error.HTTPError) and error.code not in (408, 429, 500, 502, 503, 504):
                raise RuntimeError("download failed: %s: %s" % (url, error)) from error
            if attempt == 4:
                raise RuntimeError("download failed: %s: %s" % (url, error)) from error
            time.sleep(2 ** attempt)

def fetch(path, url, sha1):
    dest = os.path.join(out, path)
    if os.path.exists(dest) and hashlib.sha1(open(dest, "rb").read()).hexdigest() == sha1:
        return
    os.makedirs(os.path.dirname(dest) or out, exist_ok=True)
    data = get(url)
    got = hashlib.sha1(data).hexdigest()
    if got != sha1:
        sys.exit("[minecraft] sha1 mismatch for %s: %s != %s" % (path, got, sha1))
    open(dest, "wb").write(data)

os.makedirs(out, exist_ok=True)
print("[minecraft] version %s -> %s (arch %s)" % (version, out, arch))

man = json.loads(get(manifest))
vurl = next((v["url"] for v in man["versions"] if v["id"] == version), None)
if not vurl:
    sys.exit("[minecraft] version not found: " + version)
vd = json.loads(get(vurl))
json.dump(vd, open(os.path.join(out, "version.json"), "w"))
print("[minecraft] java required: %s" % vd.get("javaVersion", {}).get("majorVersion"))

c = vd["downloads"]["client"]
fetch("client.jar", c["url"], c["sha1"])
print("[minecraft] client.jar %d bytes" % c["size"])

# The native jar each arch loads from <outdir>/natives, and the ELF e_machine
# the extracted .so files must carry (x86-64=62, AArch64=183, RISC-V=243).
NATIVE_JAR = {
    "x86_64": "-natives-linux.jar",
    "aarch64": "-natives-linux-arm64.jar",
    "riscv64": "-natives-linux-riscv64.jar",
}
E_MACHINE = {"x86_64": 62, "aarch64": 183, "riscv64": 243}
MAVEN_LWJGL = {"aarch64": "3.3.3", "riscv64": "3.3.6"}

lwjgl_modules = set()
n = 0
for lib in vd.get("libraries", []):
    dl = lib.get("downloads", {})
    art = dl.get("artifact")
    if art and art.get("path"):
        p = art["path"]
        if p.startswith("org/lwjgl/"):
            lwjgl_modules.add(p.split("/")[2])
            if arch in MAVEN_LWJGL:
                # aarch64/riscv64: the whole LWJGL set comes from Maven below,
                # so the classpath carries one consistent version.
                continue
        fetch("libraries/" + p, art["url"], art["sha1"]); n += 1
    for cls, cc in (dl.get("classifiers") or {}).items():
        if "linux" in cls and ("x86_64" in cls or "amd64" in cls):
            fetch("libraries/" + cc["path"], cc["url"], cc["sha1"]); n += 1
print("[minecraft] libraries: %d files" % n)

if arch in MAVEN_LWJGL:
    ver = MAVEN_LWJGL[arch]
    print("[minecraft] LWJGL %s from Maven Central (%s natives)" % (ver, arch))
    for mod in sorted(lwjgl_modules):
        # Both suffixes carry ".jar": the plain module jar, and NATIVE_JAR's
        # entry, which also serves as the extraction-time match below.
        for suffix in (".jar", NATIVE_JAR[arch]):
            name = "%s-%s%s" % (mod, ver, suffix)
            rel = "org/lwjgl/%s/%s/%s" % (mod, ver, name)
            url = "https://repo1.maven.org/maven2/" + rel
            sha1 = get(url + ".sha1").decode().strip().split()[0]
            fetch("libraries/" + rel, url, sha1); n += 1
    print("[minecraft] maven lwjgl: %d files" % (len(lwjgl_modules) * 2))

# classpath.txt is what the guest launcher expands; the Maven LWJGL jars are
# picked up by the same walk, so riscv64's override needs no special case.
jars = []
for root, _, files in os.walk(os.path.join(out, "libraries")):
    jars += [os.path.join(root, f) for f in files if f.endswith(".jar")]
open(os.path.join(out, "classpath.txt"), "w").write(":".join(sorted(jars)))
print("[minecraft] classpath: %d jars" % len(jars))

# Extract Linux natives here; the guest image ships no unzip.  The suffix
# match keeps arches apart (…-natives-linux.jar vs …-natives-linux-arm64.jar),
# and the e_machine check proves the right binaries landed.
import zipfile
natdir = os.path.join(out, "natives")
os.makedirs(natdir, exist_ok=True)
nat = 0
for j in jars:
    if not j.endswith(NATIVE_JAR[arch]):
        continue
    with zipfile.ZipFile(j) as z:
        for name in z.namelist():
            if not name.endswith(".so"):
                continue
            data = z.read(name)
            if data[:4] != b"\x7fELF" or int.from_bytes(data[18:20], "little") != E_MACHINE[arch]:
                sys.exit("[minecraft] %s in %s is not an %s ELF" % (name, os.path.basename(j), arch))
            open(os.path.join(natdir, os.path.basename(name)), "wb").write(data)
            nat += 1
print("[minecraft] natives: %d %s .so extracted" % (nat, arch))

if with_assets:
    ai = vd.get("assetIndex")
    if ai:
        idx = json.loads(get(ai["url"]))
        os.makedirs(os.path.join(out, "assets", "indexes"), exist_ok=True)
        json.dump(idx, open(os.path.join(out, "assets", "indexes", ai["id"] + ".json"), "w"))
        items = [("assets/objects/" + o["hash"][:2] + "/" + o["hash"],
                  "https://resources.download.minecraft.net/" + o["hash"][:2] + "/" + o["hash"],
                  o["hash"]) for o in idx["objects"].values()]
        lock, progress = threading.Lock(), [0]
        def one(it):
            fetch(*it)
            with lock:
                progress[0] += 1
                if progress[0] % 500 == 0:
                    print("[minecraft]   assets %d/%d" % (progress[0], len(items)))
        with concurrent.futures.ThreadPoolExecutor(max_workers=24) as ex:
            list(ex.map(one, items))
        print("[minecraft] assets: %d objects" % len(items))

print("[minecraft] done")
PY
