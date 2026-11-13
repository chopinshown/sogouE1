#!/usr/bin/env python3
"""Verify a generated 44.1 kHz file plays through a 48 kHz physical output."""
import json
import re
import subprocess
import time
import wave

from rtp_integration import ROOT, api, until


def main():
    assert api('status')['state'] == 'idle', 'Stop the current audio job first'
    output = ROOT / 'evidence' / ('legacy48-' + time.strftime('%Y%m%d-%H%M%S'))
    output.mkdir(parents=True)
    name = 'TEST-LEGACY-' + time.strftime('%Y%m%d-%H%M%S') + '.wav'
    local = output / name
    with wave.open(str(local), 'wb') as file:
        file.setparams((2, 3, 44100, 0, 'NONE', 'not compressed'))
        file.writeframes(bytes(44100 * 6 * 3))
    remote = '/mnt/sdcard/Recordings/' + name
    subprocess.run(['adb', '-s', '15dacc90f0c2e787', 'push', str(local), remote],
                   check=True, capture_output=True)
    try:
        listed = next(f for f in api('files')['files'] if f['name'] == name)
        assert listed['sampleRate'] == 44100 and listed['duration'] == 3
        api('play?file=' + name, True)
        state = until(lambda: (lambda s: s if s['state'] == 'playing' else None)(api('status')))
        assert state['playbackSampleRate'] == 48000 and state['playbackSourceRate'] == 44100
        parameters = subprocess.run(['adb', '-s', '15dacc90f0c2e787', 'shell',
                                     'cat /proc/asound/card0/pcm0p/sub0/hw_params'],
                                    check=True, capture_output=True, text=True).stdout
        assert re.search(r'rate:\s+48000\b', parameters), parameters
        api('seek?position=1', True)
        time.sleep(.15)
        position = api('status')['position']
        assert .9 <= position < 2, position
        api('stop', True)
        until(lambda: api('status')['captureReady'])
        (output / 'result.json').write_text(json.dumps(dict(state=state, hardware=parameters,
                                                            seekPosition=position), indent=2) + '\n')
        print('LEGACY_PLAYBACK_PASS 44100 Hz file → 48000 Hz physical output; duration and seek preserved')
        print('EVIDENCE', output)
    finally:
        api('stop', True)
        until(lambda: api('status')['captureReady'])
        subprocess.run(['adb', '-s', '15dacc90f0c2e787', 'shell', 'rm -f ' + remote],
                       check=True, capture_output=True)


if __name__ == '__main__':
    main()
