"""Create deterministic ADB update bundles for the E1 QML hello application."""
import argparse
import hashlib
import io
import json
import re
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent

def build(version):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Use a numeric major.minor.patch version")
    updated = version != "1.0.0"
    build_js = (
        '.pragma library\n'
        f'var version = "{version}"\n'
        f'var accent = "{"#7ce8b1" if updated else "#78d7ff"}"\n'
        f'var status = "{"ADB UPDATE VERIFIED" if updated else "INSTALLED VIA ADB"}"\n'
    ).encode()
    files = {"Main.qml": (ROOT / "Main.qml").read_bytes(),
             "run.sh": (ROOT / "run.sh").read_bytes(), "Build.js": build_js}
    checksums = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    files["checksums.sha256"] = "".join(f"{h}  {name}\n" for name, h in sorted(checksums.items())).encode()
    out = ROOT / "dist"
    out.mkdir(exist_ok=True)
    # The recorder's minimal BusyBox tar has no gzip support.
    archive = out / f"e1-hello-{version}.tar"
    with tarfile.open(archive, "w") as tar:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o755 if name == "run.sh" else 0o644
            info.mtime = 0
            tar.addfile(info, io.BytesIO(data))
    manifest = {"app": "e1-hello", "version": version, "board": "Sogou E1 RK3308",
                "runtime": "Qt 5.9.4 qmlscene", "transport": "ADB",
                "archive": archive.name, "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "files": checksums, "signature": None,
                "note": "Local ADB demonstration: hashes check integrity; this bundle is not cryptographically signed."}
    (out / f"e1-hello-{version}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return archive

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    print(build(parser.parse_args().version))
