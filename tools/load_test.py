#!/usr/bin/env python3
"""
load_test.py — Phase 10.1 concurrent-game load tester.

Spawns N WebSocket clients against ws://localhost:9000, pairs them via
quick_play, and has each pair replay a fixed 20-half-move scripted game
so every client sends legal moves without embedding a chess engine on the
client side. Records per-move round-trip time (client send → move_made
receipt on the sender's socket) and aggregates p50/p95/p99.

Auth:
    Sealed-envelope registration cannot be replayed from Python without
    reimplementing ML-KEM + X25519 + HKDF + AES-CTR + HMAC-SHA-384. The
    load test cares about the gameplay hot path, not the one-shot login
    handshake, so accounts and access tokens are pre-provisioned by the
    C++ helper `bench_provision` (which reads the SAME env vars the server
    reads, so the tokens verify identically).

Typical run (from the repo root, inside WSL):

    export DATABASE_URL=postgres://…                # same URL server uses
    export JWT_SIGNING_KEY=…                        # same key server uses
    ./build_release/bench_provision 500 > tokens.json
    ./build_release/chess_server &                  # in another shell
    python3 tools/load_test.py --tokens tokens.json --games 250

The script emits a compact summary block that BENCHMARKS.md consumes verbatim.
"""

import argparse
import asyncio
import json
import statistics
import time
from pathlib import Path

import websockets


# ── Scripted game — twenty half-moves of the Italian Opening ──────────────────
#
# Chosen for a boring reason: every one of these plies is guaranteed legal
# from the starting position, with no captures, no checks, no castling, no
# en-passant, and no promotion, so the server never rejects a move and the
# load-test client never has to compute anything about board state. UCI
# encoding: "<from-square><to-square>".

SCRIPTED_MOVES = [
    "e2e4", "e7e5",   # 1
    "g1f3", "b8c6",   # 2
    "f1c4", "f8c5",   # 3
    "d2d3", "d7d6",   # 4
    "b1c3", "g8f6",   # 5
    "h2h3", "h7h6",   # 6
    "a2a3", "a7a6",   # 7
    "b2b3", "b7b6",   # 8
    "c1b2", "c8b7",   # 9
    "d1d2", "d8d7",   # 10
]


class ClientMetrics:
    """Per-client counters collected in memory; merged after the run."""
    __slots__ = ("connect_ms", "match_ms", "latencies_ms",
                 "moves_played", "completed", "error")

    def __init__(self):
        self.connect_ms   = None    # ws.connect() latency
        self.match_ms     = None    # send(quick_play) → first match_found
        self.latencies_ms = []      # one entry per make_move → move_made
        self.moves_played = 0
        self.completed    = False
        self.error        = None


async def run_client(idx, token_row, server_url, metrics):
    """
    One WebSocket session: connect, queue, play the scripted plies for the
    colour the server hands us, then resign to close the room cleanly.
    """
    try:
        t0 = time.perf_counter()
        async with websockets.connect(server_url, max_size=2**20) as ws:
            metrics.connect_ms = (time.perf_counter() - t0) * 1000.0

            # ── quick_play → wait for match_found ────────────────────────
            t_queue = time.perf_counter()
            await ws.send(json.dumps({
                "type":         "quick_play",
                "access_token": token_row["access_token"],
                "time_base":    600,
                "time_inc":     5,
            }))

            my_color = None
            while my_color is None:
                raw = await asyncio.wait_for(ws.recv(), timeout=60.0)
                msg = json.loads(raw)
                mtype = msg.get("type", "")
                if mtype == "match_found":
                    metrics.match_ms = (time.perf_counter() - t_queue) * 1000.0
                    my_color = msg.get("color", "")
                elif mtype in ("queued", "queue_cancelled"):
                    continue
                elif mtype == "error":
                    metrics.error = f"server error before match: {msg}"
                    return
                else:
                    # Ignore stray broadcast frames (unlikely at this stage).
                    continue

            # ── replay the scripted game ─────────────────────────────────
            for turn_idx, uci in enumerate(SCRIPTED_MOVES):
                current_color = "white" if turn_idx % 2 == 0 else "black"
                if current_color == my_color:
                    from_sq, to_sq = uci[:2], uci[2:]
                    t_send = time.perf_counter()
                    await ws.send(json.dumps({
                        "type": "make_move",
                        "from": from_sq,
                        "to":   to_sq,
                    }))
                    while True:
                        raw = await asyncio.wait_for(ws.recv(), timeout=30.0)
                        msg = json.loads(raw)
                        t = msg.get("type", "")
                        if t == "move_made":
                            rtt_ms = (time.perf_counter() - t_send) * 1000.0
                            metrics.latencies_ms.append(rtt_ms)
                            metrics.moves_played += 1
                            break
                        if t in ("error", "game_over"):
                            metrics.error = f"unexpected {t}: {msg}"
                            return
                        # queued/broadcast noise — keep waiting.
                else:
                    while True:
                        raw = await asyncio.wait_for(ws.recv(), timeout=30.0)
                        msg = json.loads(raw)
                        t = msg.get("type", "")
                        if t == "move_made":
                            break
                        if t in ("error", "game_over"):
                            metrics.error = f"unexpected {t}: {msg}"
                            return

            # ── clean end ────────────────────────────────────────────────
            await ws.send(json.dumps({"type": "resign"}))
            # Drain until game_over or socket close, best-effort.
            try:
                while True:
                    raw = await asyncio.wait_for(ws.recv(), timeout=2.0)
                    msg = json.loads(raw)
                    if msg.get("type") == "game_over":
                        break
            except (asyncio.TimeoutError, websockets.ConnectionClosed):
                pass
            metrics.completed = True
    except asyncio.TimeoutError:
        metrics.error = "timeout"
    except Exception as e:                                        # noqa: BLE001
        metrics.error = f"{type(e).__name__}: {e}"


def percentile(values, p):
    if not values:
        return float("nan")
    xs = sorted(values)
    k = (len(xs) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tokens", required=True,
                    help="JSON file emitted by bench_provision")
    ap.add_argument("--games",  type=int, default=50,
                    help="Number of concurrent games to run (2 clients each)")
    ap.add_argument("--server", default="ws://localhost:9000")
    ap.add_argument("--stagger-ms", type=int, default=5,
                    help="Delay between successive client launches, ms")
    args = ap.parse_args()

    tokens = json.loads(Path(args.tokens).read_text())
    n_clients = args.games * 2
    if len(tokens) < n_clients:
        raise SystemExit(
            f"need {n_clients} tokens but tokens.json has only {len(tokens)}")

    metrics = [ClientMetrics() for _ in range(n_clients)]
    tasks = []
    wall_start = time.perf_counter()

    for i in range(n_clients):
        tasks.append(asyncio.create_task(
            run_client(i, tokens[i], args.server, metrics[i])))
        if args.stagger_ms > 0:
            await asyncio.sleep(args.stagger_ms / 1000.0)

    await asyncio.gather(*tasks, return_exceptions=True)
    wall_ms = (time.perf_counter() - wall_start) * 1000.0

    # ── aggregate ──────────────────────────────────────────────────────
    all_latencies = [x for m in metrics for x in m.latencies_ms]
    completed     = sum(1 for m in metrics if m.completed)
    errored       = sum(1 for m in metrics if m.error)
    total_moves   = sum(m.moves_played for m in metrics)
    connect_times = [m.connect_ms for m in metrics if m.connect_ms is not None]
    match_times   = [m.match_ms   for m in metrics if m.match_ms   is not None]

    def line(k, v):
        print(f"  {k:.<30s} {v}")

    print("========================================")
    print(f" Phase 10.1 load test — {args.games} games / {n_clients} clients")
    print("========================================")
    line("wall clock (s)",       f"{wall_ms/1000.0:.2f}")
    line("clients completed",    f"{completed}/{n_clients}")
    line("clients errored",      errored)
    line("total moves observed", total_moves)
    if wall_ms > 0:
        line("moves / sec",       f"{total_moves*1000.0/wall_ms:.1f}")
    if connect_times:
        line("connect p50 / p99 (ms)",
             f"{percentile(connect_times,50):.2f} / "
             f"{percentile(connect_times,99):.2f}")
    if match_times:
        line("match p50 / p99 (ms)",
             f"{percentile(match_times,50):.2f} / "
             f"{percentile(match_times,99):.2f}")
    if all_latencies:
        line("move RTT p50 (ms)", f"{percentile(all_latencies,50):.2f}")
        line("move RTT p95 (ms)", f"{percentile(all_latencies,95):.2f}")
        line("move RTT p99 (ms)", f"{percentile(all_latencies,99):.2f}")
        line("move RTT max (ms)", f"{max(all_latencies):.2f}")
        line("move RTT mean(ms)", f"{statistics.mean(all_latencies):.2f}")

    if errored:
        print("\n  --- error samples ---")
        shown = 0
        for m in metrics:
            if m.error and shown < 5:
                print(f"    {m.error}")
                shown += 1

    print()


if __name__ == "__main__":
    asyncio.run(main())
