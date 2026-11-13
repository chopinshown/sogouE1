# Xiaozhi on Sogou E1

## Upstream summary

[Xiaozhi ESP32](https://github.com/78/xiaozhi-esp32) is a voice-assistant client. The server supplies speech recognition, the language model and speech synthesis; the device captures and plays audio, manages conversation state, displays transcripts/emotions and exposes controls through MCP. It supports WebSocket or MQTT/UDP transports and Opus audio. ESP-SR supplies wake-word and voice-processing features on supported Espressif hardware. Board adapters provide the hardware-specific audio, network, display, keys and power interfaces.

This port was reviewed against upstream commit [`026e9b6e55e871089670c3909f85805b2a4b41f7`](https://github.com/78/xiaozhi-esp32/tree/026e9b6e55e871089670c3909f85805b2a4b41f7). Protocol references: [WebSocket](https://github.com/78/xiaozhi-esp32/blob/026e9b6e55e871089670c3909f85805b2a4b41f7/docs/websocket.md), [audio architecture](https://github.com/78/xiaozhi-esp32/blob/026e9b6e55e871089670c3909f85805b2a4b41f7/main/audio/README.md), [activation implementation](https://github.com/78/xiaozhi-esp32/blob/026e9b6e55e871089670c3909f85805b2a4b41f7/main/ota.cc), [MCP](https://github.com/78/xiaozhi-esp32/blob/026e9b6e55e871089670c3909f85805b2a4b41f7/docs/mcp-protocol.md). Upstream is MIT licensed; this is a Linux client implementing its protocol, rather than an ESP-IDF board build.

## E1 adaptation

The existing recorder app is the base. Its Qt Quick UI, managed launcher, Wi-Fi settings, WAV recording/browser and three-second shutdown remain in use. The app now opens on a Xiaozhi page; its back arrow opens the recorder, and **VOICE** returns to the assistant.

| Upstream component | E1 implementation |
| --- | --- |
| ESP-IDF / FreeRTOS application | Linux C worker in `recorder-app/xiaozhi.c` |
| Board audio codec / I2S | 48 kHz eight-channel PDM input `hw:2,0`, selected channels 6/7; physical speaker `plughw:0,0` |
| 16 kHz mono speech | Average selected PDM channels 6/7, low-pass filter and decimate 48→16 kHz by three, Opus at 24 kbit/s, 60 ms packets |
| Opus TTS output | Decode negotiated 8/12/16/24/48 kHz mono, sinc-resample to 48 kHz, duplicate to stereo, S24_LE output |
| ESP network abstraction | Static libcurl with Mbed TLS; verified HTTPS and WSS |
| Display adapter | `AssistantPage.qml`: state, mic level, activation code, user transcript and reply |
| Device identity / NVS | Actual wlan0 MAC, persistent UUID and private JSON under `/userdata/e1-recorder` |
| MCP device tools | Device status and software speaker volume |
| ESP32 firmware OTA | Ignored; existing E1 app packaging and ADB deployment retained |

The recorder audio thread closes its PCMs before handing ownership to the voice worker. Recording and RTP Wi-Fi audio are unavailable during a voice turn. Wi-Fi Audio has a separate page reached through **AUDIO**, and the recorder, assistant and RTP worker transfer exclusive PCM ownership before changing modes. Hardware capture/output use 48 kHz throughout; the cloud speech codec remains 16 kHz mono with filtered integer decimation on input. Capture discards 250 ms of microphone startup transients before listening. Ending or cancelling a session closes both voice PCMs before the recorder reacquires capture. The network/audio worker never runs on the UI or recorder control thread. Incoming messages are capped at 32 KiB; TTS has a bounded six-second packet queue and fails cleanly on overflow. Handshake, reply and idle deadlines prevent permanent sessions. Leaving the voice page cancels the turn.

Speech input uses a dedicated 3:1 decimator: average the main pair at 48 kHz, apply a 257-tap Blackman-windowed FIR (7.5 kHz cutoff), then retain every third filtered sample. Every 480 input frames produce exactly 160 mono samples, with no fractional-rate interpolation. The filter adds about 2.67 ms group delay and is computed only at the retained output positions, using coefficient symmetry. It rejects frequencies at/above the 16 kHz stream's Nyquist limit instead of folding them into speech. See the [ARM FIR decimation guidance](https://arm-software.github.io/CMSIS-DSP/main/group__FIR__decimate.html). State and phase persist across partial ALSA reads, and the filter tail is flushed before the final Opus packet. TTS still uses libsamplerate to convert negotiated reply rates to the 48 kHz physical output.

Capture uses nonblocking ALSA with an explicit `snd_pcm_start()`: the E1's configured capture start threshold exceeds each read request, so omitting this start leaves reads returning EAGAIN and sends no audio. Playback is also nonblocking, allowing cancellation while draining buffered speech. The E1 default speaker/processing path rejected direct S16_LE output at the server rate; the port uses the 48 kHz S24_LE format on the physical codec instead.

## Official-service setup and controls

1. Connect Wi-Fi using the existing **WI-FI** page.
2. Open **VOICE**, then press **M**. The official HTTPS endpoint is `https://api.tenclass.net/xiaozhi/ota/`.
3. Sign in at [xiaozhi.me](https://xiaozhi.me), add a device with the displayed code, then press **M** again. The client uses upstream activation version 1 and checks `/activate` before fetching connection settings again.
4. Press **REC**, speak, then press **STOP**. The client sends `listen/start` in manual mode, binary Opus packets and `listen/stop`. It displays STT and TTS sentence text and plays the response.

| Key | On the Xiaozhi page |
| --- | --- |
| REC | Start a voice turn; while listening, finish the turn |
| STOP | Finish listening; otherwise cancel an active operation; when idle, open recorder |
| M | Check activation when idle; cancel an active operation |
| + / − | Change software voice playback volume by 5% |
| POWER, held 3 s | Existing graceful device shutdown |

Touching the state strip starts/finishes a turn. Tap the activation-code area to check binding. Transcript text scrolls by touch. The Wi-Fi header opens settings when idle. On the recorder page, REC/Mark/Stop retain their original recording behavior and button-free layout.

`/userdata/e1-recorder/xiaozhi.json` holds activation URL, WebSocket URL, token, protocol version and optional `ca_bundle`. It is mode 0600 and excluded from Git, bundles, API responses and logs. `/userdata/e1-recorder/xiaozhi-client-id` preserves the UUID. No credentials are built into the app. TLS verifies both the hostname and certificate chain using `/etc/ssl/certs/ca-certificates.crt` unless a private configuration selects another CA bundle. No insecure-TLS switch is provided. The device clock must be correct for certificate validation. Temporary DNS/network failures can be retried with M or REC.

## Build and validation

Use the setup in the root README, plus CMake and Python 3.9+. `build-deps.py` downloads checksum-pinned curl 8.23.0, Mbed TLS 3.6.7, Opus 1.5.2 and libsamplerate 0.2.2, then cross-compiles static libraries with the existing Zig compiler. Downloads live in ignored `.tools/`, build products in ignored `recorder-app/build/`. cJSON 1.7.19 source and its MIT license are vendored. Dependency license notices are included as `voice-licenses.txt` in every app bundle.

```sh
make -C recorder-app deploy
sh recorder-app/test-voice.sh
# Host-only DSP/Opus checks, with pkg-config and host Opus installed:
sh recorder-app/test-speech.sh
# Optional integration test: same Wi-Fi as Mac en0, Homebrew libopus + openssl
python3 recorder-app/tests/voice_integration.py
```

The on-device unit test uses synthetic PCM, with assertions explicitly enabled. It checks 48→16 kHz integer decimation and microphone channels 6/7, 60 ms Opus encode/decode, HTTPS/WSS URL policy, header injection rejection, malformed/duplicate/unsupported hello messages, Unicode transcripts, stale sessions and ignored remote system commands. It opens no audio devices and sends no network traffic.

`GET /api/assistant` returns public state, activation code, transcripts, packet counters, level and volume. POST `/api/assistant/activate`, `/talk`, `/finish`, `/cancel`, `/volume-up` and `/volume-down` control the worker. These routes remain on the existing loopback-only API. `/api/status` reports `assistant` while it owns audio. App automatic screenshots skip the voice page.

## Verified on the E1

- Cross-build and deployment; clean QML startup.
- Recorder regression after voice sessions: physical REC/M/STOP and playback, one saved mark, zero xruns, and successful microphone reacquisition.
- Official HTTPS activation and account binding using the actual E1 MAC/UUID.
- Official WebSocket session: microphone Opus upload, recognized Chinese question, reply subtitles and 163 received/played TTS packets in a complete turn.
- Local TLS server: both 16 kHz and 24 kHz synthetic Opus replies played/drained; fragmented hello with interleaved ping; masked client frames, MCP initialize/list/volume, Unicode transcript and uplink. The integration test keeps the official profile backup on the device and restores it afterward.
- Codec/parser unit test: 48,000 decimated samples and 50 Opus packets from three seconds of synthetic 48 kHz input.

## Current scope

Implemented: official-service activation, WebSocket protocol v1, manual turns, Opus uplink/downlink, subtitles, basic MCP, cancellation, exclusive audio ownership and reuse of existing Wi-Fi/recorder/power UI.

Not implemented: ESP-SR wake words, VAD/automatic end-of-turn, AEC/full duplex, MQTT/UDP, protocol v2/v3, camera/vision, dynamic glyph assets, voice-controlled recorder tools and official firmware updates. The server may return ESP32 firmware metadata; the port never installs those images or obeys remote reboot/system commands. This branch retains the existing unsigned E1 app update format.

After the 48 kHz merge, the codec/filter tests cover 1 and 7 kHz passband tones, rejection of 8/9/12/20/23 kHz tones, chunk-boundary continuity, and exact 480→160 sample counts. Original official-service and mock-server results above predate this merge; the combined build has been compiled and its QML loaded offscreen, without replacing the installed application.
