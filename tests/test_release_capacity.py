"""Isolated local gameplay/polling probe, not a production capacity guarantee.

Requires a freshly prepared local postgres_test database; never sources .env.
Generates disposable tokens in memory. Signals only its own server, exercising
DB-enabled shutdown after gameplay; persistence is checked after worker drain.
"""
import asyncio
import base64
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import websockets
from tools.load_test import ClientMetrics, run_client, percentile


async def poller(url, tid, samples, seconds):
    async with websockets.connect(url) as ws:
        end = time.monotonic() + seconds
        seq = 0
        while time.monotonic() < end:
            seq += 1
            request_id = f"poll-{seq}"
            began = time.perf_counter()
            await ws.send(json.dumps(dict(type="tournament_state", tournament_id=tid,
                                          request_id=request_id)))
            while True:
                msg = json.loads(await asyncio.wait_for(ws.recv(), 10))
                if msg.get("request_id") == request_id:
                    assert msg["type"] == "tournament_state", msg.get("type")
                    samples.append((time.perf_counter() - began) * 1000)
                    break
            await asyncio.sleep(0.5)  # More aggressive than normal UI polling.


async def probe(url, tokens, count, tid):
    metrics = [ClientMetrics() for _ in range(count)]
    samples = []
    # One polling socket per account in addition to its active game socket.
    readers = [asyncio.create_task(poller(url, tid, samples, 8)) for _ in range(count)]
    tasks = []
    for i, metric in enumerate(metrics):
        tasks.append(asyncio.create_task(run_client(i, tokens[i], url, metric)))
        await asyncio.sleep(0.01)
    await asyncio.gather(*tasks)
    await asyncio.gather(*readers)
    errors = [m.error for m in metrics if m.error]
    moves = [v for m in metrics for v in m.latencies_ms]
    completed = sum(m.completed for m in metrics)
    print(f"users={count} sockets={count * 2} completed={completed}/{count} "
          f"errors={len(errors)} moves={len(moves)} "
          f"move_p95_ms={percentile(moves,95):.2f} move_p99_ms={percentile(moves,99):.2f} "
          f"poll_p95_ms={percentile(samples,95):.2f} poll_p99_ms={percentile(samples,99):.2f}",
          flush=True)
    assert not errors and completed == count and len(moves) == count * 10, errors[:3]


def main():
    os.chdir(ROOT)
    env = os.environ.copy()
    env["DATABASE_URL"] = "postgresql://localhost/postgres_test?host=/var/run/postgresql&user=adi"
    env["JWT_SIGNING_KEY"] = base64.b64encode(os.urandom(32)).decode()
    env["PGOPTIONS"] = "-c statement_timeout=5000 -c lock_timeout=3000"
    env["PGCONNECT_TIMEOUT"] = "5"
    for name in ("SERVER_IDENTITY_KEY_PATH", "SUPABASE_JWT_SECRET", "SUPABASE_ISSUER"):
        env.pop(name, None)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    env["PORT"] = str(port)
    provision = subprocess.run(["./build_release/bench_provision", "60"], env=env,
                               capture_output=True, check=True)
    tokens = json.loads(provision.stdout)
    baseline = subprocess.run(
        ["psql", env["DATABASE_URL"], "-XAtq", "-c",
         "SELECT count(*) FROM games WHERE completion_uuid IS NOT NULL"],
        env=env, capture_output=True, check=True)
    saved_before = int(baseline.stdout.decode().strip())
    # A scheduled event with 60 real disposable participants exercises snapshots.
    sql = """
    INSERT INTO tournaments(name,format,rounds,status,created_by,
      time_control_initial_ms,time_control_increment_ms,registration_deadline,
      first_round_starts_at,round_duration_seconds)
    SELECT 'Capacity ' || extract(epoch FROM clock_timestamp()),'swiss',1,'registration',
      min(id),600000,0,now()+interval '1 hour',now()+interval '65 minutes',60
    FROM players WHERE username LIKE 'loadtest_%' RETURNING id;
    """
    result = subprocess.run(["psql", env["DATABASE_URL"], "-XAtq", "-v", "ON_ERROR_STOP=1",
                             "-c", sql], env=env, capture_output=True, check=True)
    tid = int(result.stdout.decode().strip())
    subprocess.run(["psql", env["DATABASE_URL"], "-Xq", "-v", "ON_ERROR_STOP=1", "-c",
                    f"INSERT INTO tournament_players(tournament_id,player_id,initial_elo) "
                    f"SELECT {tid},id,elo_rating FROM players WHERE username LIKE 'loadtest_%'"],
                   env=env, capture_output=True, check=True)
    with tempfile.TemporaryFile() as log:
        command = ["./build_release/chess_server"]
        cpu_limit = int(env.get("CAPACITY_CPU_LIMIT", "0"))
        if cpu_limit:
            cpus = sorted(os.sched_getaffinity(0))[:cpu_limit]
            command = ["taskset", "-c", ",".join(map(str, cpus)), *command]
        print(f"server_cpu_affinity={cpu_limit or 'unrestricted'}; local PostgreSQL; "
              "no AI or password-login load", flush=True)
        server = subprocess.Popen(command, env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except OSError:
                    assert server.poll() is None and time.monotonic() < deadline, "startup failed"
                    time.sleep(0.05)
            for count in (10, 20, 40, 60):
                asyncio.run(probe(f"ws://127.0.0.1:{port}", tokens, count, tid))
            status = Path(f"/proc/{server.pid}/status").read_text()
            for line in status.splitlines():
                if line.startswith("VmHWM:"):
                    print(f"server_peak_resident_memory: {line.split(':',1)[1].strip()}", flush=True)
            # Exercise shutdown with a request blocked on a local DB table lock.
            locker = subprocess.Popen(
                ["psql", env["DATABASE_URL"], "-XAtq", "-v", "ON_ERROR_STOP=1", "-c",
                 "BEGIN; LOCK TABLE tournaments IN ACCESS EXCLUSIVE MODE; "
                 "SELECT pg_sleep(2); COMMIT;"], env=env,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                for _ in range(50):
                    locked = subprocess.run(
                        ["psql", env["DATABASE_URL"], "-XAtq", "-c",
                         "SELECT count(*) FROM pg_locks WHERE "
                         "relation='tournaments'::regclass AND "
                         "mode='AccessExclusiveLock' AND granted"],
                        env=env, capture_output=True, check=True)
                    if int(locked.stdout.decode().strip()) > 0:
                        break
                    time.sleep(0.02)
                else:
                    raise AssertionError("local test lock not acquired")

                async def drain_blocked_request():
                    async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
                        # Directory reads are not served by the state cache.
                        await ws.send(json.dumps(dict(type="list_tournaments")))
                        await asyncio.sleep(0.1)
                        began = time.monotonic()
                        server.send_signal(signal.SIGTERM)
                        msg = json.loads(await asyncio.wait_for(ws.recv(), 8))
                        assert msg["type"] == "tournament_list", msg.get("type")
                        assert server.wait(timeout=10) == 0
                        print(f"PASS DB-blocked request drained on shutdown "
                              f"{time.monotonic()-began:.3f}s", flush=True)
                asyncio.run(drain_blocked_request())
                assert locker.wait(timeout=5) == 0
            finally:
                if locker.poll() is None:
                    locker.kill()  # Own disposable-test lock holder only.
                    locker.wait(timeout=5)

            began = time.monotonic()
            log.seek(0)
            output = log.read()
            assert b"All workers shut down" in output and b"Server exited gracefully" in output
            result = subprocess.run(["psql", env["DATABASE_URL"], "-XAtq", "-c",
                                     "SELECT count(*) FROM games WHERE completion_uuid IS NOT NULL"],
                                    env=env, capture_output=True, check=True)
            persisted = int(result.stdout.decode().strip()) - saved_before
            assert persisted == 65, f"expected 65 completed games, got {persisted}"
            print(f"PASS DB-enabled shutdown {time.monotonic()-began:.3f}s; "
                  f"saved_games={persisted}", flush=True)
        finally:
            if server.poll() is None:
                server.kill()  # Own isolated child only, on failed probe.
                server.wait(timeout=5)


if __name__ == "__main__":
    main()
