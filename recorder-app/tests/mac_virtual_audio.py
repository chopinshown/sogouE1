#!/usr/bin/env python3
"""Check the real Mac virtual cables and E1 route; --tone plays quiet stereo tones."""
import argparse
import contextlib
import json
import math
import subprocess
import sys
import time

import numpy as np
import sounddevice as sd

from rtp_integration import ROOT, api, until


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='192.168.0.130')
    parser.add_argument('--seconds', type=int, default=15)
    parser.add_argument('--buffer-ms', type=int, default=200)
    parser.add_argument('--no-red', action='store_true')
    parser.add_argument('--tone', action='store_true')
    args = parser.parse_args()
    assert sys.platform == 'darwin'
    assert api('status')['state'] == 'idle', 'Stop the current audio job first'
    output = ROOT / 'evidence' / ('mac-virtual-' + time.strftime('%Y%m%d-%H%M%S'))
    output.mkdir(parents=True)
    for cable in ('BlackHole 2ch', 'BlackHole 16ch'):
        assert sd.query_devices(cable)['default_samplerate'] == 48000
    block_frames = 120 if args.buffer_ms < 40 else 480
    latency = .0025 if args.buffer_ms < 40 else .1
    source = {'frame': 0, 'xruns': 0}
    received = {'frames': 0, 'squares': np.zeros(2), 'peak': np.zeros(2), 'xruns': 0}

    def write(data, frames, timing, status):
        del timing
        source['xruns'] += int(bool(status))
        if args.tone:
            phase = (source['frame'] + np.arange(frames)) / 48000
            data[:, 0] = .0158489 * np.sin(2 * math.pi * 440 * phase)
            data[:, 1] = .0158489 * np.sin(2 * math.pi * 880 * phase)
        else:
            data.fill(0)
        source['frame'] += frames

    def read(data, frames, timing, status):
        del timing
        received['frames'] += frames
        received['squares'] += np.sum(data.astype(np.float64) ** 2, axis=0)
        received['peak'] = np.maximum(received['peak'], np.max(np.abs(data), axis=0))
        received['xruns'] += int(bool(status))

    api('network/start', True)
    until(lambda: api('network')['state'] == 'waiting')
    history = []
    process = None
    try:
        with contextlib.ExitStack() as stack:
            stack.enter_context(sd.OutputStream(device='BlackHole 16ch', samplerate=48000, channels=2,
                                                dtype='float32', blocksize=block_frames, latency=latency, callback=write))
            stack.enter_context(sd.InputStream(device='BlackHole 2ch', samplerate=48000, channels=2,
                                               dtype='float32', blocksize=block_frames, latency=latency, callback=read))
            log = stack.enter_context((output / 'bridge.log').open('w'))
            command = ['sh', str(ROOT / 'host/start-mac.sh'), args.host,
                       '--duration', str(args.seconds), '--buffer-ms', str(args.buffer_ms),
                       '--stats', str(output / 'host.json')]
            if args.no_red:
                command.append('--no-red')
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            while process.poll() is None:
                history.append(api('network'))
                time.sleep(.25)
            assert process.returncode == 0, (output / 'bridge.log').read_text()
        host = json.loads((output / 'host.json').read_text())
        final = api('network')
        rms = np.sqrt(received['squares'] / max(1, received['frames']))
        result = dict(device=final, host=host, virtualInputRms=rms.tolist(),
                      virtualInputPeak=received['peak'].tolist(),
                      virtualInputFrames=received['frames'], sourceFrames=source['frame'],
                      testSourceXruns=source['xruns'], testCaptureXruns=received['xruns'])
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        (output / 'device-history.json').write_text(json.dumps(history, indent=2) + '\n')
        assert received['frames'] > 48000 * args.seconds * .8
        assert np.all(rms > 1e-8), 'No microphone samples arrived through the Mac virtual input'
        assert final['receivedPackets'] > args.seconds * 300
        assert host['sentPackets'] > args.seconds * 300
        assert not host['audioError']
        if args.tone:
            assert max(s['outputLeftDb'] for s in history) > -40
            assert max(s['outputRightDb'] for s in history) > -40
        print('MAC_VIRTUAL_AUDIO_PASS BlackHole 2ch microphone / BlackHole 16ch playback, stereo 48000 Hz')
        print('QUALITY', json.dumps({
            'hostConcealed': host['concealedPackets'], 'deviceConcealed': final['concealedPackets'],
            'deviceSendDrops': final['sendDrops'], 'hostAudioXruns': host['audioXruns'],
            'deviceXruns': final['xruns']}))
        print('EVIDENCE', output)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        api('network/stop', True)
        until(lambda: api('status')['captureReady'])


if __name__ == '__main__':
    main()
