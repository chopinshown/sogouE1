"""E1 RTP v2 L24/RED profile; wire format shared with rtp_audio.c."""
import collections
import math
import secrets
import struct
import time

import numpy as np

RATE, FRAMES, CHANNELS = 44100, 110, 2
BLOCK_BYTES = FRAMES * CHANNELS * 3
PT_L24, PT_RED, RED_DISTANCE = 96, 97, 12
MAX_PACKET = 1400


def delta32(a, b):
    return ((a - b + 0x80000000) & 0xFFFFFFFF) - 0x80000000


def pack_l24(samples):
    values = np.clip(samples, -8388608, 8388607).astype('<i4')
    return values.view(np.uint8).reshape(-1, 4)[:, 2::-1].tobytes()


def unpack_l24(payload):
    b = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = (b[:, 0] << 16) | (b[:, 1] << 8) | b[:, 2]
    return ((v << 8) >> 8).reshape(-1, 2)


class Sender:
    def __init__(self, redundancy=True, ssrc=None, sequence=None, timestamp=None, distance=RED_DISTANCE):
        self.ssrc = ssrc or secrets.randbelow(0xFFFFFFFF) + 1
        self.sequence = secrets.randbits(16) if sequence is None else sequence
        self.timestamp = secrets.randbits(32) if timestamp is None else timestamp
        if not 1 <= distance <= RED_DISTANCE:
            raise ValueError('Invalid RED distance')
        self.distance = distance
        self.redundancy = redundancy
        self.history = collections.deque(maxlen=self.distance)
        self.packets = self.octets = 0
        self.transmitted_packets = self.transmitted_octets = 0

    def encode(self, samples):
        pcm = pack_l24(samples)
        if len(pcm) != BLOCK_BYTES:
            raise ValueError('Each packet requires 110 stereo frames')
        payload = pcm
        if self.redundancy:
            if len(self.history) == self.distance:
                offset = self.distance * FRAMES
                header = bytes([0x80 | PT_L24, offset >> 6,
                                ((offset << 2) & 0xFC) | (BLOCK_BYTES >> 8), BLOCK_BYTES & 255,
                                PT_L24])
                payload = header + self.history[0] + pcm
            else:
                payload = bytes([PT_L24]) + pcm
        header = struct.pack('!BBHII', 0x80,
                             (PT_RED if self.redundancy else PT_L24) | (0x80 if not self.packets else 0),
                             self.sequence & 65535, self.timestamp & 0xFFFFFFFF, self.ssrc)
        self.history.append(pcm)
        self.sequence = (self.sequence + 1) & 65535
        self.timestamp = (self.timestamp + FRAMES) & 0xFFFFFFFF
        self.packets += 1
        self.octets += len(payload)
        return header + payload

    def skip(self, frames):
        if frames < 0 or frames % FRAMES:
            raise ValueError('Skipped audio must contain complete RTP blocks')
        self.timestamp = (self.timestamp + frames) & 0xFFFFFFFF
        self.history.clear()

    def mark_sent(self, packet_size):
        self.transmitted_packets += 1
        self.transmitted_octets += packet_size - 12

    def rtcp(self, receiver):
        report = receiver.max_sequence is not None
        now = time.time()
        sr = struct.pack('!BBHIIIIII', 0x80 | int(report), 200, 12 if report else 6,
                         self.ssrc, int(now) + 2208988800,
                         int((now % 1) * 2**32), self.timestamp,
                         self.transmitted_packets & 0xFFFFFFFF, self.transmitted_octets & 0xFFFFFFFF)
        if report:
            expected = receiver.max_sequence - receiver.base_sequence + 1
            lost = min(0x7FFFFF, max(0, expected - receiver.unique_packets))
            interval_expected = expected - receiver.report_expected
            interval_received = receiver.unique_packets - receiver.report_received
            fraction = min(255, int(max(0, interval_expected - interval_received) * 256 / interval_expected)) if interval_expected else 0
            receiver.report_expected, receiver.report_received = expected, receiver.unique_packets
            sr += struct.pack('!I', receiver.ssrc) + bytes([fraction]) + lost.to_bytes(3, 'big')
            sr += struct.pack('!IIII', receiver.max_sequence & 0xFFFFFFFF, int(receiver.jitter), 0, 0)
        cname = ('mac-pc-%08x' % self.ssrc).encode('ascii')
        body = struct.pack('!I', self.ssrc) + bytes([1, len(cname)]) + cname + b'\0'
        body += b'\0' * (-len(body) % 4)
        return sr + struct.pack('!BBH', 0x81, 202, len(body) // 4) + body

    def bye(self):
        return struct.pack('!BBHI', 0x81, 203, 1, self.ssrc)


class Receiver:
    def __init__(self, ssrc, buffer_ms=60):
        self.ssrc = ssrc
        self.low_latency = buffer_ms < 40
        self.target = max(2 * FRAMES, math.floor(RATE * buffer_ms / (1000 * FRAMES) + .5) * FRAMES)
        self.filtered_queue = self.target
        self.ratio = 1.0
        self.origin = self.cursor = self.latest = None
        self.started = False
        self.slots = {}
        self.cache = np.empty((0, 2), dtype=np.int32)
        self.phase = 0.0
        self.fade = 220
        self.last_output = np.zeros(2, dtype=np.int32)
        self.previous = np.zeros(2, dtype=np.int32)
        self.gap = False
        self.packets = self.invalid = self.late = self.duplicates = 0
        self.recovered = self.concealed = self.played = self.overruns = 0
        self.max_sequence = self.base_sequence = None
        self.sequence_mask = self.unique_packets = 0
        self.report_expected = self.report_received = 0
        self.transit = None
        self.jitter = 0.0

    @property
    def queue_frames(self):
        if self.latest is None:
            return 0
        return max(0, delta32(self.latest, self.cursor) + len(self.cache))

    def _insert(self, timestamp, payload, red):
        if self.origin is None:
            self.origin = self.cursor = self.latest = timestamp
        if delta32(timestamp, self.origin) < 0 or delta32(timestamp, self.cursor) < 0:
            self.late += 1
            return
        if delta32(timestamp, self.cursor) >= 254 * FRAMES:
            self.slots.clear()
            self.cursor = timestamp
            self.cache = np.empty((0, 2), dtype=np.int32)
            self.phase = 0
            self.started = False
            self.filtered_queue = self.target
            self.overruns += 1
        if timestamp in self.slots:
            if not self.slots[timestamp][1] or red:
                self.duplicates += 1
                return
        self.slots[timestamp] = (unpack_l24(payload), red)
        end = (timestamp + FRAMES) & 0xFFFFFFFF
        if delta32(end, self.latest) > 0:
            self.latest = end

    def receive(self, packet, arrival=None):
        try:
            if len(packet) < 12 or len(packet) > MAX_PACKET:
                raise ValueError('size')
            flags, pt, sequence, timestamp, ssrc = struct.unpack_from('!BBHII', packet)
            if flags >> 6 != 2 or ssrc != self.ssrc:
                raise ValueError('identity')
            offset = 12 + (flags & 15) * 4
            size = len(packet)
            if offset > size:
                raise ValueError('CSRC')
            if flags & 16:
                if offset + 4 > size:
                    raise ValueError('extension')
                offset += 4 + struct.unpack_from('!H', packet, offset + 2)[0] * 4
                if offset > size:
                    raise ValueError('extension size')
            if flags & 32:
                pad = packet[-1]
                if not pad or pad > size - offset:
                    raise ValueError('padding')
                size -= pad
            payload = packet[offset:size]
            old = None
            if pt & 127 == PT_L24:
                if len(payload) != BLOCK_BYTES:
                    raise ValueError('L24 length')
                primary = payload
            elif pt & 127 == PT_RED:
                if not payload:
                    raise ValueError('empty RED')
                if payload[0] & 128:
                    if len(payload) != 5 + 2 * BLOCK_BYTES or payload[0] & 127 != PT_L24:
                        raise ValueError('RED length/type')
                    delta = payload[1] << 6 | payload[2] >> 2
                    length = (payload[2] & 3) << 8 | payload[3]
                    if (not delta or delta % FRAMES or delta > RED_DISTANCE * FRAMES) or length != BLOCK_BYTES or payload[4] != PT_L24:
                        raise ValueError('RED profile')
                    old = ((timestamp - delta) & 0xFFFFFFFF, payload[5:5 + length])
                    primary = payload[5 + length:]
                else:
                    if payload[0] != PT_L24 or len(payload) != BLOCK_BYTES + 1:
                        raise ValueError('RED primary')
                    primary = payload[1:]
            else:
                raise ValueError('payload type')
            if self.origin is not None and delta32(timestamp, self.origin) % FRAMES:
                raise ValueError('timestamp alignment')
        except (ValueError, struct.error):
            self.invalid += 1
            return False
        if self.max_sequence is None:
            self.base_sequence = self.max_sequence = sequence
            self.sequence_mask = 1
            self.unique_packets += 1
        else:
            delta = ((sequence - self.max_sequence + 32768) & 65535) - 32768
            if delta > 0:
                self.max_sequence += delta
                self.sequence_mask = ((self.sequence_mask << delta) | 1) & ((1 << 64) - 1) if delta < 64 else 1
                self.unique_packets += 1
            elif delta > -64 and not self.sequence_mask & (1 << -delta):
                self.sequence_mask |= 1 << -delta
                self.unique_packets += 1
        arrival = time.monotonic() if arrival is None else arrival
        transit = delta32(int(arrival * RATE), timestamp)
        if self.transit is not None:
            self.jitter += (abs(delta32(transit, self.transit)) - self.jitter) / 16
        self.transit = transit
        self.packets += 1
        if self.origin is None:
            self._insert(timestamp, primary, False)
            return True
        if old:
            self._insert(old[0], old[1], True)
        self._insert(timestamp, primary, False)
        return True

    def _pop(self):
        slot = self.slots.pop(self.cursor, None)
        self.cursor = (self.cursor + FRAMES) & 0xFFFFFFFF
        if slot:
            samples, red = slot
            self.recovered += int(red)
            if self.gap:
                samples = np.rint(samples * (np.arange(FRAMES) / FRAMES)[:, None]).astype(np.int32)
                self.gap = False
        else:
            self.concealed += 1
            self.gap = True
            samples = (self.previous * ((FRAMES - 1 - np.arange(FRAMES)) / FRAMES)[:, None]).astype(np.int32)
        self.previous = samples[-1].copy()
        return samples

    def render(self, frames):
        if ((self.started and not self.low_latency) or (not self.started and self.low_latency)) and self.queue_frames > self.target + (RATE * .005 if self.low_latency else RATE / 10):
            self.cursor = (self.latest - math.ceil(self.target / FRAMES) * FRAMES) & 0xFFFFFFFF
            self.slots = {t: slot for t, slot in self.slots.items() if delta32(t, self.cursor) >= 0}
            self.cache = np.empty((0, 2), dtype=np.int32)
            self.phase = 0
            self.started = False
            self.fade = 220
            self.filtered_queue = self.target
            self.overruns += 1
            return (self.last_output * ((frames - 1 - np.arange(frames)) / frames)[:, None]).astype(np.int32)
        if not self.started:
            if self.queue_frames < self.target:
                return np.zeros((frames, 2), dtype=np.int32)
            self.started = True
        if not self.low_latency and self.queue_frames < frames * 1.002 + 2:
            self.started = False
            self.fade = 220
            self.concealed += 1
            fade = (self.last_output * ((frames - 1 - np.arange(frames)) / frames)[:, None]).astype(np.int32)
            self.previous.fill(0)
            return fade
        if not self.low_latency or self.queue_frames >= 2 * FRAMES:
            self.filtered_queue += (self.queue_frames - self.filtered_queue) * (1 - .99 ** (frames / 440))
            self.ratio = min(1.002, max(0.998, 1 + (self.filtered_queue - self.target) * (.000005 if self.low_latency else .0000005)))
        positions = self.phase + np.arange(frames) * self.ratio
        needed = math.floor(self.phase + frames * self.ratio) + 2
        chunks = [self.cache]
        available = len(self.cache)
        while available < needed:
            chunks.append(self._pop())
            available += FRAMES
        self.cache = np.concatenate(chunks) if len(chunks) > 1 else self.cache
        indexes = positions.astype(np.int64)
        fraction = (positions - indexes)[:, None]
        values = self.cache[indexes].astype(np.float64)
        result = np.rint(values + fraction * (self.cache[indexes + 1].astype(np.float64) - values)).astype(np.int32)
        used = math.floor(self.phase + frames * self.ratio)
        self.phase = self.phase + frames * self.ratio - used
        self.cache = self.cache[used:]
        if self.fade:
            n = min(frames, self.fade)
            gain = (220 - self.fade + np.arange(n)) / 220
            result[:n] = np.rint(result[:n] * gain[:, None]).astype(np.int32)
            self.fade -= n
        self.last_output = result[-1].copy()
        self.played += frames
        return result

    def stats(self):
        return dict(receivedPackets=self.packets, recoveredPackets=self.recovered,
                    concealedPackets=self.concealed, invalidPackets=self.invalid,
                    lateBlocks=self.late, overruns=self.overruns,
                    queueMs=round(self.queue_frames * 1000 / RATE, 2),
                    jitterMs=round(self.jitter * 1000 / RATE, 3),
                    clockPpm=round((self.ratio - 1) * 1e6, 1), playedFrames=self.played)
