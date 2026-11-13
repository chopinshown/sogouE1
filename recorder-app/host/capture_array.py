#!/usr/bin/env python3
"""Capture eight synchronous E1 microphone channels, then restore the idle app.

The WAV contains room audio. Default destination is the ignored evidence folder.
Refuses to interrupt recording, playback, or an armed Wi-Fi audio interface.
"""
import argparse
import array
import json
import math
from pathlib import Path
import subprocess
import sys
import time
import urllib.request
import wave

ROOT = Path(__file__).resolve().parents[1]
ADB = ['adb', '-s', '15dacc90f0c2e787']
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def shell(command, timeout=30):
    p = subprocess.run(ADB + ['shell', command], capture_output=True, text=True, timeout=timeout)
    if p.returncode:
        raise RuntimeError(p.stdout + p.stderr)
    return p.stdout.replace('\r', '').strip()


def api(name):
    return json.load(OPENER.open('http://127.0.0.1:18770/api/' + name, timeout=4))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=8, choices=range(3, 61), metavar='3..60')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = args.output or ROOT / 'evidence' / ('aux-' + time.strftime('%Y%m%d-%H%M%S'))
    output.mkdir(parents=True, exist_ok=False)
    binary = ROOT / 'build/array-capture'
    binary.parent.mkdir(exist_ok=True)
    subprocess.run([str(ROOT.parent / '.tools/zig-aarch64-macos-0.14.1/zig'), 'cc',
                    '-target', 'aarch64-linux-gnu.2.26', '-O2', '-Wall', '-Wextra',
                    '-I' + str(ROOT / 'vendor'), str(ROOT / 'tests/array_capture.c'),
                    str(ROOT / 'sysroot/usr/lib/libasound.so.2'), '-o', str(binary)], check=True)
    subprocess.run(ADB + ['forward', 'tcp:18770', 'tcp:18770'], check=True, capture_output=True)
    state, network = api('status'), api('network')
    if state['state'] != 'idle' or network['armed']:
        raise RuntimeError('Finish recording/playback and disable Wi-Fi Audio first')
    subprocess.run(ADB + ['push', str(binary), '/tmp/e1-array-capture'], check=True, capture_output=True)
    backend = int(shell('cat /userdata/e1-recorder/audio.pid'))
    exe = shell('readlink /proc/%d/exe' % backend)
    assert exe.startswith('/userdata/e1-recorder/releases/') and exe.endswith('/recorderd'), exe
    remote = '/mnt/sdcard/Recordings/.aux-capture-%d.wav' % int(time.time())
    try:
        shell('kill -TERM %d' % backend)
        for _ in range(50):
            if shell('if [ -d /proc/%d ]; then echo running; else echo stopped; fi' % backend) == 'stopped':
                break
            time.sleep(.1)
        else:
            raise RuntimeError('Audio backend did not exit')
        print('Capturing room audio: channels 0–5 auxiliary, 6–7 main.', flush=True)
        result = shell('/tmp/e1-array-capture %s %d' % (remote, args.seconds), timeout=args.seconds + 12)
        assert 'ARRAY_CAPTURE_PASS' in result, result
        (output / 'capture.txt').write_text(result + '\n')
        subprocess.run(ADB + ['pull', remote, str(output / 'array.wav')], check=True)
    finally:
        launcher = int(shell('cat /userdata/e1-recorder/launcher.pid'))
        assert '/userdata/system/SogouE1' in shell('tr "\\000" " " < /proc/%d/cmdline' % launcher)
        shell('kill -TERM %d' % launcher)
        for _ in range(40):
            try:
                if api('status')['captureReady']:
                    break
            except Exception:
                pass
            time.sleep(.5)
        else:
            raise RuntimeError('Managed recorder did not restart')
        # Delete only this diagnostic's temporary copy; keep the Mac artifact.
        shell('rm -f ' + remote)
    with wave.open(str(output / 'array.wav'), 'rb') as f:
        assert (f.getframerate(), f.getnchannels(), f.getsampwidth(), f.getnframes()) == (48000, 8, 3, args.seconds * 48000)
        raw = f.readframes(f.getnframes())
    channels = [array.array('i') for _ in range(8)]
    for i in range(0, len(raw), 3):
        v = int.from_bytes(raw[i:i + 3], 'little', signed=True)
        channels[(i // 3) % 8].append(v)
    stats = []
    for c, values in enumerate(channels):
        mean = sum(values) / len(values)
        rms = math.sqrt(sum((v - mean) ** 2 for v in values) / len(values)) / 8388608
        stats.append({'channel': c, 'rmsDb': 20 * math.log10(max(rms, 1e-12)),
                      'peakDb': 20 * math.log10(max(max(abs(v) for v in values) / 8388608, 1e-12)),
                      'nonzeroSamples': sum(v != 0 for v in values)})
    (output / 'channels.json').write_text(json.dumps(stats, indent=2) + '\n')
    print(json.dumps(stats, indent=2))
    print('Saved:', output)


if __name__ == '__main__':
    main()
