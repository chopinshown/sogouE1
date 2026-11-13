#!/usr/bin/env python3
"""E1 noise/monitor lifecycle test. Creates only short owned WAV fixtures."""
import json
from pathlib import Path
import subprocess
import time
import urllib.error
import urllib.request
import wave

ROOT = Path(__file__).resolve().parents[1]
ADB = ['adb', '-s', '15dacc90f0c2e787']
OP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def api(path, post=False, expected=200):
    req = urllib.request.Request('http://127.0.0.1:18770/api/' + path, data=b'' if post else None)
    try:
        reply = OP.open(req, timeout=5)
    except urllib.error.HTTPError as e:
        reply = e
    with reply:
        result = json.load(reply)
        assert reply.code == expected, (path, reply.code, result)
        return result


def until(test, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        s = api('status')
        if test(s):
            return s
        time.sleep(.05)
    raise AssertionError(s)


def main():
    subprocess.run(ADB + ['forward', 'tcp:18770', 'tcp:18770'], check=True, capture_output=True)
    assert api('status')['state'] == 'idle' and not api('network')['armed']
    out = ROOT / 'evidence' / ('noise-test-' + time.strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True)
    created = []
    try:
        api('monitor/volume?value=101', True, 400)
        api('monitor/volume?value=25', True)
        api('monitor/start', True)
        until(lambda s: s['monitor']['active'])
        api('noise/learn', True)
        api('record', True, 503)
        state = until(lambda s: not s['noise']['learning'])
        assert state['noise']['ready']
        assert len(state['noise']['auxiliaryDb']) == 6 and min(state['noise']['auxiliaryDb']) > -96
        history = []
        for mode in ('off', 'on'):
            api('noise/' + mode, True)
            api('record', True)
            s = until(lambda s: s['state'] == 'recording')
            name = s['file'];assert name.startswith('REC-');created.append(name)
            api('noise/learn', True, 409)
            api('noise/off', True, 409)
            time.sleep(.6);api('mark', True)
            # Wet recording crosses the 5- and 10-second fdatasync points.
            time.sleep(.7 if mode == 'off' else 10.7);api('stop', True)
            final = until(lambda s: s['state'] == 'idle')
            assert final['duration'] > 1 and final['xruns'] == 0
            assert 0 < final['marks'][0]['seconds'] < final['duration']
            waveform = api('wave?file=' + name)
            assert waveform['noiseReduced'] == (mode == 'on')
            subprocess.run(ADB + ['pull', '/mnt/sdcard/Recordings/' + name, str(out / name)], check=True, capture_output=True)
            with wave.open(str(out / name), 'rb') as f:
                assert (f.getframerate(), f.getnchannels(), f.getsampwidth()) == (48000, 2, 3)
                assert f.getnframes() == waveform['frames']
            api('play?file=' + name, True)
            until(lambda s: s['state'] == 'playing' and not s['monitor']['active'])
            api('stop', True)
            until(lambda s: s['state'] == 'idle' and s['monitor']['active'] and s['captureReady'])
        api('network/start', True)
        until(lambda s: s['state'] == 'network' and not s['monitor']['active'])
        api('monitor/start', True, 409)
        api('network/stop', True)
        until(lambda s: s['state'] == 'idle' and s['monitor']['active'])
        for _ in range(100):
            history.append(api('status'));time.sleep(.1)
        (out / 'status.json').write_text(json.dumps(history, indent=2) + '\n')
        assert all(s['xruns'] == 0 and s['monitor']['xruns'] == 0 and not s['error'] for s in history), history[-1]
        params = subprocess.run(ADB + ['shell', 'cat /proc/asound/card0/pcm0p/sub0/hw_params'], check=True, capture_output=True, text=True).stdout
        (out / 'monitor-pcm.txt').write_text(params)
        assert 'rate: 48000' in params and 'channels: 2' in params
        print('NOISE_INTEGRATION_PASS six aux inputs, dry/wet recording+cue+tail, headphone PCM, playback/network handoff, no xruns')
    finally:
        api('stop', True)
        until(lambda s: s['state'] == 'idle')
        api('noise/off', True)
        api('monitor/stop', True)
        until(lambda s: not s['monitor']['active'])
        for name in created:
            paths = ['"/mnt/sdcard/Recordings/' + name + suffix + '"' for suffix in ('', '.marks.json', '.wave.json')]
            subprocess.run(ADB + ['shell', 'rm -f ' + ' '.join(paths)], check=True, capture_output=True)


if __name__ == '__main__':
    main()
