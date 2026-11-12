"""E1 TLS/WebSocket/ALSA integration test with synthetic Opus replies.

Requires macOS libopus (Homebrew), openssl, USB ADB, and a deployed app.
Keeps official credentials on the E1; restores the private profile in finally.
This test briefly takes over voice mode and plays a quiet 1-second test tone.
"""
import base64
import ctypes
import hashlib
import http.server
import json
import math
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'build/voice-test'
SERIAL = '15dacc90f0c2e787'
PROFILE = '/userdata/e1-recorder/xiaozhi.json'
BACKUP = PROFILE + '.test-backup'
PORT = 18771
HOST = subprocess.check_output(['ipconfig', 'getifaddr', 'en0']).decode().strip()
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def adb(*args):
    return subprocess.check_output(['adb', '-s', SERIAL, *args], timeout=30).decode().replace('\r', '')

def api(path='assistant', post=False):
    req = urllib.request.Request('http://127.0.0.1:18770/api/' + path, data=b'' if post else None)
    with opener.open(req, timeout=4) as response:
        return json.load(response)

def wait_for(predicate, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = api()
        if predicate(value):
            return value
        time.sleep(.1)
    raise AssertionError('Voice state timeout: ' + str(value))

def opus_tone(rate):
    lib = ctypes.CDLL('/opt/homebrew/lib/libopus.dylib')
    lib.opus_encoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_encoder_create.restype = ctypes.c_void_p
    lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
    lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
    error = ctypes.c_int()
    enc = lib.opus_encoder_create(rate, 1, 2048, ctypes.byref(error))
    assert enc and not error.value
    packets = []
    for frame in range(16):
        n = rate * 60 // 1000
        pcm = (ctypes.c_int16 * n)(*(int(1200 * math.sin(2 * math.pi * 440 * (frame*n+i)/rate)) for i in range(n)))
        out = ctypes.create_string_buffer(4096)
        size = lib.opus_encode(enc, pcm, n, out, len(out))
        assert size > 0
        packets.append(out.raw[:size])
    lib.opus_encoder_destroy(enc)
    return packets

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args): pass
    def exact(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.connection.recv(n-len(buf))
            if not chunk: raise EOFError()
            buf += chunk
        return buf
    def recv(self):
        h = self.exact(2); opcode = h[0] & 15; n = h[1] & 127
        if n == 126: n = struct.unpack('!H', self.exact(2))[0]
        elif n == 127: n = struct.unpack('!Q', self.exact(8))[0]
        assert n < 65536 and h[1] & 128, 'Unmasked/oversize client frame'
        mask = self.exact(4); data = self.exact(n)
        return opcode, bytes(c ^ mask[i % 4] for i, c in enumerate(data))
    def send(self, opcode, data, final=True):
        if isinstance(data, dict): data = json.dumps(data, ensure_ascii=False).encode()
        header = bytes([(128 if final else 0) | opcode])
        header += bytes([len(data)]) if len(data) < 126 else b'\x7e' + struct.pack('!H', len(data))
        self.connection.sendall(header + data)
    def do_GET(self):
        try:
            assert self.headers['Authorization'] == 'Bearer local-test-token'
            assert self.headers['Protocol-Version'] == '1'
            assert self.headers['Device-Id'] and self.headers['Client-Id']
            key = self.headers['Sec-WebSocket-Key']
            accept = base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
            self.send_response(101); self.send_header('Upgrade', 'websocket'); self.send_header('Connection', 'Upgrade'); self.send_header('Sec-WebSocket-Accept', accept); self.end_headers()
            self.connection.settimeout(15)
            opcode, data = self.recv(); hello = json.loads(data)
            assert opcode == 1 and hello['audio_params']['sample_rate'] == 16000 and hello['audio_params']['frame_duration'] == 60
            # Fragmentation with an interleaved ping exercises frame reassembly.
            hello = json.dumps({'type':'hello','transport':'websocket','session_id':'e1-test','audio_params':{'format':'opus','sample_rate':self.server.rate,'channels':1,'frame_duration':60}}).encode()
            self.send(1, hello[:35], False); self.send(9, b'ping'); self.send(0, hello[35:])
            packets = 0
            while True:
                opcode, data = self.recv()
                if opcode == 2: packets += 1; continue
                if opcode != 1: continue
                msg = json.loads(data)
                if msg.get('type') == 'listen' and msg.get('state') == 'stop': break
            assert packets >= 6, packets
            # MCP remains useful before TTS; only allow-listed tools are exposed.
            for rid, method, params in [(1,'initialize',{}),(2,'tools/list',{}),(3,'tools/call',{'name':'self.audio_speaker.set_volume','arguments':{'volume':40}})]:
                self.send(1, {'type':'mcp','session_id':'e1-test','payload':{'jsonrpc':'2.0','id':rid,'method':method,'params':params}})
                while True:
                    opcode,data = self.recv()
                    if opcode == 1:
                        reply = json.loads(data)
                        if reply.get('type') == 'mcp' and reply['payload'].get('id') == rid:
                            assert 'result' in reply['payload']; break
            self.send(1, {'type':'stt','session_id':'e1-test','text':'Local audio test 你好'})
            self.send(1, {'type':'tts','state':'start','session_id':'e1-test'})
            self.send(1, {'type':'tts','state':'sentence_start','text':'Synthetic Opus playback test'})
            for packet in self.server.packets: self.send(2, packet)
            self.send(1, {'type':'tts','state':'stop'})
            time.sleep(3)
            self.server.result = packets
        except Exception as error:
            self.server.error = repr(error)
        finally:
            self.close_connection = True

def main():
    WORK.mkdir(parents=True, exist_ok=True)
    conf = WORK / 'openssl.cnf'
    conf.write_text(f'[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=E1 test\n[ext]\nsubjectAltName=IP:{HOST}\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,digitalSignature,keyEncipherment\n')
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','2','-config',str(conf),'-keyout',str(WORK/'key.pem'),'-out',str(WORK/'cert.pem')],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    api('assistant/cancel', True);wait_for(lambda s:not s['active'])
    assert api('status')['state'] == 'idle'
    original_volume = api()['volume']
    server = http.server.ThreadingHTTPServer(('0.0.0.0', PORT), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(WORK/'cert.pem',WORK/'key.pem')
    server.socket=context.wrap_socket(server.socket,server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    backed_up = False
    try:
        # Keep credentials on-device and preserve any interrupted test backup.
        assert 'E1_PROFILE_BACKED_UP' in adb('shell',f'test ! -e {BACKUP} && cp {PROFILE} {BACKUP} && echo E1_PROFILE_BACKED_UP')
        backed_up = True
        adb('push',str(WORK/'cert.pem'),'/tmp/e1-xz-test-ca.pem')
        for rate in [16000,24000]:
            server.rate=rate;server.packets=opus_tone(rate);server.error=None;server.result=None
            profile={'ota_url':f'https://{HOST}:{PORT}/ota/','websocket_url':f'wss://{HOST}:{PORT}/voice','token':'local-test-token','protocol_version':1,'ca_bundle':'/tmp/e1-xz-test-ca.pem'}
            (WORK/'profile.json').write_text(json.dumps(profile));adb('push',str(WORK/'profile.json'),PROFILE)
            assert api('assistant/talk',True)['ok']
            wait_for(lambda s:s['state']=='listening' and s['sentPackets']>=10)
            assert api('assistant/finish',True)['ok']
            result=wait_for(lambda s:not s['active'])
            assert result['state']=='idle', result
            assert result['receivedPackets']==16 and result['playedPackets']==16, result
            assert result['userText']=='Local audio test 你好' and result['volume']==40, result
            assert server.error is None, server.error
            print('VOICE_INTEGRATION_PASS',rate,'Hz: TLS fragmented-hello ping mic Opus MCP transcript playback drain')
            time.sleep(.3)
    finally:
        try:
            if backed_up:
                api('assistant/cancel',True);wait_for(lambda s:not s['active'])
                print(adb('shell',f'mv {BACKUP} {PROFILE}; chmod 600 {PROFILE}; rm -f /tmp/e1-xz-test-ca.pem').strip())
                current_volume = api()['volume']
                action = 'volume-up' if current_volume < original_volume else 'volume-down'
                for _ in range(abs(current_volume-original_volume)//5): api('assistant/'+action,True)
        finally:
            server.shutdown();server.server_close()
    wait_for(lambda s:not s['active'])
    api('assistant/activate',True);wait_for(lambda s:not s['active'])
    print('OFFICIAL_PROFILE_RESTORED')

if __name__ == '__main__': main()
