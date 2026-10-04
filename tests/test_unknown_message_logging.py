"""No-secret logging regression against an isolated auth-disabled server.

Never sources .env, connects to a database, or signals the user's backend.
"""
import json
import os
import signal
import socket
import struct
import subprocess
import tempfile
import time

env = os.environ.copy()
for name in ('DATABASE_URL', 'JWT_SIGNING_KEY', 'SERVER_IDENTITY_KEY_PATH'):
    env.pop(name, None)
env['AUTH_SEAL_REQUIRED'] = '0'
env['SERVER_WORKER_THREADS'] = '2'
with socket.socket() as reservation:
    reservation.bind(('127.0.0.1', 0))
    port = reservation.getsockname()[1]
env['PORT'] = str(port)
marker = 'SYNTHETIC_CREDENTIAL_MUST_NEVER_BE_LOGGED'
with tempfile.TemporaryFile() as log:
    server = subprocess.Popen(['./build_release/chess_server'], env=env,
                              stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                client = socket.create_connection(('127.0.0.1', port), timeout=2)
                break
            except OSError:
                assert server.poll() is None and time.monotonic() < deadline
                time.sleep(.02)
        with client:
            client.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
                           b'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                           b'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n')
            response = b''
            while b'\r\n\r\n' not in response:
                response += client.recv(4096)
            assert b'101 Switching Protocols' in response
            for message in ({'type': 'google_auth', 'supabase_jwt': marker},
                            {'type': marker, 'password': marker}):
                payload = json.dumps(message).encode()
                header = bytes([0x81, 0x80 | len(payload)]) if len(payload) < 126 else b'\x81\xfe' + struct.pack('!H', len(payload))
                client.sendall(header + b'\0\0\0\0' + payload)
                assert client.recv(4096), 'unknown route must still respond'
        server.send_signal(signal.SIGTERM)
        server.wait(timeout=10)
        log.seek(0)
        output = log.read()
        assert marker.encode() not in output, 'credential leaked by fallback logging'
        assert b'Unrecognized message' in output
        assert server.returncode == 0
    finally:
        if server.poll() is None:
            server.kill()  # Own isolated child only.
            server.wait(timeout=5)
print('PASS auth-disabled unknown route logging contains no credentials or attacker-controlled type')
