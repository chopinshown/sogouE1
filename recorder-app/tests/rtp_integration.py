#!/usr/bin/env python3
"""Real E1 integration test. No credentials, existing recordings, or Wi-Fi profiles are read."""
import argparse
import json
import pathlib
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def api(path, post=False, expected=200):
    request = urllib.request.Request('http://127.0.0.1:18770/api/' + path,
                                     data=b'' if post else None)
    try:
        response = OPENER.open(request, timeout=4)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        result = json.load(response)
        if response.code != expected:
            raise AssertionError((path, response.code, result))
        return result


def until(predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.1)
    raise AssertionError('Device state timed out')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='192.168.0.130')
    parser.add_argument('--seconds', type=int, default=20)
    args = parser.parse_args()
    output = ROOT / 'evidence' / ('rtp-integration-' + time.strftime('%Y%m%d-%H%M%S'))
    output.mkdir(parents=True)
    subprocess.run(['adb', '-s', '15dacc90f0c2e787', 'forward', 'tcp:18770', 'tcp:18770'], check=True, capture_output=True)
    initial = api('status')
    assert initial['state'] in ('idle', 'network'), 'Finish recording/playback first'
    assert not api('network')['active'], 'Close the existing computer bridge first'
    if initial['state'] == 'idle':
        api('network/start', True)
    until(lambda: api('network')['state'] == 'waiting')
    assert not api('status')['captureReady']
    api('record', True, expected=409)
    with socket.create_connection((args.host, 18772), timeout=10) as client:
        client.sendall(b'E1RTP/1 48000 24 2 110 50000 50001 42 120 1\n')
        assert client.recv(256).startswith(b'ERROR unsupported session')
    command = [sys.executable, str(ROOT / 'host/e1_audio.py'), args.host,
               '--duration', str(args.seconds), '--buffer-ms', '200',
               '--drop-send-every', '17', '--drop-receive-every', '19',
               '--stats', str(output / 'host.json')]
    native = []
    process = None
    try:
        with (output / 'bridge.log').open('w') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            while process.poll() is None:
                native.append(api('network'))
                time.sleep(.25)
            assert process.returncode == 0, (output / 'bridge.log').read_text()
        host = json.loads((output / 'host.json').read_text())
        final = api('network')
        (output / 'device.json').write_text(json.dumps(final, indent=2) + '\n')
        (output / 'device-history.json').write_text(json.dumps(native, indent=2) + '\n')
        assert host['sentPackets'] > args.seconds * 300
        assert host['receivedPackets'] > args.seconds * 200
        assert host['recoveredPackets'] > 20
        assert final['sentPackets'] > args.seconds * 300
        assert final['playedFrames'] > 44100 * args.seconds * .4
        assert final['recoveredPackets'] > 20
        assert final['invalidPackets'] == 0 and host['invalidPackets'] == 0
        assert final['rtcpPackets'] >= 1 and host['rtcpPackets'] >= 1
        assert final['xruns'] == 0
        until(lambda: not api('network')['active'])
        # The same armed listener accepts another session, including plain L24.
        plain = subprocess.run([sys.executable, str(ROOT / 'host/e1_audio.py'), args.host,
                                '--duration', '3', '--buffer-ms', '120', '--no-red'],
                               capture_output=True, text=True, timeout=30)
        (output / 'plain.log').write_text(plain.stdout + plain.stderr)
        assert plain.returncode == 0, plain.stderr
        assert api('network')['playedFrames'] > 44100
        print('RTP_INTEGRATION_PASS duplex L24/RED, injected loss recovery, RTCP, exclusive audio ownership, reconnect')
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        api('network/stop', True)
        until(lambda: api('status')['captureReady'])
    before = {entry['name'] for entry in api('files')['files']}
    api('record', True)
    until(lambda: api('status')['state'] == 'recording')
    time.sleep(1)
    api('mark', True)
    time.sleep(.5)
    api('stop', True)
    saved = until(lambda: (lambda s: s if s['state'] == 'idle' and s['captureReady'] else None)(api('status')))
    name = saved['file']
    assert name not in before and re.fullmatch(r'REC-[0-9-]+\.wav', name)
    assert len(saved['marks']) == 1 and saved['xruns'] == 0 and not saved['error']
    waveform = api('wave?file=' + name)
    assert waveform['sampleRate'] == 44100 and waveform['channels'] == 2 and waveform['bits'] == 24
    api('play?file=' + name, True)
    until(lambda: api('status')['state'] == 'playing')
    api('stop', True)
    until(lambda: api('status')['captureReady'])
    (output / 'recorder.json').write_text(json.dumps(saved, indent=2) + '\n')
    print('RECORDER_REGRESSION_PASS record, mark, stop, waveform, playback, microphone reacquisition')
    print('EVIDENCE', output)


if __name__ == '__main__':
    main()
