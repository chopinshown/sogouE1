"""Install a checksummed E1 hello bundle over ADB and verify UI readiness."""
import argparse
import hashlib
import json
import re
import shlex
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SERIAL = "15dacc90f0c2e787"
DEVICE_ROOT = "/userdata/e1-hello"

def adb(*args, timeout=20):
    result = subprocess.run(["adb", "-s", SERIAL, *args], capture_output=True, timeout=timeout)
    output = (result.stdout + result.stderr).decode(errors="replace").replace("\r", "")
    if result.returncode:
        raise RuntimeError(output.strip())
    return output

def shell(script):
    # This legacy adbd uses a PTY; explicit markers also check remote exit status.
    wrapped = "(\nset -e\n" + script + "\n)\nr=$?; echo E1_REMOTE_EXIT=$r; exit $r"
    output = adb("shell", wrapped)
    if "E1_REMOTE_EXIT=0" not in output:
        raise RuntimeError(output)
    return output.replace("E1_REMOTE_EXIT=0", "").strip()

RESTART = r'''
ROOT=/userdata/e1-hello
PID=$(cat "$ROOT/launcher.pid" 2>/dev/null || true)
if [ -n "$PID" ] && [ -r "/proc/$PID/cmdline" ] &&
   tr '\000' '\n' < "/proc/$PID/cmdline" | grep -Fxq /userdata/system/SogouE1; then
    kill -TERM "$PID"
else
    for PID in $(pidof SogouE1 2>/dev/null || true); do
        if [ "$(readlink /proc/$PID/exe)" = /usr/bin/SogouE1 ]; then
            kill -TERM "$PID"
        fi
    done
fi
'''

def rollback():
    # Factory selection is persistent and works on the next init respawn too.
    return shell("touch /userdata/e1-hello/disabled\n" + RESTART)

def activate(version):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Use a numeric major.minor.patch version")
    print(shell(f'''
ROOT=/userdata/e1-hello
test -x "$ROOT/releases/{version}/run.sh"
(cd "$ROOT/releases/{version}" && sha256sum -c checksums.sha256)
echo '{version}' > "$ROOT/current.version.new"
mv "$ROOT/current.version.new" "$ROOT/current.version"
rm -f "$ROOT/disabled"
: > "$ROOT/runtime.log"
sync
'''))
    print(shell(RESTART))
    return wait_ready(version)

def install(version):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Use a numeric major.minor.patch version")
    manifest = json.loads((ROOT / "dist" / f"e1-hello-{version}.json").read_text())
    archive = ROOT / "dist" / manifest["archive"]
    if hashlib.sha256(archive.read_bytes()).hexdigest() != manifest["sha256"]:
        raise ValueError("Local archive failed SHA-256 verification")
    print(shell(r'''
if [ -e /userdata/system/SogouE1 ] &&
   ! grep -Fq '# Writable application override.' /userdata/system/SogouE1; then
    echo 'An unrelated application override exists; refusing to replace it.'
    exit 1
fi
mkdir -p /userdata/e1-hello/incoming /userdata/e1-hello/releases /userdata/e1-hello/screenshots
test -x /usr/bin/qmlscene
'''))
    remote = f"{DEVICE_ROOT}/incoming/{archive.name}"
    print(adb("push", str(archive), remote + ".part").strip())
    print(adb("push", str(ROOT / "launcher.sh"), DEVICE_ROOT + "/incoming/launcher.sh").strip())
    launcher_hash = hashlib.sha256((ROOT / "launcher.sh").read_bytes()).hexdigest()
    q = shlex.quote
    print(shell(f'''
ROOT={q(DEVICE_ROOT)}
VERSION={q(version)}
ARCHIVE={q(remote)}
echo '{manifest['sha256']}  {remote}.part' | sha256sum -c -
echo '{launcher_hash}  {DEVICE_ROOT}/incoming/launcher.sh' | sha256sum -c -
mv "$ARCHIVE.part" "$ARCHIVE"
STAGE="$ROOT/releases/.stage-$VERSION-$$"
mkdir "$STAGE"
tar -xf "$ARCHIVE" -C "$STAGE"
(cd "$STAGE" && sha256sum -c checksums.sha256)
chmod 755 "$STAGE/run.sh"
if [ -e "$ROOT/releases/$VERSION" ]; then
    echo 'Release already exists; use a new version.'
    exit 1
fi
mv "$STAGE" "$ROOT/releases/$VERSION"
if [ -f "$ROOT/current.version" ]; then cp "$ROOT/current.version" "$ROOT/previous.version"; fi
echo "$VERSION" > "$ROOT/current.version.new"
mv "$ROOT/current.version.new" "$ROOT/current.version"
cp "$ROOT/incoming/launcher.sh" /userdata/system/SogouE1.new
chmod 755 /userdata/system/SogouE1.new
mv /userdata/system/SogouE1.new /userdata/system/SogouE1
rm -f "$ROOT/disabled"
: > "$ROOT/runtime.log"
sync
'''))
    print(shell(RESTART))
    result = wait_ready(version)
    result["archive_sha256"] = manifest["sha256"]
    return result

def wait_ready(version):
    for _ in range(25):
        time.sleep(1)
        log = shell("cat /userdata/e1-hello/runtime.log")
        if f"E1_HELLO_READY {version}" in log:
            print(log)
            # Give grabToImage's asynchronous callback a chance to finish.
            time.sleep(1)
            log = shell("cat /userdata/e1-hello/runtime.log")
            print(log)
            screenshots = ROOT / "screenshots"
            screenshots.mkdir(exist_ok=True)
            try:
                print(adb("pull", f"{DEVICE_ROOT}/screenshots/{version}.png", str(screenshots / f"{version}.png")).strip())
            except RuntimeError as error:
                print("Screenshot unavailable:", error)
            return {"version": version, "status": "ready", "transport": "ADB", "runtime_log": log}
        if "E1_HELLO_EXIT" in log:
            break
    print(shell("cat /userdata/e1-hello/runtime.log"))
    print(rollback())
    raise RuntimeError("Hello UI failed readiness; restored factory selection")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("version", nargs="?")
    parser.add_argument("--factory", action="store_true")
    parser.add_argument("--activate", action="store_true", help="Activate an already installed release")
    args = parser.parse_args()
    if args.factory:
        print(rollback())
        print("Factory recorder selected")
    elif args.version:
        result = activate(args.version) if args.activate else install(args.version)
        (ROOT / "dist" / f"result-{args.version}.json").write_text(json.dumps(result, indent=2) + "\n")
    else:
        parser.error("Specify a version or --factory")
