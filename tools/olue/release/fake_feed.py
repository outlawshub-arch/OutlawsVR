"""REQ-AUTOUPDATE's test data: fake releases as folders and a fake install, for -OLUpdateProbe -OLUpdateProbeDir=<folder>.

  python fake_feed.py <folder>

Nothing here is a real build. The zip is deflated as the release's is, 30 MB, so the download takes several chunks."""
import json
import os
import random
import shutil
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_latest_json import latest_for, file_sha256

FILES = {
    "OutlawsUE.exe": b"new exe",
    "Play Outlaws VR.bat": b"@echo relaunched>\"%~dp0relaunched.txt\"\r\n",
    "Engine/Binaries/thing.txt": b"new engine",
    "OutlawsUE/Content/Paks/new.pak": b"new pak",
    "OutlawsUE/Binaries/Win64/game.txt": b"new game",
    "Tools/MenuOverlay.kpf": b"new theme",
    "Tools/NativeMaps/mapa/mapa.rcm": b"map a new",
    "Tools/NativeMaps/mapa/mapa.lab": b"lab a new",
    "Tools/NativeMaps/mapb/mapb.rcm": b"map b new",
    "Tools/NativeMaps/mapb/mapb.lab": b"lab b new",
    "Tools/OutlawsVR.apk": b"fake apk " * 1000,
}
OLD = {
    "OutlawsUE.exe": "old exe",
    "Play Outlaws VR.bat": "@echo relaunched>\"%~dp0relaunched.txt\"\r\n",
    "my_note.txt": "the player's note",
    "Engine/Binaries/thing.txt": "old engine",
    "OutlawsUE/Content/Paks/stale_old.pak": "old pak",
    "OutlawsUE/Saved/OutlawsSaves/keep.sav": "the player's save",
    "Tools/MenuOverlay.kpf": "old theme",
    "Tools/NativeMaps/mapa/mapa.rcm": "map a old",
}


def write_zip(path, leave_out=()):
    rnd = random.Random(7)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in FILES.items():
            if name not in leave_out:
                z.writestr("OutlawsVR/" + name, data)
        z.writestr("OutlawsVR/OutlawsUE/Content/Paks/big.bin", rnd.randbytes(20 * 1024 * 1024))
        z.writestr("OutlawsVR/OutlawsUE/Content/Paks/big2.bin", bytes(40 * 1024 * 1024))


def feed(root, name, build, zip_from=None, change=None, leave_out=()):
    d = os.path.join(root, name)
    os.makedirs(d)
    z = os.path.join(d, "OutlawsVR-fake.zip")
    if zip_from:
        shutil.copy(zip_from, z)
    else:
        write_zip(z, leave_out)
    latest = latest_for(z, "v9.9", "9.9", "fake123", build, require=False)
    if change:
        latest.update(change)
    with open(os.path.join(d, "latest.json"), "w", encoding="utf-8") as f:
        json.dump(latest, f, indent=1)
    with open(os.path.join(d, "zip.sha256"), "w") as f:
        f.write(file_sha256(z))
    return z


def main():
    root = sys.argv[1]
    if os.path.exists(root):
        sys.exit("the folder is there already: " + root)
    os.makedirs(root)
    z = feed(root, "feed_new", 50)
    feed(root, "feed_newer", 51, z)
    feed(root, "feed_same", 50, z)
    feed(root, "feed_badhash", 50, z, {"zip_sha256": "0" * 64})
    feed(root, "feed_badapk", 50, z, {"apk_sha256": "1" * 64})
    feed(root, "feed_notheme", 50, leave_out=("Tools/MenuOverlay.kpf",))
    os.makedirs(os.path.join(root, "feed_none"))
    os.makedirs(os.path.join(root, "feed_garbage"))
    with open(os.path.join(root, "feed_garbage", "latest.json"), "w") as f:
        f.write("<html>not json</html>")
    for name, text in OLD.items():
        p = os.path.join(root, "install", name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", newline="") as f:
            f.write(text)
    with zipfile.ZipFile(z) as zf:
        zf.extractall(os.path.join(root, "whole_unzip"))
    os.rename(os.path.join(root, "whole_unzip", "OutlawsVR"), os.path.join(root, "whole"))
    os.rmdir(os.path.join(root, "whole_unzip"))
    print("fake feed in", root)


if __name__ == "__main__":
    main()
