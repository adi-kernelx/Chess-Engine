# Multiplayer Chess Platform

A production-grade multiplayer chess platform written from near-scratch in C++17. Real-time online play, spectating, replay, engine analysis, Swiss tournaments, and a from-scratch chess engine — all built on infrastructure that is largely hand-written, backed only by a small set of well-justified libraries (OpenSSL for CSPRNG and lattice crypto, `nlohmann/json`, `libpq`).

This project was built as an educational systems exercise: **anything I have formally studied is hand-written**; a library is used only where from-scratch would be genuinely unsafe or disproportionate.

- **Deep documentation**: [ARCHITECTURE.md](docs/ARCHITECTURE.md) · [PROTOCOL.md](docs/PROTOCOL.md) · [SECURITY.md](docs/SECURITY.md) · [BENCHMARKS.md](docs/BENCHMARKS.md)
- **Implementation plan**: [implementation_plan.md](implementation_plan.md)
- **Phase-7 detail (identity + crypto)**: [implementation_phase_7.md](implementation_phase_7.md)

---

## What it does

- Two players can play a full game of chess over WebSocket, with a **server-authoritative** rules engine.
- Quick-play matchmaking finds an opponent within an **ELO band** that widens if no match arrives.
- Spectators can join any live game read-only; fan-out is non-blocking.
- Completed games are **persisted** in Postgres and can be replayed move-by-move in the browser with optional engine evaluation at each ply.
- Anyone can play against the built-in **chess engine** — alpha-beta + iterative deepening + Zobrist transposition table + MVV-LVA move ordering.
- Full authenticated identity: password + Argon2id, or **Google Sign-In via Supabase Auth**. Password-carrying messages travel inside a **post-quantum sealed envelope** (ML-KEM-768 + X25519 + ML-DSA-65 + AES-256-CTR + HMAC-SHA-384).
- **Scheduled Swiss tournaments** with registration deadlines, round check-in,
  reserved live games, automatic results, no-show handling, and live standings.
- Statistical **anti-cheat** analysis of persisted games based on move-time correlation with position complexity.

---

## Architecture at a glance

```
Browser ──ws──► epoll loop (main thread)
                    │  reads bytes into per-Connection buffer
                    └─► ThreadPool::submit(...)
                            │
                            ▼
                       Worker thread
                            │
              parse HTTP upgrade OR WebSocket frame
                            │
                            ▼
                    MessageRouter.route(json)
                            │
                            ▼
                    GameHandler / AuthHandler
                            │
                            ▼
                     GameRoom (per-room mutex)
                        ├─ Board  (validate move)
                        ├─ Clock  (steady_clock)
                        └─ Broadcast to both seats + spectators
```

Full design rationale — why mailbox not bitboards, why mutex+CV not lock-free, why the sealed envelope is one-shot not persistent, why Postgres not SQLite — lives in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Build & run

Development happens inside **WSL2 (Ubuntu 26.04 LTS)** on Windows, or any recent Linux. The code needs POSIX sockets, `epoll`, and pthreads — it does not build natively on Windows.

**Toolchain requirements**: GCC 15.2+, CMake 3.16+, OpenSSL 3.5+ (for ML-KEM-768, ML-DSA-65, Argon2id in the default provider — earlier versions do not have these), `libpq`, `nlohmann/json` (vendored).

```bash
# Fresh Release build
mkdir -p build_release && cd build_release
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"

# Run the server
./chess_server           # listens on ws://0.0.0.0:${PORT:-9000}
```

The frontend is entirely static. Serve `frontend/` with any HTTP server:

```bash
cd frontend && python3 -m http.server 8000
# open http://localhost:8000
```

**Warning about `-O`.** `CMakeLists.txt` intentionally sets no default build type — a bare `cmake ..` compiles at `-O0` with `-Wall -Wextra -Wpedantic -Werror`. That is fine for development and debugging but ~5× slower than Release. Every performance number in this repo assumes `-DCMAKE_BUILD_TYPE=Release`.

### Environment variables

`chess_server` reads a handful of environment variables at startup. Most are optional; the server logs which features it disabled and why.

| Variable                     | Purpose                                         | Required for |
|------------------------------|-------------------------------------------------|--------------|
| `PORT`                       | Listen port (default 9000)                      | Cloud Run |
| `DATABASE_URL`               | `postgresql://…` connection string              | Auth, persistence |
| `JWT_SIGNING_KEY`            | Base64-encoded 32-byte key for HS384 access-token signing | Auth |
| `SERVER_IDENTITY_KEY_PATH`   | Path to the ML-DSA-65 private key file          | Sealed-envelope register/login |
| `SUPABASE_JWT_SECRET`        | Supabase project's legacy HS256 secret          | Google Sign-In |

Local Google OAuth setup and the current HS256/JWKS compatibility boundary are
documented in [`docs/SUPABASE_GOOGLE_AUTH_SETUP.md`](docs/SUPABASE_GOOGLE_AUTH_SETUP.md).

---

## Tests

Every phase has its own custom test binary. After `make`:

```bash
./test_move_gen                # perft to depth 5 — the correctness ground truth
./test_engine                  # alpha-beta search + eval
./test_transposition_table     # Zobrist hashing + TT
./test_hash                    # SHA-256/384/512 + HMAC + HKDF vs FIPS/RFC vectors + OpenSSL diff
./test_aes                     # AES-256, CTR, AEAD
./test_pqc                     # ML-KEM / ML-DSA / X25519 wrappers
./test_sealed_envelope         # PQC-hybrid one-shot sealer
./test_token                   # JWT HS384 + refresh rotation + logout_all
./test_password                # Argon2id + username validation
./test_auth_required           # every game command rejects unauth'd callers
./test_spectator               # broadcast fan-out + disconnect sweep
./test_replay                  # persisted game replay round-trip
./test_anti_cheat              # statistical move-time analyser
./test_tournament              # Swiss pairings + full 8-player run
# and more — see CMakeLists.txt for the full list
```

DB-backed tests skip cleanly when `DATABASE_URL` is not set. A single documented Release-mode flake (`test_engine` "Start pos near 0", CLAUDE.md §Build & Run) is the visible symptom of missing quiescence search, not a regression.

---

## Headline benchmarks

Measured on AMD Ryzen 5 5600H (6c/12t) under WSL2 Ubuntu 26.04, Release `-O2`, 17 September 2026. Medians of three warm runs — see [BENCHMARKS.md](docs/BENCHMARKS.md) for full methodology and raw runs.

### Chess engine (`bench_engine`)

| Position    | 2 s budget | 5 s budget | 10 s budget |
|-------------|-----------:|-----------:|------------:|
| startpos    | depth 6, 390 K NPS | depth 7, 386 K NPS | depth 7, 385 K NPS |
| kiwipete    | depth 6, 325 K NPS | depth 7, 330 K NPS | depth 8, 316 K NPS |
| middlegame  | depth 6, 386 K NPS | depth 7, 388 K NPS | depth 7, 385 K NPS |

### Multiplayer server (`load_test.py`, WSL2 loopback)

| Concurrent games | WS clients | Wall clock | Moves/s | RTT p50 | RTT p99 | Completed |
|-----------------:|-----------:|-----------:|--------:|--------:|--------:|:---------:|
| 50               | 100        | 1.94 s     | 515     | 6.7 ms  |  89 ms  | 100 / 100 |
| 100              | 200        | 3.31 s     | 604     | 15 ms   | 118 ms  | 200 / 200 |
| 250              | 500        | 7.37 s     | 678     | 36 ms   | 180 ms  | 500 / 500 |

Steady-state RSS after the 500-client run: **27 MB**; peak during the run: **916 MB** (nlohmann::json transient allocation across 12 worker threads).

The plan's aspirational "500 K NPS" and "sub-50 ms p99 at 500 games" targets are not met — those would require quiescence search and less-JSON-heavy hot paths respectively. See [BENCHMARKS.md](docs/BENCHMARKS.md) §Optimization attempt for the honest write-up.

---

## Project status

All ten phases of the original implementation plan are complete:

- **Phases 1–2** — epoll TCP server, thread pool, WebSocket RFC 6455, JSON router.
- **Phase 3** — perft-verified chess rules engine (all depths 1–5 match published counts).
- **Phase 4** — online multiplayer, matchmaking, Fischer clock.
- **Phase 5** — dark-theme browser frontend, HTML5 Canvas board, WebSocket client.
- **Phase 6** — alpha-beta engine with Zobrist TT.
- **Phase 7** — password + JWT + sealed-envelope + Google Sign-In (via Supabase Auth).
- **Phase 8** — Postgres persistence (games, ELO transactions, indexed history).
- **Phase 9** — spectator mode, replay, statistical anti-cheat, Swiss tournaments.
- **Phase 10** — benchmarking + documentation (this pass).

Deferred future work is listed in [docs/BENCHMARKS.md](docs/BENCHMARKS.md) §Where real NPS gains would come from and in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §What is deliberately not here.

---

## Layout

```
src/
  core/        types, Result<T,E>, logger
  net/         TCP + epoll, Connection, WebSocket, MessageRouter
  concurrent/  thread pool, task queue
  chess/       board, move_gen, notation, zobrist, TT, evaluator, engine
  game/        rooms, matchmaking, handler, AI player
  crypto/      hand-written SHA/HMAC/HKDF/AES/AEAD/base64; RAII around ML-KEM/ML-DSA/X25519
  auth/        password, JWT, sessions, OAuth verifier, rate limiter
  storage/     libpq wrapper, repositories, schema, migrations
  analysis/    Phase 9.3 anti-cheat
  tournament/  Phase 9.4 Swiss + persistence
frontend/      vanilla JS + HTML5 Canvas, no framework
tools/         bench_engine, bench_provision, load_test.py, key generators
tests/         one binary per phase, no test framework, plain assertions
docs/          ARCHITECTURE, PROTOCOL, SECURITY, BENCHMARKS
```
