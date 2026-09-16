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
#   tools/fetch-minecraft.sh [version] [outdir]
#   tools/fetch-minecraft.sh                     # default 1.21.11
#   tools/fetch-minecraft.sh 26.3 build/mc/26.3  # needs Java 25
#
# Version choice matters for the JVM: the 1.21.x line needs Java 21 (what the
# xfce image ships), while the 26.x line already needs Java 25.
set -eu

VERSION=${1:-1.21.11}
OUT=${2:-build/minecraft/$VERSION}
WITH_ASSETS=${WITH_ASSETS:-1}
MANIFEST=${MANIFEST:-https://piston-meta.mojang.com/mc/game/version_manifest_v2.json}

python3 - "$VERSION" "$OUT" "$WITH_ASSETS" "$MANIFEST" <<'PY'
import hashlib, json, os, sys, urllib.request

version, out, with_assets, manifest = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4]

def get(url):
    with urllib.request.urlopen(url, timeout=180) as r:
        return r.read()

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
print("[minecraft] version %s -> %s" % (version, out))

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

n = 0
for lib in vd.get("libraries", []):
    dl = lib.get("downloads", {})
    art = dl.get("artifact")
    if art and art.get("path"):
        fetch("libraries/" + art["path"], art["url"], art["sha1"]); n += 1
    for cls, cc in (dl.get("classifiers") or {}).items():
        if "linux" in cls and ("x86_64" in cls or "amd64" in cls):
            fetch("libraries/" + cc["path"], cc["url"], cc["sha1"]); n += 1
print("[minecraft] libraries: %d files" % n)

jars = []
for root, _, files in os.walk(os.path.join(out, "libraries")):
    jars += [os.path.join(root, f) for f in files if f.endswith(".jar")]
open(os.path.join(out, "classpath.txt"), "w").write(":".join(sorted(jars)))
print("[minecraft] classpath: %d jars" % len(jars))

# Extract Linux natives here; the guest image ships no unzip.
import zipfile
natdir = os.path.join(out, "natives")
os.makedirs(natdir, exist_ok=True)
nat = 0
for j in jars:
    if "-natives-linux" not in j:
        continue
    with zipfile.ZipFile(j) as z:
        for name in z.namelist():
            if name.endswith(".so"):
                open(os.path.join(natdir, os.path.basename(name)), "wb").write(z.read(name))
                nat += 1
print("[minecraft] natives: %d .so extracted" % nat)

if with_assets:
    ai = vd.get("assetIndex")
    if ai:
        idx = json.loads(get(ai["url"]))
        os.makedirs(os.path.join(out, "assets", "indexes"), exist_ok=True)
        json.dump(idx, open(os.path.join(out, "assets", "indexes", ai["id"] + ".json"), "w"))
        objs = idx["objects"]
        for i, (name, o) in enumerate(objs.items()):
            h = o["hash"]
            fetch("assets/objects/" + h[:2] + "/" + h,
                  "https://resources.download.minecraft.net/" + h[:2] + "/" + h, h)
            if i and i % 1000 == 0:
                print("[minecraft]   assets %d/%d" % (i, len(objs)))
        print("[minecraft] assets: %d objects" % len(objs))

print("[minecraft] done")
PY
