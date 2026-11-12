# Sogou E1 Recorder

A custom recorder for the Sogou E1, with physical-button controls, a native ALSA audio backend and a Qt Quick display. Applications are built on the Mac and installed through ADB using the firmware's writable application override.

## Recorder features

- Record **44,100 Hz, 24-bit, stereo PCM WAV** files.
- Live left/right level meters with peak hold.
- Physical Record, Mark and Stop controls; no on-screen buttons.
- Marks stored as embedded WAV cue points and in a JSON sidecar.
- Recording browser, stereo waveform preview, playback and seeking.
- Hardware file selection and navigation between marks.

Recordings are saved in the large recording partition:

```text
/mnt/sdcard/Recordings/
  REC-YYYYMMDD-HHMMSS-000.wav
  REC-YYYYMMDD-HHMMSS-000.wav.marks.json
  REC-YYYYMMDD-HHMMSS-000.wav.wave.json
```

Capture uses the digital PDM microphone pair on `hw:2,0`, channels 0 and 1. Its signed 24-bit samples arrive left-aligned in 32-bit words and are packed into three-byte WAV samples. Capture runs at 44.1 kHz without software resampling. Playback uses the device's default ALSA output path.

## Tested device

| Component | Configuration |
| --- | --- |
| Processor | Rockchip RK3308, ARM64 |
| Factory firmware | ZhiyinOS 1.2.3 |
| System | Buildroot 2018.02-rc3, Linux 4.4.143 |
| Runtime | glibc 2.26, ALSA 1.1.5, Qt 5.9.4 |
| Display | 320 × 320 touchscreen |
| Build host | Apple Silicon Mac |

Recording format, WAV cue points, waveform generation, playback, seeking and Stop were checked on the device. Physical microphone-meter response and audible playback were also confirmed.

## Set up the build environment

Install Python 3, Make and Android Platform Tools. With Homebrew:

```sh
brew install python android-platform-tools
git clone git@github.com:chopinshown/sogouE1.git
cd sogouE1
```

The build script currently expects the **Apple Silicon macOS build of Zig 0.14.1** at the path below. Download it from the official Zig site and verify its checksum:

```sh
mkdir -p .tools
curl -fL https://ziglang.org/download/0.14.1/zig-aarch64-macos-0.14.1.tar.xz \
  -o .tools/zig-aarch64-macos-0.14.1.tar.xz
echo '39f3dc5e79c22088ce878edc821dedb4ca5a1cd9f5ef915e9b3cc3053e8faefa  .tools/zig-aarch64-macos-0.14.1.tar.xz' \
  | shasum -a 256 -c -
tar -xJf .tools/zig-aarch64-macos-0.14.1.tar.xz -C .tools
```

For another host platform, adjust the Zig executable path in `recorder-app/build.sh`. The target remains `aarch64-linux-gnu.2.26`.

## Connect the recorder

On the E1, open **通用设置 → 关于本机** (General Settings → About), then repeatedly tap **MAC地址** (MAC address) to enable ADB. Connect a USB data cable and check:

```sh
adb devices -l
```

The tested device provides a root ADB shell. Deployment and backup scripts currently contain the serial `15dacc90f0c2e787`; update their `SERIAL` constants when using a different recorder.

Copy the matching ALSA runtime library from the device for linking:

```sh
mkdir -p recorder-app/sysroot/usr/lib recorder-app/evidence
adb -s 15dacc90f0c2e787 pull /usr/lib/libasound.so.2.0.0 \
  recorder-app/sysroot/usr/lib/libasound.so.2
```

The library is a local build dependency and is excluded from Git. ALSA 1.1.5 headers and their upstream license are included under `recorder-app/vendor/`.

## Build and deploy

Run these commands from the repository root:

```sh
make -C recorder-app build      # Build the ARM64 audio backend
make -C recorder-app package    # Build and create an update bundle
make -C recorder-app deploy     # Build, package, install and check readiness
```

Stop any active recording before deploying, because deployment restarts the UI.

The installer verifies the bundle and individual file SHA-256 hashes, stages a versioned release, switches the active version pointer, and restarts the application. It checks that capture is ready and the QML UI has loaded. A failed readiness check selects the factory app.

| Device path | Purpose |
| --- | --- |
| `/userdata/system/SogouE1` | Managed launcher override |
| `/userdata/e1-recorder/releases/` | Installed recorder releases |
| `/userdata/e1-recorder/current.version` | Active release identifier |
| `/userdata/e1-recorder/audio.log` | Backend log |
| `/userdata/e1-recorder/ui.log` | UI log |
| `/usr/bin/SogouE1` | Factory application |

These are **unsigned ADB application updates**. SHA-256 checks detect corruption; they do not authenticate a publisher. Wireless OTA, signed-package verification and automatic rollback to a previous custom release are not implemented.

## Use the app

| Physical button | Action |
| --- | --- |
| **REC** | Start a new recording while idle, from any page |
| **M**, while recording | Add one sample-indexed mark per press |
| **STOP**, while recording or playing | Save the recording or stop playback |
| **M**, on the ready screen | Open the recording list |
| **+ / −**, in the recording list | Move the highlighted file selection up/down |
| **M**, in the recording list | Open the selected file's waveform preview |
| **M**, in the preview | Start or stop playback |
| **+ / −**, in the preview | Seek to the next/previous mark; otherwise step five seconds |
| **STOP**, while idle | Go back from preview to files, or from files to the ready screen |

The screen displays meters, waveforms, the selected file and button hints. Held-key auto-repeat is ignored so a held M button adds only one mark.

To return to the factory application, hold **M** for 1.2 seconds on the idle ready screen, then press **M** to confirm or **STOP** to cancel. You can also run:

```sh
make -C recorder-app factory
```

Deploy again to reactivate the custom recorder.

Standard RIFF WAV files have a size limit of approximately 4 GiB, or 4.5 hours at this format. The app stops and saves before reaching that limit. Recording data is about 0.95 GB per hour.

## Diagnostics

The backend's control API listens on the recorder's loopback address, port `18770`. Deployment creates an ADB forward to the Mac. To inspect status manually:

```sh
adb -s 15dacc90f0c2e787 forward tcp:18770 tcp:18770
curl --noproxy '*' http://127.0.0.1:18770/api/status
adb -s 15dacc90f0c2e787 shell 'tail -50 /userdata/e1-recorder/audio.log'
adb -s 15dacc90f0c2e787 shell 'tail -50 /userdata/e1-recorder/ui.log'
```

The backend serves `/api/status`, `/api/files` and `/api/wave` through GET requests. Recording, marking, stopping, playback and seeking use POST requests in `recorder-app/recorderd.c`.

Saved `grabToImage` screenshots appear vertically mirrored on the tested Qt software renderer; the physical screen is upright.

## Firmware backup

The backup utility copies firmware regions over ADB, verifies source hashes before and after transfer, compresses them with Zstandard, and verifies decompression:

```sh
brew install zstd
python3 firmware-analysis/backup_firmware.py \
  "factory-backup/firmware-$(date +%Y%m%d-%H%M%S)"
```

It includes both boot/system slots, U-Boot, trust, misc, OEM, the eMMC boot areas, boot prefix and GPT metadata. The OEM filesystem is briefly frozen during its copy. It excludes the userdata partition, recording partition, RPMB and hardware security secrets. Images are per-region raw backups, not a vendor OTA `update.img` package.

## Repository layout

| Path | Contents |
| --- | --- |
| `recorder-app/` | Recorder backend, QML interface, packaging and ADB deployment |
| `hello-app/` | Initial display app and versioned ADB update demonstration |
| `app-probe/` | Minimal unsigned ARM64 execution probe |
| `firmware-analysis/` | Firmware backup and Qt resource extraction tools |
| `inspect_hardware_readonly.sh` | Read-only hardware inspection script for an ADB shell |
| `recorder_access.c` | MTP storage-access experiment for the normal USB mode |

Firmware dumps, recordings, extracted proprietary assets, toolchains, runtime libraries and generated bundles are excluded from Git. `app-probe/build_probe.py` and `firmware-analysis/extract_qt_resources.py` additionally require the Python package `pyelftools`.
