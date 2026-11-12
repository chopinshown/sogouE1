"""Back up E1 firmware partitions over ADB, excluding user and recording data."""
import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import time
from pathlib import Path

SERIAL = "15dacc90f0c2e787"
MIB = 1024 * 1024

def adb(*args, timeout=90):
    p = subprocess.run(["adb", "-s", SERIAL, *args], capture_output=True, timeout=timeout)
    text = (p.stdout + p.stderr).decode(errors="replace").replace("\r", "")
    if p.returncode:
        raise RuntimeError(text.strip())
    return text

def shell(command, timeout=90):
    result = adb("shell", "(\nset -e\n" + command + "\n)\nr=$?; echo E1_BACKUP_EXIT=$r; exit $r", timeout=timeout)
    if "E1_BACKUP_EXIT=0" not in result:
        raise RuntimeError(result)
    return result.replace("E1_BACKUP_EXIT=0", "").strip()

def sha256_file(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(8 * MIB), b""):
            h.update(block)
    return h.hexdigest()

def remote_hash(path):
    text = shell("sha256sum " + path)
    value = text.split()[0]
    if not re.fullmatch("[0-9a-f]{64}", value):
        raise RuntimeError(text)
    return value

def save_json(path, obj):
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_text(json.dumps(obj, indent=2) + "\n")
    temporary.replace(path)

def main(output):
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / "backup-manifest.json"
    if manifest_path.exists():
        raise RuntimeError("Use a fresh backup directory")
    disk_sectors = int(shell("cat /sys/class/block/mmcblk0/size"))
    geometry = []
    for i, name in enumerate(["uboot", "trust", "misc", "boot_a", "boot_b", "system_a", "system_b", "oem"], 1):
        fields = shell(f"cat /sys/class/block/mmcblk0p{i}/start /sys/class/block/mmcblk0p{i}/size").split()
        geometry.append({"name": name, "source": f"/dev/mmcblk0p{i}",
                         "start_sector": int(fields[0]), "bytes": int(fields[1]) * 512})
    manifest = {
        "device_serial": SERIAL,
        "created_at": datetime.datetime.now().astimezone().isoformat(),
        "scope": "Firmware partitions, eMMC boot areas, boot prefix and GPT metadata; no user-data partitions",
        "excluded": ["/dev/mmcblk0p9 userdata", "/dev/mmcblk0p10 sdcard and recordings", "RPMB authenticated storage and hardware secrets"],
        "format": "Per-region raw images compressed with zstd; not a vendor OTA update.img",
        "device_user_area_sectors": disk_sectors,
        "sector_bytes": 512,
        "status": "in_progress",
        "images": [],
    }
    save_json(manifest_path, manifest)
    (output / "product-version.json").write_text(shell("cat /etc/sogou/system_version.info") + "\n")
    (output / "mounts-before.txt").write_text(shell("cat /proc/mounts") + "\n")
    prefix_sectors = geometry[0]["start_sector"]
    shell(f"dd if=/dev/mmcblk0 of=/tmp/e1-backup-prefix.img bs=512 count={prefix_sectors} 2>/dev/null\n"
          f"dd if=/dev/mmcblk0 of=/tmp/e1-backup-gpt-tail.img bs=512 skip={disk_sectors - 33} count=33 2>/dev/null")
    regions = [
        {"name": "emmc-boot0", "source": "/dev/mmcblk0boot0", "bytes": 4 * MIB},
        {"name": "emmc-boot1", "source": "/dev/mmcblk0boot1", "bytes": 4 * MIB},
        {"name": "boot-prefix", "source": "/tmp/e1-backup-prefix.img", "disk_source": "/dev/mmcblk0", "start_sector": 0, "bytes": prefix_sectors * 512},
        *geometry,
        {"name": "gpt-backup", "source": "/tmp/e1-backup-gpt-tail.img", "disk_source": "/dev/mmcblk0", "start_sector": disk_sectors - 33, "bytes": 33 * 512},
    ]
    manifest["expected_uncompressed_bytes"] = sum(x["bytes"] for x in regions)
    save_json(manifest_path, manifest)
    print(f"Backing up {len(regions)} regions, {manifest['expected_uncompressed_bytes'] / MIB:.2f} MiB", flush=True)
    try:
        for region in regions:
            name, source, size = region["name"], region["source"], region["bytes"]
            start = time.monotonic()
            frozen = False
            try:
                if name == "oem":
                    # Thaw independently even if the host disconnects while this small partition is read.
                    shell("nohup sh -c 'sleep 60; fsfreeze --unfreeze /oem' > /tmp/e1-oem-thawguard.log 2>&1 < /dev/null &\n"
                          "echo $! > /tmp/e1-oem-thawguard.pid\nfsfreeze --freeze /oem")
                    frozen = True
                print(f"{name}: hashing source ({size / MIB:.2f} MiB)", flush=True)
                before = remote_hash(source)
                raw = output / f"{name}.img.partial"
                # A valid boot0 transfer may already be present from transport qualification.
                if not (name == "emmc-boot0" and raw.exists() and raw.stat().st_size == size):
                    print(f"{name}: copying", flush=True)
                    adb("pull", source, str(raw), timeout=300)
                if raw.stat().st_size != size:
                    raise RuntimeError(f"{name}: wrong image size")
                local_hash = sha256_file(raw)
                after = remote_hash(source)
                if local_hash != before or local_hash != after:
                    raise RuntimeError(f"{name}: source changed or transfer checksum mismatch")
            finally:
                if frozen:
                    shell("fsfreeze --unfreeze /oem\n"
                          "if [ -r /tmp/e1-oem-thawguard.pid ]; then kill $(cat /tmp/e1-oem-thawguard.pid) 2>/dev/null || true; fi")
            compressed = output / f"{name}.img.zst.partial"
            with compressed.open("wb") as stream:
                subprocess.run(["zstd", "-q", "-T0", "-3", "-c", str(raw)], stdout=stream, check=True)
            h = hashlib.sha256()
            decoded_bytes = 0
            decoder = subprocess.Popen(["zstd", "-q", "-d", "-c", str(compressed)], stdout=subprocess.PIPE)
            for block in iter(lambda: decoder.stdout.read(8 * MIB), b""):
                decoded_bytes += len(block)
                h.update(block)
            decoder.stdout.close()
            if decoder.wait() or h.hexdigest() != local_hash or decoded_bytes != size:
                raise RuntimeError(f"{name}: compressed image validation failed")
            final = compressed.with_suffix("")
            compressed.replace(final)
            image = {**region, "file": final.name, "raw_sha256": local_hash,
                     "source_sha256_before": before, "source_sha256_after": after,
                     "compressed_bytes": final.stat().st_size, "compressed_sha256": sha256_file(final),
                     "source_consistent": True, "decompression_verified": True,
                     "oem_frozen_during_copy": frozen, "elapsed_seconds": round(time.monotonic() - start, 2)}
            manifest["images"].append(image)
            save_json(manifest_path, manifest)
            raw.unlink()
            print(f"{name}: verified, compressed to {final.stat().st_size / MIB:.2f} MiB ({image['elapsed_seconds']} s)", flush=True)
        manifest["status"] = "verified"
        manifest["completed_at"] = datetime.datetime.now().astimezone().isoformat()
        manifest["total_compressed_bytes"] = sum(i["compressed_bytes"] for i in manifest["images"])
        save_json(manifest_path, manifest)
        (output / "SHA256SUMS").write_text("".join(i["compressed_sha256"] + "  " + i["file"] + "\n" for i in manifest["images"]))
        (output / "mounts-after.txt").write_text(shell("cat /proc/mounts") + "\n")
        print(f"BACKUP VERIFIED: {output}\nTotal compressed: {manifest['total_compressed_bytes'] / MIB:.2f} MiB", flush=True)
    except BaseException as error:
        manifest["status"] = "incomplete"
        manifest["error"] = str(error)
        save_json(manifest_path, manifest)
        raise
    finally:
        shell("rm -f /tmp/e1-backup-prefix.img /tmp/e1-backup-gpt-tail.img\n"
              "if [ -r /tmp/e1-oem-thawguard.pid ]; then fsfreeze --unfreeze /oem 2>/dev/null || true; fi")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    main(parser.parse_args().output.resolve())
