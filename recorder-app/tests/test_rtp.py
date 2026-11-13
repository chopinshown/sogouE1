import pathlib
import struct
import sys
import unittest

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'host'))
from rtp import FRAMES, Receiver, Sender, pack_l24, unpack_l24


class RtpTests(unittest.TestCase):
    def test_signed_wire_samples(self):
        samples = np.array([[-8388608, 8388607], [-1, 0], [1, -2]], dtype=np.int32)
        self.assertEqual(pack_l24(samples), bytes.fromhex('8000007fffffffffff000000000001fffffe'))
        np.testing.assert_array_equal(unpack_l24(pack_l24(samples)), samples)

    def test_redundancy_burst_wrap(self):
        tx = Sender(ssrc=42, sequence=65530, timestamp=0xfffff000)
        rx = Receiver(42, 120)
        for p in range(50):
            samples = np.column_stack([np.full(FRAMES, p * 1000), np.full(FRAMES, -p * 1000 - 1)]).astype(np.int32)
            packet = tx.encode(samples)
            self.assertLessEqual(len(packet), 1337)
            if not 10 <= p < 14:
                self.assertTrue(rx.receive(packet, 100 + p * FRAMES / 44100))
        for p in range(10, 14):
            slot = rx.slots[(0xfffff000 + p * FRAMES) & 0xffffffff]
            self.assertTrue(slot[1])
            self.assertEqual(slot[0][0, 0], p * 1000)
        for _ in range(10):
            rx.render(440)
        self.assertEqual(rx.recovered, 4)
        self.assertEqual(rx.concealed, 0)
        self.assertGreater(rx.max_sequence, 65535)

    def test_malformed_does_not_change_state(self):
        tx = Sender(ssrc=123)
        rx = Receiver(123)
        packet = tx.encode(np.zeros((FRAMES, 2), dtype=np.int32))
        for size in range(len(packet)):
            self.assertFalse(rx.receive(packet[:size]))
        wrong = bytearray(packet)
        wrong[8] ^= 1
        self.assertFalse(rx.receive(wrong))
        self.assertIsNone(rx.origin)
        self.assertEqual(rx.packets, 0)
        self.assertTrue(rx.receive(packet))

    def test_plain_l24_padding_and_extension(self):
        tx = Sender(False, ssrc=123)
        rx = Receiver(123)
        packet = tx.encode(np.zeros((FRAMES, 2), dtype=np.int32))
        padded = bytes([packet[0] | 32]) + packet[1:] + b'\0\0\0\x04'
        self.assertTrue(rx.receive(padded))
        extended = bytes([packet[0] | 16]) + packet[1:12] + struct.pack('!HHI', 0xabcd, 1, 0) + packet[12:]
        self.assertTrue(rx.receive(extended))
        self.assertEqual(rx.unique_packets, 1)

    def test_bounded_window_and_silence(self):
        tx = Sender(False, ssrc=123)
        rx = Receiver(123)
        np.testing.assert_array_equal(rx.render(440), np.zeros((440, 2), dtype=np.int32))
        for _ in range(1000):
            rx.receive(tx.encode(np.zeros((FRAMES, 2), dtype=np.int32)))
        self.assertGreater(rx.overruns, 0)
        self.assertLessEqual(len(rx.slots), 256)
        self.assertGreaterEqual(rx.ratio, .998)

    def test_clock_drift_and_deliberate_loss(self):
        for ppm in (-500, 500):
            tx = Sender(ssrc=123)
            rx = Receiver(123, 120)
            packet_time = FRAMES / (44100 * (1 + ppm / 1e6))
            send_at = render_at = 0.0
            silence = np.zeros((FRAMES, 2), dtype=np.int32)
            while min(send_at, render_at) < 60:
                if send_at <= render_at:
                    packet = tx.encode(silence)
                    if tx.packets % 17:
                        rx.receive(packet, send_at)
                    send_at += packet_time
                else:
                    rx.render(440)
                    render_at += 440 / 44100
            self.assertGreater(rx.recovered, 1000)
            self.assertEqual(rx.concealed, 0)
            self.assertEqual(rx.overruns, 0)
            self.assertLess(rx.queue_frames / 44.1, 200)

    def test_bridge_audio_callbacks(self):
        from e1_audio import Bridge
        import argparse
        bridge = Bridge(argparse.Namespace(no_red=False))
        samples = np.column_stack([np.full(440, .25), np.full(440, -.5)]).astype(np.float32)
        bridge.capture_callback(samples, 440, None, False)
        self.assertEqual(len(bridge.capture), 4)
        self.assertEqual(bridge.capture[0][0].tolist(), [2097152, -4194304])
        bridge.receiver = Receiver(42, 40)
        tx = Sender(ssrc=42)
        for _ in range(20):
            bridge.receiver.receive(tx.encode(np.full((FRAMES, 2), 1000000)))
        output = np.empty((440, 2), dtype=np.float32)
        bridge.playback_callback(output, 440, None, False)
        self.assertIsNone(bridge.audio_error)
        np.testing.assert_allclose(output[-1], 1000000 / 8388608, atol=1e-6)

    def test_low_latency_clock_and_recovery(self):
        for ppm in (-500, 0, 500):
            tx = Sender(ssrc=123, distance=2)
            rx = Receiver(123, 15)
            send_at = render_at = 0.0
            silence = np.zeros((FRAMES, 2), dtype=np.int32)
            while min(send_at, render_at) < 30:
                if send_at <= render_at:
                    packet = tx.encode(silence)
                    if tx.packets % 17:
                        rx.receive(packet, send_at)
                    send_at += FRAMES / (44100 * (1 + ppm / 1e6))
                else:
                    rx.render(FRAMES)
                    render_at += FRAMES / 44100
            self.assertGreater(rx.recovered, 500)
            self.assertEqual(rx.concealed, 0)
            self.assertEqual(rx.overruns, 0)
            self.assertLessEqual(rx.queue_frames / 44.1, 20)

    def test_low_latency_bounds_and_callback_size(self):
        from e1_audio import Bridge
        import argparse
        bridge = Bridge(argparse.Namespace(no_red=False, buffer_ms=15))
        self.assertEqual(bridge.block_frames, FRAMES)
        self.assertEqual(bridge.audio_latency_ms, 2.5)
        self.assertEqual(bridge.sender.distance, 2)
        self.assertEqual(bridge.capture.maxlen, 4)
        tx = Sender(ssrc=123, distance=2)
        rx = Receiver(123, 15)
        for _ in range(80):
            rx.receive(tx.encode(np.zeros((FRAMES, 2), dtype=np.int32)))
        rx.render(FRAMES)
        self.assertEqual(rx.overruns, 1)
        self.assertLessEqual(rx.queue_frames / 44.1, 20)

    def test_low_latency_does_not_pause_clock_on_packet_gap(self):
        tx = Sender(ssrc=123, distance=2)
        rx = Receiver(123, 15)
        for _ in range(7):
            rx.receive(tx.encode(np.zeros((FRAMES, 2), dtype=np.int32)))
        rx.render(FRAMES)
        self.assertTrue(rx.started)
        before = rx.played
        for _ in range(40):
            rx.render(FRAMES)
        self.assertEqual(rx.played - before, 40 * FRAMES)
        self.assertGreater(rx.concealed, 0)
        self.assertTrue(rx.started)

    def test_source_overrun_preserves_rtp_clock(self):
        from e1_audio import Bridge
        import argparse
        bridge = Bridge(argparse.Namespace(no_red=False, buffer_ms=15))
        bridge.capture_callback(np.zeros((880, 2), dtype=np.float32), 880, None, False)
        self.assertEqual(bridge.source_overruns, 4)
        self.assertEqual(bridge.skipped_frames, 440)
        tx = Sender(ssrc=123, timestamp=1000, distance=2)
        for _ in range(3):
            tx.encode(np.zeros((FRAMES, 2), dtype=np.int32))
        before = tx.timestamp
        tx.skip(440)
        packet = tx.encode(np.zeros((FRAMES, 2), dtype=np.int32))
        self.assertEqual(struct.unpack_from('!I', packet, 4)[0], before + 440)
        self.assertEqual(len(packet), 12 + 1 + FRAMES * 6)


if __name__ == '__main__':
    unittest.main()
