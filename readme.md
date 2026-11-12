# Sogou E1 Recorder + Xiaozhi

A custom recorder for the Sogou E1, with physical-button controls, a native ALSA audio backend and a Qt Quick display. Applications are built on the Mac and installed through ADB using the firmware's writable application override.

## Xiaozhi voice assistant

The `feat/xiaozhi-e1` branch adds a Linux client for the [Xiaozhi voice assistant](https://github.com/78/xiaozhi-esp32), using the recorder app as its base. It opens on the voice page: **M** activates/binds the official service, **REC** starts talking, **STOP** sends the turn, and **M** cancels an active operation. **+ / −** adjust voice playback volume. The back arrow opens the recorder; **VOICE** returns to the assistant. Wi-Fi settings and the three-second POWER shutdown remain available.

See [the port design, setup, controls and limitations](docs/xiaozhi-e1.md). This first port uses manual WebSocket/Opus turns; wake words and full-duplex echo cancellation are not implemented. Build dependencies now include CMake; the build downloads checksum-pinned voice/TLS libraries and includes their license notices in the app bundle.

## Recorder features

- Record **44,100 Hz, 24-bit, stereo PCM WAV** files.
- Live left/right level meters with peak hold.
- Physical Record, Mark and Stop controls; a button-free recording screen.
- Marks stored as embedded WAV cue points and in a JSON sidecar.
- Recording browser, stereo waveform preview, playback and seeking.
- Hardware and touch file selection, waveform seeking and mark navigation.
- Wi-Fi scanning, password entry, saved networks and connection/IP status.

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
| `/userdata/e1-recorder/poweroff-last.json` | Last shutdown request and audio-finalization audit |
| `/usr/bin/SogouE1` | Factory application |

These are **unsigned ADB application updates**. SHA-256 checks detect corruption; they do not authenticate a publisher. Wireless OTA, signed-package verification and automatic rollback to a previous custom release are not implemented.

## Use the app

| Physical button | Action |
| --- | --- |
| **REC** | Start a new recording while idle, from any page |
| **M**, while recording | Add one sample-indexed mark per press |
| **STOP**, while recording or playing | Save the recording or stop playback |
| **M**, on the ready screen | Open the recording list |
| **+**, on the ready screen | Open Wi-Fi settings |
| **+ / −**, in Wi-Fi settings | Highlight a network |
| **M**, in Wi-Fi settings | Open the highlighted network; connect from its password page |
| **+ / −**, in the recording list | Move the highlighted file selection up/down |
| **M**, in the recording list | Open the selected file's waveform preview |
| **M**, in the preview | Start or stop playback |
| **+ / −**, in the preview | Seek to the next/previous mark; otherwise step five seconds |
| **STOP**, while idle | Go back from preview to files, or from files to the ready screen |
| **POWER**, held for 3 seconds | Save any active recording and shut down the device |

The screen displays meters, waveforms, the selected file and button hints. Held-key auto-repeat is ignored so a held M button adds only one mark.

Release POWER before three seconds to cancel shutdown. At the threshold, the app finalizes WAV audio and marks, closes the audio devices, prepares `/proc/simplae_panel`, syncs storage and invokes the normal BusyBox `poweroff` sequence. The factory application uses the same panel preparation and poweroff command, with a 1.5-second hold followed by a confirmation dialog; this app uses a direct three-second hold.

Touch works alongside these physical keys: tap **FILES** to open recordings, swipe the list, tap a file to preview it, and tap or drag the waveform to seek. Tap a mark to select its position, or swipe the mark row to reveal more marks. The back arrow returns to the previous page. Physical keys remain available after touch interactions. Recording and playback controls use the hardware keys, with no on-screen Record, Mark, Stop or Play buttons.

To return to the factory application, hold **M** for 1.2 seconds on the idle ready screen, then press **M** to confirm or **STOP** to cancel. You can also run:

```sh
make -C recorder-app factory
```

Deploy again to reactivate the custom recorder.

Standard RIFF WAV files have a size limit of approximately 4 GiB, or 4.5 hours at this format. The app stops and saves before reaching that limit. Recording data is about 0.95 GB per hour.

## Wi-Fi settings

While idle, tap **WI-FI** in the top bar or press **+**. Tap **SCAN**, choose a network, enter its password and tap **CONNECT**. The compact phone-style keyboard uses 12 large keys. Tap a letter key repeatedly to cycle its characters; pause for 0.9 seconds or tap another key to accept the character. Hold a letter key to enter its digit. Tap the bottom-left mode key to cycle **abc → ABC → 123 → symbols**. Space/zero and backspace occupy the other bottom keys; **SHOW/HIDE** sits beside the password. Physical **+** changes keyboard mode and **−** erases a character while editing. The status page distinguishes authentication from obtaining an IPv4 address and shows the address when assigned. “Connected” indicates a Wi-Fi link with an address, without testing Internet access.

Saved networks reconnect automatically when Wi-Fi starts. Select a saved network and tap **CONNECT** with an empty password to reuse its credentials; enter a new password to replace them after successful authentication. **FORGET** asks for confirmation. **ON/OFF** controls Wi-Fi and remembers the preference. **STOP** or the back arrow closes the password editor or returns to the recorder. **REC** and the three-second POWER hold remain available throughout these screens.

This implementation supports scanned open and WPA/WPA2 personal networks with ASCII passphrases (8–63 bytes) or 64-digit hexadecimal PSKs. Hidden-network entry, enterprise authentication, WEP and WPA3-only networks are not implemented.

The native Wi-Fi worker uses the firmware's `wpa_supplicant` 2.6 service, its [documented control socket protocol](https://w1.fi/wpa_supplicant/devel/ctrl_iface_page.html), and the existing `dhcpcd` service. It starts independently of the factory UI and runs separately from audio capture. Credentials travel in local HTTP POST bodies and Unix socket messages, and are excluded from API responses and command-line arguments. New personal-network passphrases are converted to PSKs using the device's OpenSSL runtime before storage. The factory-compatible profile file is `/data/cfg/wpa_supplicant.conf` (on userdata); the app restricts it to mode `0600` and creates an initial, private device-side backup at `/userdata/e1-recorder/wpa_supplicant.factory.conf`. These files contain credentials and stay on the recorder.

Tentative profiles are saved only after authentication succeeds. Cancel, an incorrect-password event or a 35-second authentication timeout removes the tentative profile and reloads the previous saved configuration. Scanning and changing settings remain independent of recording; USB ADB remains available when Wi-Fi is off.

## Diagnostics

The backend's control API listens on the recorder's loopback address, port `18770`. Deployment creates an ADB forward to the Mac. To inspect status manually:

```sh
adb -s 15dacc90f0c2e787 forward tcp:18770 tcp:18770
curl --noproxy '*' http://127.0.0.1:18770/api/status
adb -s 15dacc90f0c2e787 shell 'tail -50 /userdata/e1-recorder/audio.log'
adb -s 15dacc90f0c2e787 shell 'tail -50 /userdata/e1-recorder/ui.log'
```

The backend serves `/api/status`, `/api/files` and `/api/wave` through GET requests. Recording, marking, stopping, playback, seeking and shutdown use POST requests in `recorder-app/recorderd.c`.

Wi-Fi status and discovered/saved networks are available at `GET /api/wifi`. The form-encoded POST endpoints `/api/wifi/scan`, `/connect`, `/cancel`, `/enable` and `/forget` queue work in `recorder-app/wifi.c`. Connection fields are `ssid` (raw SSID bytes as hex), `security` (`open`, `WPA-PSK` or `saved`), and `password`; enable uses `enabled=0/1`, and forget uses the profile `id`. A successful POST acknowledges the queued command; poll Wi-Fi status for its result. Password editors with nonempty input are excluded from automatic screenshots.

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
