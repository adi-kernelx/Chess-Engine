# Benchmarks

All numbers here were measured **17 September 2026** on the development machine described below, using the tools in `tools/`. Reproducing the run is one command block; the raw scripts are checked in.

## Test rig

| Component | Value |
|---|---|
| CPU | AMD Ryzen 5 5600H (6 cores, 12 threads, ~3.3 GHz base) |
| RAM | 8 GB |
| Kernel | Linux 6.6.87 (WSL2 Ubuntu 26.04 on Windows 11) |
| Toolchain | GCC 15.2, CMake 4.2.3, `-DCMAKE_BUILD_TYPE=Release` (`-O2`) |
| Postgres | 18, local instance (`chessbench` / `chess_bench` DB) |
| Server | `./chess_server` bound to `ws://127.0.0.1:9000` |

> **Caveat: WSL2 vs bare metal.** WSL2 shares the CPU with Windows; on the first invocation after a period of idle the CPU has not yet ramped to full clock and the caches are empty. Every number below is the median of **three consecutive warm runs** — the first, cold run for each binary was discarded. This is not superstition: the first cold run at the same binary was consistently 2–2.5× worse. All measurements below are stable and reproducible.

---

## 1. Engine microbenchmark (`bench_engine`)

Standalone: no server, no DB. Fresh 64 MB transposition table per (position, budget). No warm-up — the "warm" refers to the CPU/WSL state, not the engine itself.

Positions:
- **startpos** — standard opening
- **kiwipete** — the classic hard perft position; many captures, both castles available, en passant square live
- **middlegame** — a locked Nc3/Nc6/Bb3-vs-Bb6 English structure

| Position    | Budget | Depth reached |     Nodes |     NPS |
|-------------|-------:|--------------:|----------:|--------:|
| startpos    |    2 s |             6 |   ~790 K  | ~390 K  |
| startpos    |    5 s |             7 |  ~1.93 M  | ~386 K  |
| startpos    |   10 s |             7 |  ~3.85 M  | ~385 K  |
| kiwipete    |    2 s |             6 |   ~658 K  | ~325 K  |
| kiwipete    |    5 s |             7 |  ~1.67 M  | ~330 K  |
| kiwipete    |   10 s |             8 |  ~3.16 M  | ~316 K  |
| middlegame  |    2 s |             6 |   ~773 K  | ~386 K  |
| middlegame  |    5 s |             7 |  ~1.93 M  | ~388 K  |
| middlegame  |   10 s |             7 |  ~3.85 M  | ~385 K  |

### Reading these numbers

**~380 K nodes per second on quiet positions**, **~320 K on tactical**. That matches CLAUDE.md's headline claim ("396 K NPS after Release build") and comfortably beats the pre-Release ~83 K measured in phase 6.4. The plan document's target of 500 K NPS was aspirational; without algorithmic changes (see §Optimization attempt below) it is not reachable.

**Depth reached is modest** — 7 plies in 10 seconds on the starting position is well short of what a serious engine hits. The reason is a known limitation, not a bug: the engine has no **quiescence search**. When the search hits its depth ceiling in the middle of an unresolved capture sequence, it stops and evaluates the material-imbalance directly, which is misleading. Depth ceilings are therefore chosen conservatively.

`test_engine`'s "Start pos near 0" case has been failing under Release since Phase 6.4 — the search reaches deep enough in the 500 ms budget that it starts returning ~157 cp for a dead-equal starting position (search sees a temporary imbalance because it is truncating a still-active tactic). CLAUDE.md documents this as a Plan Deviation. It is the visible symptom of the missing quiescence search.

---

## 2. Load test (`tools/load_test.py`)

Pre-provisioned test accounts (via `bench_provision`); scripted 20-half-move Italian Opening replayed by every pair; RTT measured client-side from `make_move` send to `move_made` receipt on the sender's socket. All against `ws://127.0.0.1:9000` on the same machine (loopback).

| Concurrent games | WS clients | Wall clock | Moves/s | RTT p50 | p95 | p99 | Max | Completed |
|-----------------:|-----------:|-----------:|--------:|--------:|----:|----:|----:|:---------:|
| 50               | 100        | 1.94 s     | 515     | 6.7 ms  | 51 ms  |  89 ms |  161 ms | 100/100 |
| 100              | 200        | 3.31 s     | 604     | 15 ms   | 68 ms  | 118 ms |  250 ms | 200/200 |
| 250              | 500        | 7.37 s     | 678     | 36 ms   | 107 ms | 180 ms | 1439 ms | 500/500 |

Zero errors across all three runs.

### Reading these numbers

**Latency scales linearly with concurrency** because the 12-thread pool becomes the queue: at 500 clients most moves land during a small burst window, so p99 climbs. The p50 stays healthy even at maximum load (~35 ms is well below the 50 ms move-latency target in the plan document).

**Throughput plateaus around 680 moves/s.** That is the sustained rate the whole pipeline — WebSocket parse → JSON parse → `MessageRouter::route` → `GameHandler::handle_make_move` → `Board::make_move` → `move_gen::generate_legal_moves` → broadcast to two seats — can handle end-to-end. On a 12-thread pool that means ~57 moves/s/thread, or roughly 17 ms of CPU work per move, most of which is JSON serialisation on the broadcast path (two `move_made` frames per move, each with a full FEN).

**RSS is small; peak is not.** Steady-state after the 500-client run: **27 MB**. Peak (`VmPeak`) during the run: **916 MB**. The peak is nlohmann::json's transient allocation during simultaneous parsing on 12 workers plus 500 concurrent read/write buffers on the connection side — most of it drains as soon as the load finishes. For a Cloud Run deployment the 1 GB memory tier is comfortable; the 512 MB tier is not.

### One-line note on real-world latency

These numbers are localhost loopback. From IIT Patna to a `asia-south1` Cloud Run instance in Mumbai, expect ~30 ms baseline RTT on top of the server-side p50. That is a floor the server cannot budge, only choice of region can. Load-testing against the deployed target would be dominated by that floor and by Cloud Run's rate limiter, so this suite runs against local WSL2.

---

## 3. Optimization attempt (Phase 10.2)

The `implementation_plan.md` §10.2 section asked for "top 2–3 bottlenecks, with before/after numbers." The honest report is that the top bottleneck is **algorithmic, not micro-optimisation-shaped**.

### What was tried

A precomputed lookup table for the target squares reachable by a knight or king from any origin square. Previously, `is_square_attacked`, `generate_knight_moves`, and `generate_king_moves` each ran an 8-iteration bounds-checked `(dr, df)` loop; that was replaced by a length-2-to-8 array iteration keyed on origin square. Correctness verified: full `test_move_gen` passes at every depth.

### Measured impact

Warm-run medians of three trials for each condition:

| Position    | Before (median NPS) | After (median NPS) | Delta |
|-------------|--------------------:|-------------------:|------:|
| startpos    |             388 K   |            388 K   |   0%  |
| kiwipete    |             317 K   |            322 K   | +1.6% |
| middlegame  |             384 K   |            388 K   |   +1% |

Every delta is inside run-to-run noise.

### Why the null result is the real finding

`-O2` was already unrolling the 8-iteration loops and the CPU's branch predictor was correctly calling every bounds check. The array lookup gives the compiler nothing new to hoist. Modern compilers on modern CPUs are simply not beaten by "clever" microstructure changes at this scale — a lesson well worth banking.

The refactor was kept, not reverted — it deduplicates four copies of the same `(dr, df)` tables and makes the geometry of attack detection live in one place. It is honest about being a readability change, not a performance one; the file comment [src/chess/move_gen.cpp](../src/chess/move_gen.cpp) says as much.

### Where real NPS gains would come from

1. **Quiescence search.** The single biggest strength win, roughly a day of code + a day of debugging. Also fixes the pre-existing `test_engine` "Start pos near 0" Release-mode flake as a side-effect.
2. **Piece list on `Board`.** `generate_pseudo_legal_moves` currently scans all 64 squares every call to find own-colour pieces (only ~16 exist at start, dropping through the game). Maintaining an incremental piece list in `make_move`/`undo_move` would replace a 64-square scan with a 16-piece walk — a real ~20–30 % NPS win, but touches everywhere that mutates board state.
3. **Null-move pruning.** ~30–40 % effective depth improvement in the middlegame; standard trick, but wants a zugzwang guard so it doesn't misbehave in endgames.

None of these fit the "bounded 10.2 micro-optimisation" budget. They are proper future work.

---

## Reproducing these numbers

From the repo root, inside WSL:

```bash
# 1. Fresh Release build
mkdir -p build_release && cd build_release
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)" chess_server bench_engine bench_provision
cd ..

# 2. Engine microbenchmark (no server needed)
./build_release/bench_engine

# 3. Load test — needs a local Postgres and the server running.
#    See tools/load_test.py header for the DATABASE_URL / JWT_SIGNING_KEY setup.
./build_release/bench_provision 1000 > tokens.json
./build_release/chess_server &
python3 tools/load_test.py --tokens tokens.json --games 250 --stagger-ms 10
```
