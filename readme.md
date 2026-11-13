# Sogou E1 Recorder

A custom recorder for the Sogou E1, with physical-button controls, a native ALSA audio backend and a Qt Quick display. Applications are built on the Mac and installed through ADB using the firmware's writable application override.

## Recorder features

- Record **48,000 Hz, 24-bit, stereo PCM WAV** files.
- Live left/right level meters with peak hold.
- Physical Record, Mark and Stop controls; a button-free recording screen.
- Marks stored as embedded WAV cue points and in a JSON sidecar.
- Recording browser, stereo waveform preview, playback and seeking.
- Hardware and touch file selection, waveform seeking and mark navigation.
- Wi-Fi scanning, password entry, saved networks and connection/IP status.
- Wi-Fi Audio mode: simultaneous 24-bit stereo microphone capture and physical output over RTP/UDP, with a Mac/Windows Python bridge.

Recordings are saved in the large recording partition:

```text
/mnt/sdcard/Recordings/
  REC-YYYYMMDD-HHMMSS-000.wav
  REC-YYYYMMDD-HHMMSS-000.wav.marks.json
  REC-YYYYMMDD-HHMMSS-000.wav.wave.json
```

Capture opens all eight PDM slots on `hw:2,0` at 48 kHz, then selects zero-based channels **6 and 7** as the stereo recording and network source. These slots carry the external-ADC/main-microphone pair; they were silent at 44.1 kHz with the tested firmware, while 32/48/96 kHz capture produced audio on all eight slots. The selected signed 24-bit samples arrive left-aligned in 32-bit words and are packed into three-byte WAV samples. Capture runs at 48 kHz without software resampling. Playback uses an app-local ALSA plug targeting the physical `hw:0,0` codec at 48 kHz. Existing 44.1 kHz stereo 24-bit WAV files remain visible and playable; an app-local ALSA plug converts playback to 48 kHz while preserving source timing and seeking. The factory `default` ALSA path feeds the software loopback; `eq_drc_process` supplies its playback processing/routing. The custom application uses the physical codec directly.

## Wi-Fi audio interface

This branch adds an experimental stereo network interface. The E1 runs the native audio/network worker; a computer bridge connects the stream to real or virtual audio devices. The recorder and network worker have exclusive ownership of ALSA: stop network mode before recording or previewing a WAV. The recording page retains its button-free layout.

On the recorder, tap **AUDIO** in the top bar, then press **REC** or tap the status panel to enable network audio. The page shows the Wi-Fi IP address while waiting and the computer address when connected, with separate microphone and output meters. **STOP** disables the interface; a second STOP returns to the recorder. **M** opens Wi-Fi settings when the interface is off. Holding POWER for three seconds still performs the normal graceful shutdown.

Install the computer bridge dependencies in a virtual environment (Python 3.10 or newer):

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r recorder-app/host/requirements.txt
python recorder-app/host/e1_audio.py --list-devices
```

On Windows, use `py -m venv .venv` and `.venv\Scripts\Activate.ps1` in PowerShell instead of the first two commands. The bridge uses NumPy and python-sounddevice/PortAudio. macOS and Windows sounddevice wheels include PortAudio; other platforms may require a separate PortAudio installation.

For a transport test without opening computer audio devices:

```sh
# Replace this address with the IP shown on the E1.
# Sends silence to the E1 and saves its microphone stream as stereo 24-bit WAV.
python recorder-app/host/e1_audio.py 192.168.0.130 \
  --duration 20 --record recorder-app/evidence/e1-test.wav \
  --stats recorder-app/evidence/e1-test.json

# Quiet left 440 Hz / right 880 Hz physical-output test.
python recorder-app/host/e1_audio.py 192.168.0.130 --tone --duration 15
```

For normal desktop applications, install two independent virtual audio cables, then route them as follows. The bridge does not install drivers or change the computer's default audio devices.

| Path | Desktop application chooses | Bridge argument |
| --- | --- | --- |
| E1 microphones → application | Virtual cable A's recording/input endpoint | `--receive-device` selects cable A's playback/output endpoint |
| Application → E1 output | Virtual cable B's playback/output endpoint | `--send-device` selects cable B's recording/input endpoint |

macOS can use independent [BlackHole](https://github.com/ExistentialAudio/BlackHole) devices, for example the 2-channel and 16-channel variants, with only the first two channels used on each. Windows can use separate [VB-CABLE](https://vb-audio.com/Cable/) cables, such as A and B. Driver installation may require administrator authentication and a restart of audio applications. Use the device indexes printed by `--list-devices`:

```sh
# Example indexes only: select your virtual devices from --list-devices.
python recorder-app/host/e1_audio.py 192.168.0.130 \
  --receive-device 5 --send-device 6 --buffer-ms 120
```

For the tested Mac configuration, install **BlackHole 2ch and BlackHole 16ch** (signed/notarized version 0.7.1 was checked), then reload Core Audio or restart the Mac so the devices appear. They can also be installed with `brew install --cask blackhole-2ch blackhole-16ch`. The Mac launcher uses the Command Line Tools C compiler to build a small Core Audio helper that sets only these two virtual devices to 48 kHz:

```sh
# E1: AUDIO → REC. Then run on the Mac:
sh recorder-app/host/start-mac.sh 192.168.0.130 --buffer-ms 15 --no-red
```

In your desktop application's audio settings select **BlackHole 2ch as the microphone/input**, and **BlackHole 16ch as the playback/output**, using stereo channels 1–2. The E1 microphones feed the first device; application playback on the second reaches the E1's physical output. The launcher leaves the Mac's default audio-device selection alone. Ctrl+C closes the bridge; STOP on the E1 disables network mode. The Mac launcher defaults to a 15 ms network target (720 stereo frames, exactly 15 ms). The example disables redundancy to reduce Wi-Fi traffic; omit `--no-red` to enable the short recovery copy. Larger buffers can be selected explicitly when deadline misses cause gaps. The Python CLI's general-purpose default remains 120 ms. These settings do not include the additional Core Audio/ALSA latency.

Use different cables for the two directions; using both ends of one cable feeds the microphone stream back into the E1 output. Configure both virtual cables for 48 kHz stereo (use the first two channels of a larger cable). The bridge's direction names are from the computer's perspective: **receive** feeds an output device, and **send** captures an input device. It opens exactly two channels at 48 kHz. This is a companion-software interface; Wi-Fi alone does not enumerate a sound device in the OS. A dedicated branded HAL/WDM/ASIO driver and installer are outside this prototype. Windows code is provided but has not been exercised on a Windows host.

### Wire format and buffering

- RTP v2 over unicast IPv4 UDP; [L24/48000/2](https://www.rfc-editor.org/rfc/rfc3190.html), dynamic payload type **96**, big-endian signed 24-bit samples, interleaved left/right.
- **120 frames per packet**, exactly 2.5 ms. RTP timestamps count stereo frames, not individual channel samples. Sequence, timestamp and SSRC start randomly for each session.
- Default [RFC 2198 redundant audio](https://www.rfc-editor.org/rfc/rfc2198.html), dynamic payload type **97**. Each packet carries the primary L24 block and, after warmup, a copy from **two packets earlier** (about 5 ms) below a 40 ms buffer setting, or **12 packets earlier** (about 30 ms) with larger buffers. The maximum RTP datagram is **1,457 bytes**, below the normal LAN MTU. Plain L24 is available with `--no-red`.
- Raw audio is 2.304 Mbps each way. RED approximately doubles that, to 4.608 Mbps each way before RTP/UDP/IP overhead. The local link sustained approximately 5 Mbps each way in the preceding bandwidth test, but packet loss and delay remain variable.
- Receiver jitter buffers reorder by RTP timestamp, deduplicate primary/redundant blocks, recover missing audio, and count unrecoverable blocks. Adaptive linear interpolation follows independent audio clocks, limited to ±2,000 ppm. Source WAV recording remains native-rate; network playback/host capture can be resampled for clock correction.
- The Mac launcher defaults to **15 ms** and the standalone Python CLI to **120 ms**. `--buffer-ms` accepts 10–200 ms with RED, or 5–200 ms for plain L24. Targets are rounded to whole 120-frame packets. This is only the network buffer: ALSA, PortAudio and virtual devices add latency. Actual end-to-end audio latency has not been measured.
- UDP sockets request DSCP EF priority, which depends on the LAN honoring it. Nonblocking ALSA capture/output run together. The sender paces queued packets and retries temporary UDP backpressure for a bounded interval. Low-latency mode (below 40 ms) preserves the playout clock, conceals missing blocks, and discards late packets instead of repeatedly pausing to refill. Larger-buffer mode retains fade/rebuffer recovery. Both trim stale startup backlog.
- Low-latency mode requests **480-frame ALSA buffers** (10 ms), **120-frame periods** (2.5 ms), and **120-frame Mac callbacks** with approximately 2.5 ms requested device latency. PortAudio is allowed to configure its Core Audio client parameters. The API reports actual ALSA sizes and the bridge reports device latency; these are separate from the network target. Startup queue drops are reported separately, and dropping host capture data advances the RTP timestamp so playback stays on the original clock.
- Basic compound RTCP Sender/Receiver Reports and CNAME are exchanged every five seconds. RTCP BYE and TCP heartbeat/disconnect release the session. A disconnected computer can connect again while the E1 remains armed.

The real vendor driver reports **LPS MAX / IPS Card Disable** even when generic `iwconfig` says power management is off. Audio mode snapshots the actual modes from `/proc/net/rtl8723ds/wlan0/ps_info`, invokes `iwpriv wlan0 pm_set lps=0` and `ips=0`, and restores the snapshot on normal stop/shutdown. The private ioctl is implemented in the [Rockchip/Realtek driver](https://github.com/rockchip-linux/kernel/blob/develop-4.4/drivers/net/wireless/rockchip_wlan/rtl8723ds/os_dep/linux/ioctl_linux.c). No saved Wi-Fi profiles or firmware files are changed.

### Session/control API

The existing HTTP API stays on device loopback port 18770:

- `GET /api/network`: armed/active state, meters, packet/recovery/gap counters, ALSA xruns, queue depth and clock correction.
- `POST /api/network/start`: reserve the audio hardware and enable the LAN listener.
- `POST /api/network/stop` or `/api/stop`: disarm and return ownership to the recorder.

Only while armed, a dedicated TCP listener on **18772** accepts one computer. The bridge sends this ASCII line (decimal unsigned integers):

```text
E1RTP/1 48000 24 2 110 HOST_RTP_PORT HOST_RTCP_PORT HOST_SSRC BUFFER_MS RED_ENABLED\n
```

The reply is:

```text
E1RTP/1 48000 24 2 110 18774 18775 E1_SSRC BUFFER_MS\n
```

After preparing its audio devices, the client sends `START\n`; the E1 opens its PCMs, discards 250 ms of PDM microphone startup transients, and replies `READY\n` before media begins. This settling time occurs once per connection. Ports 18774/18775 carry RTP/RTCP. The peer IP comes from the accepted TCP connection; inbound audio must match that IP, the negotiated UDP source port and SSRC. The client sends `PING\n` once per second and receives `PONG\n`; five seconds without a heartbeat, TCP EOF, STOP, or RTCP BYE ends the media session. The bridge retries initial connection attempts; a failed active session requires rerunning it. There is no pairing, encryption, SRTP or Internet traversal: use an isolated/trusted LAN and arm the interface only when needed. Device ports close when disarmed. `--sdp path.sdp` saves negotiated microphone stream parameters for reference.

### Validation and current limits

```sh
. .venv/bin/activate
sh recorder-app/test-rtp.sh
# Connected E1 required; uses ADB for control only and Wi-Fi for all media.
python recorder-app/tests/rtp_integration.py --host 192.168.0.130
python recorder-app/tests/legacy_playback.py
# macOS with the virtual drivers installed and configured at 48 kHz:
python recorder-app/tests/mac_virtual_audio.py --host 192.168.0.130 --buffer-ms 15
```

The C unit suite checks the shared eight-channel→stereo extractor with distinct channel sentinels (including a full-scale negative sample), and runs under AddressSanitizer/UndefinedBehaviorSanitizer and exercises L24 sample layout, packet-loss burst recovery, sequence/timestamp wrapping, RTCP and malformed-packet fuzzing. Python tests cover signed sample boundaries, RTP extensions/padding, bounded buffering, ±500 ppm clock drift with deliberate loss, and desktop audio callbacks. The device integration test checks duplex streaming, deliberate loss recovery, exclusive hardware ownership, RTCP, reconnect/plain L24, and record/mark/stop/waveform/playback after network mode.

Physical headphone output was confirmed on the E1, including periods of clean playback, but the user also heard intermittent clicks. Low-buffer tests run at a 15 ms target with zero hardware xruns, but still show frequent deadline misses; disabling redundancy reduced misses in the tested microphone path. A sub-20 ms end-to-end latency has not been established. Live tests with larger buffers report recovery of hundreds of intentionally dropped packets and zero ALSA xruns; unrecoverable packets and rebuffering still occur on this LAN. The transport is functional and remains experimental, with audible dropouts possible. It is not yet suitable for dependable low-latency instrument monitoring. A muted real Core Audio output-clock test passed without PortAudio xruns, while microphone-network gaps remained. Mac virtual-driver routing has been tested with BlackHole 2ch/16ch at 48 kHz in both directions. Windows hardware testing remains to be performed; desktop endpoints are supplied by the separately installed virtual audio drivers.

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
