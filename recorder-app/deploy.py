import argparse
import hashlib
import json
import subprocess
import time
import urllib.request
from pathlib import Path

ROOT=Path(__file__).resolve().parent
SERIAL="15dacc90f0c2e787"
REMOTE="/userdata/e1-recorder"
def adb(*args):
    p=subprocess.run(["adb","-s",SERIAL,*args],capture_output=True,timeout=30)
    out=(p.stdout+p.stderr).decode(errors="replace").replace("\r","")
    if p.returncode:raise RuntimeError(out)
    return out
def shell(command):
    out=adb("shell","(\nset -e\n"+command+"\n)\nr=$?; echo E1_DEPLOY_EXIT=$r; exit $r")
    if "E1_DEPLOY_EXIT=0" not in out:raise RuntimeError(out)
    return out.replace("E1_DEPLOY_EXIT=0","").strip()
RESTART=r'''
FOUND=0
for ROOT in /userdata/e1-recorder /userdata/e1-hello; do
    PID=$(cat "$ROOT/launcher.pid" 2>/dev/null || true)
    if [ -n "$PID" ] && [ -r "/proc/$PID/cmdline" ] &&
       tr '\000' '\n' < "/proc/$PID/cmdline" | grep -Fxq /userdata/system/SogouE1; then
        kill -TERM "$PID"; FOUND=1; break
    fi
done
if [ "$FOUND" = 0 ]; then
    for PID in $(pidof SogouE1 2>/dev/null || true); do
        if [ "$(readlink /proc/$PID/exe)" = /usr/bin/SogouE1 ]; then kill -TERM "$PID"; fi
    done
fi
'''
def api(path):
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    return json.load(opener.open("http://127.0.0.1:18770/api/"+path,timeout=5))
def factory():
    print(shell("touch /userdata/e1-recorder/disabled\n"+RESTART))
    print("Factory recorder selected")
def deploy():
    manifest=json.loads((ROOT/"dist/latest.json").read_text())
    archive=ROOT/"dist"/manifest["archive"]
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==manifest["sha256"]
    print(shell(r'''
if [ -e /userdata/system/SogouE1 ] &&
   ! grep -Eq 'E1_RECORDER_MANAGED_LAUNCHER|# Writable application override\.' /userdata/system/SogouE1; then
    echo 'Unrelated application override exists'; exit 1
fi
mkdir -p /userdata/e1-recorder/incoming /userdata/e1-recorder/releases
if [ ! -e /userdata/e1-recorder/previous-launcher.sh ] && [ -e /userdata/system/SogouE1 ]; then
    cp /userdata/system/SogouE1 /userdata/e1-recorder/previous-launcher.sh
fi
'''))
    print(adb("push",str(archive),REMOTE+"/incoming/app.tar"))
    print(adb("push",str(ROOT/"launcher.sh"),REMOTE+"/incoming/launcher.sh"))
    lh=hashlib.sha256((ROOT/"launcher.sh").read_bytes()).hexdigest()
    release=manifest["release"]
    print(shell(f'''
ROOT=/userdata/e1-recorder
echo '{manifest['sha256']}  /userdata/e1-recorder/incoming/app.tar' | sha256sum -c -
echo '{lh}  /userdata/e1-recorder/incoming/launcher.sh' | sha256sum -c -
STAGE="$ROOT/releases/.stage-$$"
mkdir "$STAGE"
tar -xf "$ROOT/incoming/app.tar" -C "$STAGE"
(cd "$STAGE" && sha256sum -c checksums.sha256)
chmod 755 "$STAGE/run.sh" "$STAGE/recorderd"
if [ -d "$ROOT/releases/{release}" ]; then
    (cd "$ROOT/releases/{release}" && sha256sum -c "$STAGE/checksums.sha256")
else
    mv "$STAGE" "$ROOT/releases/{release}"
fi
if [ -f "$ROOT/current.version" ]; then cp "$ROOT/current.version" "$ROOT/previous.version"; fi
echo '{release}' > "$ROOT/current.version.new"
mv "$ROOT/current.version.new" "$ROOT/current.version"
cp "$ROOT/incoming/launcher.sh" /userdata/system/SogouE1.new
chmod 755 /userdata/system/SogouE1.new
mv /userdata/system/SogouE1.new /userdata/system/SogouE1
rm -f "$ROOT/disabled"
: > "$ROOT/ui.log"
: > "$ROOT/audio.log"
sync
'''))
    print(shell(RESTART))
    print(adb("forward","tcp:18770","tcp:18770"))
    for _ in range(30):
        time.sleep(1)
        try:
            state=api("status")
            log=shell("cat /userdata/e1-recorder/ui.log")
            if state["captureReady"] and "E1_RECORDER_UI_READY" in log:
                print(json.dumps(state));print(log)
                time.sleep(2)
                try:print(adb("pull",REMOTE+"/screenshot.png",str(ROOT/"evidence/screenshot.png")))
                except Exception as e:print("Screenshot:",e)
                (ROOT/"dist/deployed.json").write_text(json.dumps({"release":release,"state":state,"ui_log":log},indent=2)+"\n")
                return
        except Exception:pass
    print(shell("cat /userdata/e1-recorder/ui.log /userdata/e1-recorder/audio.log"))
    factory()
    raise RuntimeError("Recorder app did not become ready; selected factory app")
if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("--factory",action="store_true");args=p.parse_args()
    factory() if args.factory else deploy()
