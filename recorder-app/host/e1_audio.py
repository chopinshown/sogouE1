#!/usr/bin/env python3
"""Bidirectional E1 RTP bridge for macOS/Windows; --help describes audio routing."""
import argparse
import collections
import contextlib
import json
import math
import pathlib
import select
import socket
import struct
import sys
import threading
import time
import wave

import numpy as np

from rtp import CHANNELS, FRAMES, RATE, Receiver, Sender


class Bridge:
    def __init__(self, args):
        self.args = args
        self.low_latency = getattr(args, 'buffer_ms', 120) < 40
        self.block_frames = FRAMES if self.low_latency else 440
        self.audio_latency_ms = getattr(args, 'audio_latency_ms', None) or (2.5 if self.low_latency else None)
        self.stream_latency = {}
        self.sender = Sender(not args.no_red, distance=2 if self.low_latency else 12)
        self.lock = threading.Lock()
        self.capture = collections.deque(maxlen=4 if self.low_latency else 400)
        self.capture_tail = np.empty((0, 2), dtype=np.int32)
        self.skipped_frames = 0
        self.running = True
        self.audio_error = None
        self.source_overruns = self.audio_xruns = self.dropped_send = self.dropped_receive = 0
        self.startup_source_overruns = self.startup_audio_xruns = 0
        self.rtcp_received = 0
        self.receiver = None

    def capture_callback(self, data, frames, timing, status):
        del frames, timing
        if status:
            self.audio_xruns += 1
        samples = np.rint(np.clip(data, -1, 1 - 2**-23) * 8388608).astype(np.int32)
        with self.lock:
            samples = np.concatenate([self.capture_tail, samples])
            blocks = len(samples) // FRAMES
            for index in range(blocks):
                if len(self.capture) == self.capture.maxlen:
                    self.source_overruns += 1
                    self.skipped_frames += FRAMES
                self.capture.append(samples[index * FRAMES:(index + 1) * FRAMES].copy())
            self.capture_tail = samples[blocks * FRAMES:].copy()

    def playback_callback(self, output, frames, timing, status):
        del timing
        if status:
            self.audio_xruns += 1
        try:
            with self.lock:
                samples = self.receiver.render(frames)
            output[:] = samples / 8388608.0
            if getattr(self.args, 'mute_receive', False):
                output.fill(0)
        except Exception as error:
            output.fill(0)
            self.audio_error = str(error)
            self.running = False

    def snapshot(self):
        with self.lock:
            result = self.receiver.stats() if self.receiver else {}
        return dict(result, sentPackets=self.sender.transmitted_packets, generatedPackets=self.sender.packets, rtcpPackets=self.rtcp_received,
                    sourceOverruns=self.source_overruns, audioXruns=self.audio_xruns,
                    startupSourceOverruns=self.startup_source_overruns, startupAudioXruns=self.startup_audio_xruns,
                    simulatedSendLoss=self.dropped_send, simulatedReceiveLoss=self.dropped_receive,
                    audioError=self.audio_error, audioBlockFrames=self.block_frames,
                    audioDeviceLatencyMs=self.stream_latency)

    def run(self):
        args = self.args
        server_ip = socket.gethostbyname(args.host)
        with contextlib.ExitStack() as stack:
            udp = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
            rtcp = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
            for sock in (udp, rtcp):
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
                with contextlib.suppress(OSError):
                    sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0xB8)
                sock.bind((args.bind, 0))
                sock.setblocking(False)
            for attempt in range(3):
                try:
                    connection = socket.create_connection((server_ip, args.port), timeout=6)
                    break
                except (TimeoutError, ConnectionRefusedError):
                    if attempt == 2:
                        raise
                    print('Waiting for E1 Wi-Fi Audio connection…', file=sys.stderr, flush=True)
                    time.sleep(0.5)
            control = stack.enter_context(connection)
            hello = f'E1RTP/1 {RATE} 24 2 {FRAMES} {udp.getsockname()[1]} {rtcp.getsockname()[1]} {self.sender.ssrc} {args.buffer_ms} {int(not args.no_red)}\n'
            control.sendall(hello.encode('ascii'))
            response = bytearray()
            while not response.endswith(b'\n'):
                part = control.recv(1)
                if not part or len(response) >= 255:
                    raise RuntimeError('E1 closed the session handshake')
                response.extend(part)
            fields = response.decode('ascii').strip().split()
            if len(fields) != 9 or fields[:5] != ['E1RTP/1', str(RATE), '24', '2', str(FRAMES)]:
                raise RuntimeError(response.decode('ascii', errors='replace'))
            rtp_port, rtcp_port, ssrc, buffer_ms = map(int, fields[5:])
            if not (1024 <= rtp_port <= 65535 and 1024 <= rtcp_port <= 65535 and 0 < ssrc <= 0xFFFFFFFF and 5 <= buffer_ms <= 200):
                raise RuntimeError('Invalid E1 session parameters')
            remote, remote_rtcp = (server_ip, rtp_port), (server_ip, rtcp_port)
            udp.connect(remote)
            self.receiver = Receiver(ssrc, buffer_ms)
            if args.sdp:
                pathlib.Path(args.sdp).write_text(
                    f'v=0\no=- 0 0 IN IP4 {server_ip}\ns=Sogou E1 stereo microphone\nc=IN IP4 {args.bind if args.bind != "0.0.0.0" else control.getsockname()[0]}\nt=0 0\nm=audio {udp.getsockname()[1]} RTP/AVP 96 97\na=rtpmap:96 L24/44100/2\na=rtpmap:97 red/44100/2\na=fmtp:97 96/96\na=recvonly\n')
            recording = None
            if args.record:
                recording = stack.enter_context(wave.open(args.record, 'wb'))
                recording.setparams((2, 3, RATE, 0, 'NONE', 'not compressed'))
            if args.send_device is not None or args.receive_device is not None:
                import sounddevice as sd
                core_settings = sd.CoreAudioSettings(change_device_parameters=True, fail_if_conversion_required=True) if sys.platform == 'darwin' and self.low_latency else None
                if args.send_device is not None:
                    stream = stack.enter_context(sd.InputStream(device=device_id(args.send_device), channels=2, samplerate=RATE,
                                                                 dtype='float32', blocksize=self.block_frames, latency=self.audio_latency_ms / 1000 if self.audio_latency_ms is not None else 'high', extra_settings=core_settings, callback=self.capture_callback))
                    self.stream_latency['input'] = round(stream.latency * 1000, 3)
                if args.receive_device is not None:
                    stream = stack.enter_context(sd.OutputStream(device=device_id(args.receive_device), channels=2, samplerate=RATE,
                                                                  dtype='float32', blocksize=self.block_frames, latency=self.audio_latency_ms / 1000 if self.audio_latency_ms is not None else 'high', extra_settings=core_settings, callback=self.playback_callback))
                    self.stream_latency['output'] = round(stream.latency * 1000, 3)
            control.sendall(b'START\n')
            ready = bytearray()
            while not ready.endswith(b'\n'):
                part = control.recv(1)
                if not part or len(ready) >= 255:
                    raise RuntimeError('E1 did not become audio-ready')
                ready.extend(part)
            if ready != b'READY\n':
                raise RuntimeError(ready.decode('ascii', errors='replace'))
            with self.lock:
                self.startup_source_overruns = self.source_overruns
                self.startup_audio_xruns = self.audio_xruns
                self.source_overruns = self.audio_xruns = 0
                self.skipped_frames = 0
                self.capture.clear()
                self.capture_tail = np.empty((0, 2), dtype=np.int32)
            control.setblocking(False)
            print(json.dumps(dict(event='connected', host=server_ip, rtpPort=rtp_port, localRtpPort=udp.getsockname()[1],
                                  sampleRate=RATE, bits=24, channels=2, bufferMs=buffer_ms, redundancy=not args.no_red)), flush=True)
            started = time.monotonic()
            next_send = next_render = last_ping = last_rtcp = last_stats = started
            sample_index = received_datagrams = 0
            try:
                while self.running and (not args.duration or time.monotonic() - started < args.duration):
                    now = time.monotonic()
                    if now - last_ping >= 1:
                        control.sendall(b'PING\n')
                        last_ping = now
                    if now - last_rtcp >= 5:
                        with self.lock:
                            report = self.sender.rtcp(self.receiver)
                        rtcp.sendto(report, remote_rtcp)
                        last_rtcp = now
                    if args.send_device is None:
                        if now >= next_send:
                            if args.tone:
                                phase = (sample_index + np.arange(FRAMES)) / RATE
                                amplitude = 8388607 * 10 ** (args.tone_db / 20)
                                samples = np.column_stack([np.sin(2 * math.pi * 440 * phase), np.sin(2 * math.pi * 880 * phase)]) * amplitude
                                samples = np.rint(samples).astype(np.int32)
                            else:
                                samples = np.zeros((FRAMES, 2), dtype=np.int32)
                            sample_index += FRAMES
                            self.transmit(udp, remote, samples)
                            next_send += FRAMES / RATE
                            if now - next_send > 0.1:
                                next_send = now
                    else:
                        with self.lock:
                            samples = self.capture.popleft() if self.capture else None
                            skipped = self.skipped_frames if samples is not None else 0
                            self.skipped_frames -= skipped
                        if samples is not None:
                            if skipped:
                                self.sender.skip(skipped)
                            self.transmit(udp, remote, samples)
                    if args.receive_device is None and now >= next_render:
                        with self.lock:
                            samples = self.receiver.render(self.block_frames)
                        if recording:
                            raw = np.clip(samples, -8388608, 8388607).astype('<i4').view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
                            recording.writeframesraw(raw)
                        next_render += self.block_frames / RATE
                        if now - next_render > 0.1:
                            next_render = now
                    readable, _, _ = select.select([udp, rtcp, control], [], [], 0.0005)
                    for sock in readable:
                        if sock is control:
                            reply = control.recv(512)
                            if not reply:
                                raise RuntimeError('E1 disconnected; press REC to rearm if needed')
                            continue
                        for _ in range(64):
                            try:
                                packet, source = sock.recvfrom(2048)
                            except BlockingIOError:
                                break
                            if sock is rtcp:
                                if source == remote_rtcp:
                                    self.rtcp_received += 1
                                continue
                            if source != remote:
                                continue
                            received_datagrams += 1
                            if args.drop_receive_every and received_datagrams % args.drop_receive_every == 0:
                                self.dropped_receive += 1
                                continue
                            with self.lock:
                                self.receiver.receive(packet)
                    if now - last_stats >= 1:
                        print(json.dumps(dict(event='stats', **self.snapshot())), flush=True)
                        last_stats = now
                    if self.audio_error:
                        raise RuntimeError(self.audio_error)
            finally:
                self.running = False
                with contextlib.suppress(OSError):
                    rtcp.sendto(self.sender.bye(), remote_rtcp)
                    control.sendall(b'STOP\n')
                final = dict(event='finished', **self.snapshot())
                if args.stats:
                    pathlib.Path(args.stats).write_text(json.dumps(final, indent=2) + '\n')
                print(json.dumps(final), flush=True)

    def transmit(self, udp, remote, samples):
        packet = self.sender.encode(samples)
        if self.args.drop_send_every and self.sender.packets % self.args.drop_send_every == 0:
            self.dropped_send += 1
        else:
            udp.send(packet)
            self.sender.mark_sent(len(packet))


def device_id(value):
    try:
        return int(value)
    except ValueError:
        return value


def arguments():
    parser = argparse.ArgumentParser(description=__doc__, epilog='receive-device is an OUTPUT device receiving E1 microphone audio; send-device is an INPUT device capturing computer playback. Use two independent virtual cables for full duplex to avoid a feedback loop. No audio devices: sends silence (or --tone) and optionally records E1 microphones to --record.')
    parser.add_argument('host', nargs='?', help='E1 Wi-Fi IPv4 address shown on its Audio page')
    parser.add_argument('--port', type=int, default=18772)
    parser.add_argument('--bind', default='0.0.0.0')
    parser.add_argument('--buffer-ms', type=int, choices=range(5, 201), metavar='5..200', default=120)
    parser.add_argument('--audio-latency-ms', type=float, help='requested host audio-device latency (default 2.5 ms below a 40 ms network buffer, otherwise PortAudio high setting)')
    parser.add_argument('--send-device', help='host audio INPUT supplying computer→E1 playback')
    parser.add_argument('--receive-device', help='host audio OUTPUT receiving E1→computer microphones')
    parser.add_argument('--mute-receive', action='store_true', help='mute the selected output while retaining its real audio clock (diagnostics)')
    parser.add_argument('--list-devices', action='store_true')
    parser.add_argument('--record', help='save received microphone audio as 24-bit stereo WAV (without receive-device)')
    parser.add_argument('--duration', type=float, default=0, help='seconds; 0 runs until Ctrl+C')
    parser.add_argument('--tone', action='store_true', help='send quiet 440 Hz left / 880 Hz right test tone')
    parser.add_argument('--tone-db', type=float, default=-36, help='test-tone peak dBFS (default -36)')
    parser.add_argument('--no-red', action='store_true', help='use plain L24 payload type 96 without redundancy')
    parser.add_argument('--drop-send-every', type=int, default=0, help='test only: intentionally drop every Nth sent packet')
    parser.add_argument('--drop-receive-every', type=int, default=0, help='test only: intentionally discard every Nth incoming packet')
    parser.add_argument('--stats', help='write final statistics JSON')
    parser.add_argument('--sdp', help='write negotiated microphone stream SDP for reference')
    args = parser.parse_args()
    if args.list_devices:
        import sounddevice as sd
        print(sd.query_devices())
        parser.exit()
    if not args.host:
        parser.error('provide the E1 Wi-Fi address')
    if not args.no_red and args.buffer_ms < 10:
        parser.error('RED requires at least a 10 ms jitter buffer')
    if args.audio_latency_ms is not None and not 1 <= args.audio_latency_ms <= 200:
        parser.error('audio-device latency must be 1..200 ms')
    if args.record and args.receive_device is not None:
        parser.error('--record currently requires no --receive-device')
    if args.tone and args.send_device is not None:
        parser.error('--tone cannot be combined with --send-device')
    if not -96 <= args.tone_db <= -6 or args.duration < 0 or args.drop_send_every < 0 or args.drop_receive_every < 0:
        parser.error('invalid duration, tone level, or loss injection interval')
    return args


if __name__ == '__main__':
    try:
        Bridge(arguments()).run()
    except KeyboardInterrupt:
        pass
    except Exception as error:
        print(f'E1 bridge: {error}', file=sys.stderr)
        sys.exit(1)
