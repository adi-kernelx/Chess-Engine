"""Real sealed authentication against own server/local disposable DB only.

Never sources .env, uses ephemeral identity/JWT keys, deletes only the uniquely
named test account and sessions, and never signals the user's running backend.
"""
import base64
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time
import uuid
import hashlib
import hmac
import json

root = Path(__file__).resolve().parents[1]
env = os.environ.copy()
env['DATABASE_URL'] = 'postgresql://localhost/postgres_test?host=/var/run/postgresql&user=adi'
env['JWT_SIGNING_KEY'] = base64.b64encode(os.urandom(32)).decode()
env['PGCONNECT_TIMEOUT'] = '5'
env['SERVER_WORKER_THREADS'] = '2'
env['AUTH_READ_POOL_SIZE'] = '2'
env['SEALED_TEST_USERNAME'] = 'seal_' + uuid.uuid4().hex[:12]
for name in ('SUPABASE_JWT_SECRET', 'SUPABASE_ISSUER', 'SUPABASE_AUDIENCE'):
    env.pop(name, None)
google_secret = os.urandom(32)
env['SUPABASE_JWT_SECRET'] = base64.b64encode(google_secret).decode()
env['SUPABASE_ISSUER'] = 'https://sealed-test.invalid/auth/v1'
env['SUPABASE_AUDIENCE'] = 'authenticated'
env['SUPABASE_PROVIDER'] = 'google'
env['SUPABASE_JWT_ALGORITHM'] = 'HS256'  # Synthetic legacy transport fixture.
env['AUTH_SEAL_REQUIRED'] = '1'
google_sub = str(uuid.uuid4())
encode = lambda value: base64.urlsafe_b64encode(value).rstrip(b'=').decode()
header = encode(b'{"alg":"HS256","typ":"JWT"}')
claims = encode(json.dumps({'iss': env['SUPABASE_ISSUER'], 'aud': 'authenticated',
    'sub': google_sub, 'email': env['SEALED_TEST_USERNAME']+'@example.invalid',
    'email_verified': True, 'iat': int(time.time()), 'exp': int(time.time())+300,
    'app_metadata': {'provider': 'google'}}).encode())
signature = encode(hmac.new(google_secret, (header+'.'+claims).encode(), hashlib.sha256).digest())
env['SEALED_TEST_GOOGLE_JWT'] = header+'.'+claims+'.'+signature
with tempfile.TemporaryDirectory(prefix='chess-sealed-auth-') as temporary:
    key = str(Path(temporary) / 'identity.key')
    generated = subprocess.run([str(root/'build_release/gen_server_identity'), key],
                               capture_output=True, text=True, check=True)
    pin = re.search(r'base64\s*:\s*(\S+)', generated.stdout).group(1)
    if env.get('SEALED_TEST_IDENTITY_TEXT') == '1':
        text_key = str(Path(temporary) / 'identity.txt')
        subprocess.run(['node', 'tools/convert_server_identity.mjs', key, text_key],
                       cwd=root, capture_output=True, check=True, timeout=10)
        key = text_key
    env['SERVER_IDENTITY_KEY_PATH'] = key
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    env['PORT'] = str(port)
    with tempfile.TemporaryFile() as log:
        server = subprocess.Popen([str(root/'build_release/chess_server')], env=env,
                                  stdout=log, stderr=subprocess.STDOUT, cwd=root)
        try:
            deadline = time.monotonic()+10
            while True:
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1):
                        break
                except OSError:
                    assert server.poll() is None and time.monotonic() < deadline
                    time.sleep(.02)
            subprocess.run(['node', 'tests/test_sealed_auth_network.mjs',
                            f'ws://127.0.0.1:{port}', pin], cwd=root, env=env,
                           check=True, timeout=40)
        finally:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)
            username = env['SEALED_TEST_USERNAME']
            assert re.fullmatch(r'seal_[0-9a-f]{12}', username)
            subprocess.run(['psql', env['DATABASE_URL'], '-Xq', '-v', 'ON_ERROR_STOP=1', '-c',
                f"BEGIN; DELETE FROM sessions WHERE player_id IN (SELECT id FROM players WHERE username='{username}' OR google_sub='{google_sub}'); "
                f"DELETE FROM players WHERE username='{username}' OR google_sub='{google_sub}'; COMMIT;"],
                env=env, capture_output=True, check=True, timeout=10)
        assert server.returncode == 0
print('PASS isolated sealed-required server startup and graceful shutdown')
