"""REQ-AUTOUPDATE: writes latest.json beside a release zip. Run it on the zip the release uploads, after the zip is final, and
upload latest.json with it. The game reads this one file at startup; the zip carries both platforms.

  python make_latest_json.py <the zip> --tag v0.96 --version 0.96 --commit abc1234 [--build N] [--out DIR]

--build defaults to StoreVersion in Config/DefaultEngine.ini: the number the game compares, and the Android versionCode, so it
must rise from one release to the next. Stops, writing nothing, if the zip lacks what an update must carry: the exe, the APK,
Tools/MenuOverlay.kpf and Tools/NativeMaps (user 5 Nov: without them players lose the theme)."""
import argparse
import hashlib
import json
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def store_version():
    ini = os.path.join(HERE, "..", "..", "..", "Config", "DefaultEngine.ini")
    with open(ini, encoding="utf-8") as f:
        m = re.search(r"^StoreVersion=(\d+)", f.read(), re.M)
    return int(m.group(1)) if m else 0


def plain_name(name):
    """The game's PlainName (OutlawsUpdate.cpp): letters, digits, dot, dash, underscore, 128 at most, no "..". latest.json with a tag,
    version or zip outside it is thrown away by the game with no message, so the update would never be offered."""
    return bool(re.fullmatch(r"[A-Za-z0-9._-]{1,128}", name)) and ".." not in name


def latest_for(zip_path, tag, version, commit, build, require=True):
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        top = names[0].split("/")[0]
        apk = top + "/Tools/OutlawsVR.apk"
        lacking = []
        if top + "/OutlawsUE.exe" not in names:
            lacking.append("OutlawsUE.exe")
        if apk not in names:
            lacking.append("Tools/OutlawsVR.apk")
        if top + "/Tools/MenuOverlay.kpf" not in names:
            lacking.append("Tools/MenuOverlay.kpf")
        if not any(n.startswith(top + "/Tools/NativeMaps/") and not n.endswith("/") for n in names):
            lacking.append("Tools/NativeMaps")
        if lacking and require:
            sys.exit("the zip lacks: " + ", ".join(lacking))
        h = hashlib.sha256()
        if apk in names:
            with z.open(apk) as f:
                for block in iter(lambda: f.read(1024 * 1024), b""):
                    h.update(block)
    return {
        "schema": 1,
        "version": version,
        "tag": tag,
        "build": build,
        "commit": commit,
        "zip": os.path.basename(zip_path),
        "zip_size": os.path.getsize(zip_path),
        "zip_sha256": file_sha256(zip_path),
        "apk_sha256": h.hexdigest(),
    }


def main():
    ap = argparse.ArgumentParser(description="latest.json for a release zip")
    ap.add_argument("zip")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--commit", required=True)
    ap.add_argument("--build", type=int, default=0)
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    for label, value in (("tag", a.tag), ("version", a.version), ("zip name", os.path.basename(a.zip))):
        if not plain_name(value):
            sys.exit("the game would ignore this update: %s %r has a character outside letters, digits, dot, dash and underscore (0.96-hotfix1, not 0.96 hotfix 1)" % (label, value))
    if not os.path.basename(a.zip).endswith(".zip"):
        sys.exit("the game would ignore this update: the zip's name must end in .zip")
    build = a.build or store_version()
    if build <= 0:
        sys.exit("no build number")
    latest = latest_for(a.zip, a.tag, a.version, a.commit, build)
    out = os.path.join(a.out or os.path.dirname(os.path.abspath(a.zip)), "latest.json")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(latest, f, indent=1)
        f.write("\n")
    print("%s: build %d, %s, %d bytes, zip %s, apk %s" % (out, build, latest["zip"], latest["zip_size"], latest["zip_sha256"][:12], latest["apk_sha256"][:12]))


if __name__ == "__main__":
    main()
