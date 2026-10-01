# Architecture

This is a design-decisions document, not an API reference. Every non-obvious choice below has a reason it was made, and — usually — an alternative that was rejected. The point is that a reader who lands on this codebase for the first time can understand *why* it looks the way it does, and where they can safely change things.

For the concrete wire format, see [PROTOCOL.md](PROTOCOL.md). For the security model, [SECURITY.md](SECURITY.md). For the numbers backing performance claims, [BENCHMARKS.md](BENCHMARKS.md).

---

## 1. What this project is

A multiplayer chess platform built from near-scratch in C++17 as an educational systems project. External dependencies are deliberately kept few: **only what would be genuinely unsafe or disproportionate to hand-write** is a library call.

| Hand-written                                                                | Library call                          | Why library |
|-----------------------------------------------------------------------------|---------------------------------------|-------------|
| TCP + epoll event loop, WebSocket (RFC 6455), JSON message router           | –                                     | – |
| Thread pool + MPMC task queue                                               | –                                     | – |
| Chess rules (board, moves, notation, perft-verified to depth 5)             | –                                     | – |
| Chess engine (alpha-beta + iterative deepening + Zobrist TT + MVV-LVA)      | –                                     | – |
| SHA-256, SHA-384, SHA-512, HMAC, HKDF, AES-256, AES-CTR + HMAC AEAD, JWT    | –                                     | – |
| Token-bucket rate limiter, Fischer clock, Swiss pairing, anti-cheat stats  | –                                     | – |
| –                                                                           | OpenSSL 3.5 CSPRNG + Argon2id         | never hand-roll a CSPRNG; Argon2id is out of syllabus |
| –                                                                           | OpenSSL 3.5 ML-KEM-768, ML-DSA-65, X25519 | LWE and Module-LWE are studied, but constant-time lattice code is a separate specialisation with a **silent** failure mode (weak sampling still round-trips perfectly); the reference Kyber shipped a timing leak in 2024 (KyberSlash) that reached expert libraries. A study implementation belongs in `research/`, not `chess_server`. |
| –                                                                           | `nlohmann/json`                       | header-only, no build complexity, hand-rolling adds nothing pedagogical |
| –                                                                           | `libpq`                               | a Postgres wire protocol implementation is not the point of this project |

Every crypto primitive listed under "hand-written" is validated against **FIPS 180-4 / RFC 4231 / RFC 5869 vectors and cross-checked differentially against OpenSSL** on 10 000 random inputs. See `tests/test_hash.cpp`, `test_aes.cpp`.

---

## 2. Data flow for one live move

```
Browser ──ws──► epoll loop (main thread, tcp_server.cpp)
                    │  reads bytes into Connection buffer
                    └─► ThreadPool::submit(...)
                            │
                            ▼
                       Worker thread
                            │
            handle_client_data() — parses HTTP upgrade OR WebSocket frames
                            │
                            ▼
                MessageRouter.route(json)          ← sealed-envelope unwrap
                            │                       for opted-in types
                            ▼
                GameHandler::handle_make_move()
                            │
                            ▼
                 GameRoom (per-room mutex)
                  ├─ Board (chess/board.cpp) validates via move_gen
                  ├─ Fischer clock switched (steady_clock)
                  └─ Broadcast move_made frame to both seats
                     + every spectator
```

The whole flow is single-process. There is no separate matchmaking service, no separate replay service, no message bus. The plan's original diagram showed those as separate components; in practice they all live behind the same `MessageRouter` because they share the same `GameRoom` state and factoring them out would only introduce serialisation without a scale reason.

---

## 3. Big decisions, and why

### 3.1 Mailbox board, not bitboards

`Board` stores an `std::array<Piece, 64>` (a "mailbox"), rank-major, `a1 = 0`. Bitboards would give roughly one order of magnitude speedup on move generation *if* combined with magic bitboards for sliding-piece attacks — and would be about 3× the code, most of which is off-syllabus tricky bitmath.

The plan document deferred bitboards to "Phase 10 if needed"; when Phase 10 arrived and the numbers were measured, the answer was **no**. Steady-state NPS is ~380 K on quiet positions (see [BENCHMARKS.md](BENCHMARKS.md)). For a personal-scale platform with a handful of AI games at a time, that is comfortably enough. What the engine actually lacks is quiescence search, not raw speed.

### 3.2 Mutex + condition variable, not lock-free

`concurrent/task_queue.h` uses `std::mutex` + `std::condition_variable`. The plan doc pitched "MPMC lock-free queue"; that got rejected on measurement. On this workload:

- Producers arrive at ~10 Hz per connection (one enqueue per WebSocket frame).
- Workers block on the CV, wake, dequeue, process, block again.
- Contention on the queue mutex is < 0.1 % of total wait time in profiling.

A lock-free MPMC is 5× the code, opens the door to ABA bugs, and buys nothing measurable at these arrival rates. The rule cited: *complexity must justify itself with numbers.* This one couldn't.

### 3.3 Sharded room map with `shared_ptr` lifetime

`RoomManager` stores rooms across N shards (`std::unordered_map<GameId, std::shared_ptr<GameRoom>>` per shard) keyed by game id. Two consequences worth reading twice:

1. **Sharding**: a hash of `game_id` picks the shard, so unrelated games contend on different mutexes. That is textbook.
2. **`shared_ptr` for lifetime**: a worker thread takes a `shared_ptr<GameRoom>` copy out of the manager and works on it. If the last player disconnects and the manager evicts the room while the worker is still executing `handle_make_move`, the worker's local `shared_ptr` keeps the room alive until the function returns. **This is not a leak** — the reference count is bounded by "how many workers currently have this game in their local frame", which is at most `hardware_concurrency()`.

The alternative was raw pointers with an "is this room still alive?" check. That is a race condition disguised as an optimisation.

### 3.4 Recursive mutex on `connections_mutex_`

`TcpServer::connections_mutex_` is `std::recursive_mutex`. This is deliberate. Bug #2 in the implementation log: the matchmaker's match-found callback locks it while a worker is already holding it (because the worker is *inside* the connection-write path that the callback invokes). Making the mutex recursive fixed the deadlock; **cleaning it back to `std::mutex`** — a natural refactor instinct — **reintroduces the bug.** There is a comment on the declaration; do not disregard it.

### 3.5 Edge-triggered `EPOLLET` with manual write-flush

The main event loop uses `EPOLLET` for reads. Edge-triggered means the kernel notifies exactly once when a socket becomes readable/writable; it is up to the app to drain until `EAGAIN`. That is the recommended mode for high-connection-count servers, and it is what the plan asked for.

The consequence, though, is that when a handler writes to *another* player's connection (matchmaking notification, opponent move broadcast), it cannot rely on `EPOLLET` to schedule a later flush. The convention: **drain the write buffer inline right there** —

```cpp
while (conn->has_data_to_write()) conn->write_to_socket();
```

— and this pattern is repeated everywhere a handler talks to a foreign fd. If a message ever "vanishes" during matchmaking, this is the first place to look.

### 3.6 Postgres via Supabase, not SQLite

The plan doc specified SQLite; Phase 7 replanning switched to Postgres via Supabase. The reason is a hard Cloud Run constraint: Cloud Run's filesystem is **ephemeral and RAM-backed**, so a SQLite file is wiped on every container restart and costs against the memory limit. The switch is a straight substitution — `libpq` for `sqlite3` — with two bonuses: real concurrent-writer support (goodbye to the "single-writer + WAL" workaround) and out-of-process backups.

Supabase specifically was chosen for two orthogonal reasons: (a) it is a hosted Postgres so no VM to run, and (b) its Auth service lets Google Sign-In work *without* hand-rolling OAuth (see [SECURITY.md](SECURITY.md)). Nothing in the C++ server is Supabase-specific — the driver is stock `libpq`, and the code would run against a self-hosted Postgres unchanged.

### 3.7 Auth on the game socket is JWT (HS384), not sessions

The access token is a JWT signed with HMAC-SHA-384. The refresh token is 32 random bytes; the sessions table stores its SHA-384. Two design points:

- **HS384, not HS256.** The rest of the crypto stack targets NIST Category 3 (~192-bit classical strength). Using HS256 here would make the JWT the weakest link at 128 bits. Cost: 16 extra bytes per signature.
- **Refresh tokens are opaque, not JWTs.** JWTs cannot be revoked without an allowlist that defeats the point. Refresh tokens are opaque so the server can delete a row to log a user out. Rotation, family revocation, and `logout_all` are all row deletes; `players.token_epoch` is bumped for `logout_all` and the access-token verifier re-checks it.

### 3.8 Sealed envelope: one-shot, not a channel

`register` and `login` — the two messages that ever carry a password on the wire — are wrapped in a **sealed envelope**: an ML-KEM-768 + X25519 hybrid KEM whose one-time server key is signed by the server's long-lived ML-DSA-65 identity. Rev 1 of Phase 7 proposed a **persistent PQC channel** encrypting every WebSocket message, chess moves included. Rev 2 shrank it to a one-shot payload sealer, because:

- **Chess moves are not long-lived secrets.** `wss://` already protects them in transit.
- **Passwords are.** Harvest-now-decrypt-later is a genuine threat to a long-lived secret. Making the exchange PQC-hybrid means a record-then-decrypt attack fails even in a post-quantum world.
- **PQ signatures on moves defend against the operator forging results.** Nobody needs protection from the operator on a personal project.

The service is deliberately payload-agnostic — the same envelope path can wrap `change_password` or a future `send_private_message` with zero new crypto code. Registration of "which types are sealed" is a single call in `main.cpp`; unsealed arrivals are rejected, so an attacker cannot strip the envelope and downgrade the request.

### 3.9 Fischer clock via `steady_clock`, on the server

Time on the wire is authoritative. The server holds `white_time_ms` and `black_time_ms`. On `make_move`, the elapsed time is subtracted from the mover's clock, the Fischer increment is added, and the new pair is broadcast. Clock computations use `std::chrono::steady_clock` — never wall clock — because a system time jump would otherwise flag every online game.

The client displays what the server sends. It does not tick between frames.

---

## 4. Threading and locks

There are four kinds of shared state:

| State                                | Guard                                           | Locking scope |
|--------------------------------------|-------------------------------------------------|---------------|
| Per-connection buffers               | Held only on one worker at a time (see below)   | – |
| `TcpServer::connections_` map        | `std::recursive_mutex connections_mutex_`       | Short; hold only while adding/removing a Connection |
| `RoomManager` shards                 | `std::mutex` per shard                          | Held during map insert/erase/find_by_id/find_by_fd |
| `GameRoom` internal state            | One `std::mutex` per room                       | Held during `make_move`, clock update, seat mutation, and broadcast list snapshot |

Two invariants:

- **A Connection is processed by at most one worker at a time.** The main epoll loop, on receiving an EPOLLIN, marks the connection "processing", submits a task, and skips future EPOLLIN notifications for that fd until the worker clears the flag. This is the workhorse of connection-safety without needing a per-connection mutex.
- **Locks nest one way only**: `connections_mutex_` → shard mutex → `GameRoom` mutex. Never the reverse. All handler paths respect this; the recursive-mutex quirk is inside this same order, not a violation of it.

---

## 5. Where the code lives

Roughly one directory per architectural layer. Anything that looks like it belongs in two places usually does — the actual dividing line was drawn empirically as the code was written, and the log records why every deviation happened.

| Directory | Contents | Notes |
|-----------|----------|-------|
| `src/core/` | `types.h`, `Result<T,E>`, thread-safe `Logger` | Foundational types, no dependencies on anything else in the tree |
| `src/net/` | `TcpServer`, `Connection`, `WebSocket`, `MessageRouter` | Pure networking; no chess, no auth |
| `src/concurrent/` | `ThreadPool`, `TaskQueue` | Independent of everything |
| `src/chess/` | `Board`, `move_gen`, `notation`, `zobrist`, `transposition_table`, `evaluator`, `engine` | Perft-verified move generator is the ground truth for all chess logic |
| `src/game/` | `GameRoom`, `RoomManager`, `GameHandler`, `Matchmaker`, `AIPlayer`, `match_notify` | The "gluing chess to the network" layer |
| `src/crypto/` | Hand-written SHA/HMAC/HKDF/AES/AEAD/base64, RAII wrappers for ML-KEM/ML-DSA/X25519, sealed envelope | Everything under here has FIPS/RFC KATs and OpenSSL differential tests |
| `src/auth/` | `password`, `username`, `token`, `session`, `oauth_verify`, `rate_limiter`, `auth_handler`, `service` | Passwords never leave this directory in the clear |
| `src/storage/` | `database`, `player_repo`, `game_repo`, schema, migrations | Every query is parameterised; no string concatenation |
| `src/analysis/` | Anti-cheat statistical engine + persistence | Phase 9.3 |
| `src/tournament/` | Swiss pairing + persistence + lifecycle manager | Phase 9.4 |
| `frontend/` | Vanilla JS, HTML5 Canvas board, dark-theme CSS, no framework | Modular files under `js/` |
| `tests/` | Each phase has its own binary — no GoogleTest, minimal harness | Passes end-to-end at every commit under `-Werror` |
| `tools/` | Utility executables and scripts | `bench_engine`, `bench_provision`, `load_test.py`, key-generation helpers |

---

## 6. What is deliberately not here

Worth naming explicitly, because their absence is a design choice, not a TODO:

- **No opening book.** The plan called for one. In practice, at the engine's search depth an opening book helps only in the very first few moves, where the outcomes are close to equal anyway. The engine plays known-good opening lines from search regardless, and a book file would be one more thing to ship.
- **No binary move protocol.** See [PROTOCOL.md](PROTOCOL.md) — the plan sketched an 8/12-byte binary encoding; JSON turned out fine and easier to debug.
- **No exceptions on the hot path.** All error handling is `Result<T, Error>` explicit-return; exceptions surface only on truly unexpected failures (`std::bad_alloc`, JSON parse errors from external input) and log the source.
- **No `new` / `delete` in application code.** `std::unique_ptr`, `std::shared_ptr`, RAII wrappers. If you find a raw `new` outside a placement-new inside a container, that is a bug.
- **No per-connection PQC channel.** Deliberate scope-cut in Phase 7 rev 2; see [SECURITY.md](SECURITY.md) and `implementation_phase_7.md`.
- **No production of a hand-rolled Kyber implementation on the shipping path.** A study implementation lives in `research/kyber_reference/` (planned) and is validated against NIST KATs; nothing under `src/` links against it.

---

## 7. The LLD refactor (2026-09, LLD-0 through LLD-7)

The nine-phase implementation plan produced a working platform whose top-level classes had grown wide. `GameHandler` alone owned 24 routes, all authentication extraction, all JSON parsing, matchmaking orchestration, room lifecycle, persistence, spectator broadcast, and inline error frames — ~1300 lines carrying six concerns. The LLD refactor cut that down to a set of narrow adapters + cohesive services + explicit boundaries, without changing a single wire frame or breaking any of the 40+ test binaries.

The seven phases are all recorded in `docs/implementation_log.md` (kept local per convention); the summary below is the shape you can rely on when reading the code today.

### 7.1 Four layers, one rule

```
Transport            src/net/          WebSocket framing, delivery, generation-guarded handles
Protocol             src/protocol/     Route policies, request pipeline, JSON codec, DTOs
Application          src/application/  Ports (interfaces), services (use-case orchestration)
Domain               src/chess/, game/ Board, engine, rules, GameRoom
Infrastructure       src/storage/,     Postgres adapters, sealed registry adapter, crypto
                     src/crypto/
```

**Dependency rule:** application depends on domain values and narrow ports; infrastructure implements ports. Nothing in `src/chess/` or `src/game/` includes `nlohmann/json`, `libpq`, or `openssl` — that's the boundary. `src/game/` holds domain state (rooms, matchmaker), which is why it isn't merged into `src/chess/`.

### 7.2 The request pipeline

Every incoming WebSocket message flows through one auditable path:

```
raw frame → SealOpen → ParseJson → Auth → Dispatch          (typed routes)
raw frame → SealOpen → Dispatch                             (raw routes — AuthHandler)
```

`RequestPipeline` (`src/protocol/request_pipeline.h`) owns the stages. Each route declares its `RoutePolicy` alongside its handler:

```cpp
pipeline.register_route(
    RoutePolicy{"create_game", AuthRequirement::Required},
    [this](RequestContext& c, const json& m, MessageSink& s) { ... });
```

* **SealOpen** consults an injected `SealOpenFn` (in production a lambda wrapping `crypto::SealedRegistry::inspect`). Rejected → silent drop; Rewritten → replaces the raw message with plaintext; Continue → passthrough.
* **ParseJson** runs `nlohmann::json::parse` once at the boundary. Malformed → `{type:"error", message:"Invalid JSON: ..."}`.
* **Auth** consults an injected `IdentityExtractor`. Missing/invalid token on an `AuthRequirement::Required` route → `{type:"error", code:"auth_required", message:"..."}`.

AuthHandler registers as **raw routes** — it keeps its own `auth_error{code:...}` frame shape, its five per-surface `RateLimiter` members, and the login-per-account deceptive rate-limit that returns `invalid_credentials` (not `rate_limited`) so the wire cannot enumerate accounts.

### 7.3 The port catalogue

| Port | Header | Real adapter | Fake / Null |
|------|--------|--------------|-------------|
| `MessageSink` | `application/ports/message_sink.h` | `net::SocketMessageSink` | `FakeSink` in tests |
| `GameStore` | `application/ports/game_store.h` | `storage::PostgresGameStore` | `ports::NullGameStore` |
| `PlayerQueries` | `application/ports/player_queries.h` | `storage::PostgresPlayerQueries` | `ports::NullPlayerQueries` |
| `Clock` | `application/ports/clock.h` | `ports::SystemClock` | `ports::FakeClock` |
| `MoveSelector` | `application/ports/move_selector.h` | `chess::EngineMoveSelector` | `FakeMoveSelector` in tests |
| `IdentityExtractor` | `application/auth/identity_extractor.h` | (uses DB + signer) | null-args extractor fails closed |

The `Null*` adapters implement the "capability-disabled" pattern: when the server starts without `DATABASE_URL`, services receive real non-null references that always return `{ok:false, code:Disconnected}`. Handlers never branch on `if (!db_)` — the port's `capable()` flag is the only signal.

### 7.4 GameRoom completion path

```
submit_move → board.make_move → status?
                                  │
                                  └─ terminal → finish_game (under mutex_)
                                                  │
                                                  ├─ build_snapshot_locked
                                                  │     (immutable value:
                                                  │      board, timings, seats,
                                                  │      completion_uuid, revision)
                                                  │
                                                  └─ queue GameCompleted event
LockAndDrain destructor: releases mutex, then fires
                          on_game_completed(ev) on every listener
                                                  │
                                                  ▼
                          GameCompletionService::on_game_completed
                                                  │
                                                  ├─ short-circuit if AI game / no auth / store !capable
                                                  │
                                                  └─ store.save_completed_game(snapshot)
                                                        │
                                                        ├─ SELECT id WHERE completion_uuid = $1
                                                        │     ← already_persisted retry short-circuit
                                                        │
                                                        └─ BEGIN → INSERT games + moves + rating updates → COMMIT
                                                              │
                                                              └─ partial unique index on completion_uuid
                                                                 catches the concurrent-retry race
```

Idempotency key is a v4 UUID generated once at snapshot build (`src/core/uuid.cpp`, not CSPRNG because the use is uniqueness not security). A retry with the same snapshot returns `already_persisted=true` and does not re-increment ELO/stats.

Notification of participants and spectators is a **separate** path — inline in `GameplayService`, using the caller's `MessageSink`. Broadcast cannot swallow a persist failure and vice versa.

### 7.5 Concurrency invariants (LLD-6)

Three additive invariants make future async work safely possible; none of them is exercised under the current serialized event loop, but every one of them is testable today.

**ConnectionHandle generation (LLD-1)** — `Connection` holds a `uint64_t generation_` bumped in `TcpServer::handle_new_connection`. `ConnectionHandle{fd, generation}` is the stable identity a foreign-fd sender remembers. `SocketMessageSink::send` compares generations before writing; a fd reused for a new client cannot receive frames queued for the previous holder.

**Bounded write buffer (LLD-6.4)** — `Connection::MAX_WRITE_BUFFER_BYTES = 4 MB`. Every append checks the cap; overflow flips a sticky `write_buffer_overflowed()` flag and drops the write. `TcpServer::handle_client_data` closes the connection on the flag. A slow client cannot force RAM growth.

**GameRoom revision counter (LLD-6.4)** — `std::atomic<uint64_t> revision_` bumped at the top of every mutating public method. `GameSnapshot` pins the revision at build time. A future async consumer holding a snapshot compares `snapshot.revision` against `room->revision()` before applying its result and discards on mismatch. Read-only accessors do not bump.

### 7.6 What was NOT changed

The plan flagged the following as **out of scope**:

* Rating semantics — the pre-refactor snapshot-based ELO math is preserved bit-for-bit; concurrent-game rating policy is a separate future decision.
* The global `connections_mutex_` (recursive) is retained — plan §6.4 forbids removing it "just to improve a benchmark." Per-connection single-flight is already provided by holding it end-to-end in `handle_client_data`; per-fd atomic flags would add nothing today.
* AI move computation is synchronous. Async extraction needs a benchmark first proving current serialization is a bottleneck — none exists.
* Frontend visual redesign — no CSS changes, no new screens; only the `Screen::unmount` teardown was hardened (LLD-7).

See `docs/SEQUENCES.md` for the four sequence diagrams (move accept, completion + failed-save retry, sealed login, disconnect + revision-guarded stale result) the plan asked for. See `implementation_log.md` for the phase-by-phase record of what changed and why.

### 7.7 Scheduled live tournament path

Tournament chess reuses the normal authoritative game and completion pipeline;
it does not introduce a second game implementation:

```text
browser create/register/check-in commands
                 │
                 ▼
TournamentHandler → TournamentService → TournamentManager → PostgreSQL
                                              │ scheduled maintenance tick
                                              ▼
                                  TournamentRuntimeService
                                      │ reserves durable seats
                                      ▼
                                   RoomManager → GameRoom
                                      │ terminal immutable snapshot
                                      ▼
GameCompletionService → PostgresGameStore → TournamentCompletionSink
                                              │ idempotent pairing result
                                              ▼
                         recomputed tournament score + Buchholz + next round
```

`TournamentManager` owns deadlines, lifecycle transitions, Swiss state, and
audited corrections. `GameRoom` owns chess rules and clocks.
`TournamentRuntimeService` is the Facade/Mediator that creates or reconstructs
scheduled rooms and binds sockets only when the authenticated database player
id matches a reserved seat. A `pairing_id` survives in the immutable
completion snapshot, so only a successfully persisted game can resolve the
pairing automatically. Duplicate completion delivery is a no-op at both the
game and tournament boundaries.

The browser is a projection of this state. It displays local-time schedules,
one contextual countdown, registration/check-in controls, pairings, and live
game links, but it never decides whether a deadline passed or a result counts.
`tournament_game_ready` is emitted only after `pairing.game_id` is durable;
refresh recovery uses public `tournament_state` plus authenticated
`get_active_game`. Creator overrides change tournament standings and Buchholz
only; they intentionally do not rewrite the immutable replay, profile record,
or global rating.

Scheduled but unstarted rooms can be reconstructed after process loss.
Already-moving rooms are still process-memory state, so the first Cloud Run
deployment must remain single-instance and must not claim active-game failover.
