import hashlib
import io
import json
import tarfile
from pathlib import Path

ROOT=Path(__file__).resolve().parent
VERSION="1.0.0"
files={p.name:p.read_bytes() for p in ROOT.glob("*.qml")}
files["run.sh"]=(ROOT/"run.sh").read_bytes()
files["recorderd"]=(ROOT/"build/recorderd").read_bytes()
hashes={name:hashlib.sha256(data).hexdigest() for name,data in files.items()}
release=VERSION+"-"+hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest()[:8]
files["checksums.sha256"]="".join(h+"  "+name+"\n" for name,h in sorted(hashes.items())).encode()
out=ROOT/"dist";out.mkdir(exist_ok=True)
archive=out/("e1-recorder-"+release+".tar")
with tarfile.open(archive,"w") as tar:
    for name,data in sorted(files.items()):
        info=tarfile.TarInfo(name);info.size=len(data);info.mtime=0
        info.mode=0o755 if name in ["run.sh","recorderd"] else 0o644
        tar.addfile(info,io.BytesIO(data))
manifest={"app":"E1 Recorder","version":VERSION,"release":release,"archive":archive.name,
          "sha256":hashlib.sha256(archive.read_bytes()).hexdigest(),"files":hashes,
          "format":{"container":"WAV PCM","sampleRate":44100,"bits":24,"channels":2},
          "storage":"/mnt/sdcard/Recordings","transport":"ADB","signature":None}
(out/"latest.json").write_text(json.dumps(manifest,indent=2)+"\n")
print(archive)
