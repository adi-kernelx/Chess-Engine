# Architecture

A C++17 modular monolith for real-time chess. The static browser client communicates over WebSocket; Supabase PostgreSQL stores identities and completed results.

See [Protocol](PROTOCOL.md), [Security](SECURITY.md), [Sequences](SEQUENCES.md) and [Benchmarks](BENCHMARKS.md). The [architecture SVG](../UML/01-architecture-overview.svg) provides a visual overview.

## 1. Layers and dependency direction

| Layer | Location | Responsibility |
| --- | --- | --- |
| Transport | `src/net/`, `src/concurrent/` | Connections, WebSocket framing, bounded workers and delivery |
| Protocol | `src/protocol/` | Structural JSON decoding, route policy and request pipeline |
| Application | `src/application/` | Use-case services and narrow ports |
| Domain | `src/chess/`, `src/game/` | Rules, search, rooms, clocks and matchmaking |
| Infrastructure | `src/storage/`, `src/auth/`, `src/crypto/` | PostgreSQL, identity validation and cryptographic adapters |
| Feature modules | `src/tournament/`, `src/analysis/` | Tournament rules/storage and review-oriented game analysis |
| Browser | `frontend/` | Vanilla ES modules, CSS, Canvas board and SVG assets |

Application services depend on domain values and ports. Concrete database/socket/crypto adapters are supplied at composition time. Chess legality does not depend on JSON, libpq or a browser.

Important ports include message delivery, game persistence/query storage, clocks, move selection and tournament runtime. Null/fake implementations allow isolated service tests and explicit unavailable-feature behavior.

## 2. Request flow

```text
WebSocket text frame
  → seal inspection and JSON/type extraction
  → RequestPipeline route policy
  → identity extraction where required
  → typed application service or raw auth adapter
  → domain state / infrastructure port
  → serialized reply and room events
```

Auth routes retain their own credential-specific rate limiting and error frames. Typed game/query/tournament routes use shared decoding and authorization. JSON field order is not the server's routing contract.

A live move reaches GameplayService and GameRoom. The room validates it, updates position/history/clocks and records events under its own mutex. Listener delivery runs after unlocking. A completion event contains an immutable final snapshot for persistence.

## 3. Concurrency and ownership

### Connections and delivery

The epoll loop schedules connection work onto a bounded pool. Per-connection processing guards and generation-tagged handles prevent concurrent/stale processing from operating on a replacement socket. Writes have a bounded buffer; slow consumers cannot grow it without limit.

Transport maintenance is separated from durable tournament scheduling. Blocking SQL must not run while holding the broad connection-map lock: a slow tournament read must not prevent unrelated WebSocket upgrades or gameplay delivery.

### Rooms and events

RoomManager uses sharded maps of shared room pointers. Workers retain a room while using it; map eviction does not invalidate that reference. Each room protects mutable board, seat, clock and result state independently.

GameRoom transitions through waiting, in-progress and finished states. Notifications are drained after unlocking to avoid listener re-entry and cross-room lock cycles. Revision-tagged snapshots prevent obsolete engine work from changing a newer/finished position.

The task queue uses a mutex and condition variable. A lock-free queue is not needed to express or verify the required ownership rules.

### Database connections

libpq connections are not used concurrently by unrelated threads. Existing auth-write, tournament and persistence sessions retain their guards.

A bounded DatabasePool is used narrowly for fresh identity/profile/epoch reads:

- Exclusive movable leases pin one session through a transaction.
- Return rolls back an unfinished/aborted transaction before reuse.
- Checkout wait is bounded at 1,500 ms.
- Broken sessions are quarantined and reconnected on a later borrow; failed queries are not automatically replayed.
- Fresh reads preserve immediate logout-all epoch visibility; there is no identity cache that silently delays revocation.
- Saturation is unavailable/retryable, not an invalid-session verdict.

`AUTH_READ_POOL_SIZE` defaults to 2, accepts 0–4; zero disables the pool. Three dedicated sessions remain: five total connections by default, seven at maximum. Include overlapping revisions and administrative clients when budgeting database connections.

`SERVER_WORKER_THREADS` defaults to 4, accepts 1–8. Two workers are the intended one-vCPU starting point for overlapping I/O; threads, vCPUs, database connections and cloud instances are different controls.

## 4. Chess and engine decisions

A 64-square mailbox board supports legal moves, undo, FEN, SAN, castling, en passant and promotion. Perft and rule tests are the correctness baseline.

Search uses alpha-beta, iterative deepening, move ordering, Zobrist transposition tables and quiescence with check evasions. Difficulty choices bound depth/time. Bot display ratings are not measured engine ratings.

The server uses monotonic clocks for elapsed game time. The client interpolates received clocks for display; only the server decides timeout and results. Scheduled tournament timestamps use wall-clock time for event scheduling, not elapsed move accounting.

A binary move protocol, bitboard rewrite and distributed engine service are outside the current release.

## 5. Shared gameplay and durable completion

Ordinary and tournament games use the same GameRoom, board, clock, player controls and spectator stream. TournamentRuntimeService adds reserved identities, scheduled start gates and no-show handling; it does not implement a second chess engine.

Completion persistence receives a final value snapshot and a completion identity. Atomic writes update the saved game, move timings and applicable player statistics/ratings. Duplicate completion delivery must not duplicate a durable game or rating update. Failed persistence is reported/retried, not presented as a successful save.

Live room IDs and durable replay IDs are distinct. A tournament pairing receives its saved replay ID from completion persistence. Byes and unplayed forfeits cannot point to an unrelated game.

## 6. Tournament lifecycle

Registration closes at its deadline or the first-round pairing lock, whichever comes first. Pairings are prepared 90 seconds before round one. Reopening after the lock is denied.

Early check-in reserves a seat; game entry cannot bypass the scheduled start. A missing opponent prevents clocks from running. Check-in expiry resolves one-sided and double no-shows as forfeits.

State reads use coherent database snapshots and short per-event coalescing to reduce repeated work. Normalized-name uniqueness is enforced through a transaction advisory lock rather than trusting the browser.

### Swiss

A fixed round count, normal scoring and Buchholz standings. Pairing prefers avoiding rematches, then minimizing total absolute initial-rating difference, then score gap, with deterministic color allocation and odd-player byes.

The complete-matching search is designed for small fields; its worst-case search cost is not a validated large-tournament solution.

### Winners Advance

Stages are automatic. Decisive winners and bye recipients advance; a bye does not count as a played win.

A first draw schedules a same-pair replay with reversed colors. After their second draw, both players remain eligible but cannot play each other again. The event completes when one survivor remains or no legal pairing is possible.

Placement follows advancement/elimination stage. Wins break ties among final survivors only; equal final wins share rank/champion status. Creator corrections require authority, a reason and an audit row, and cannot retroactively invalidate an already-advanced stage. Saved replay content remains the actual played game.

## 7. Authentication boundary

The server issues its own application tokens for both password and Google identity. Supabase's Google JWT is verified through ES256 public JWKS and exchanged for an application session; it is not a gameplay token.

Production-sensitive auth uses a single-use ML-KEM/X25519 sealed payload authenticated by a pinned ML-DSA server identity. This complements HTTPS/WSS; it does not encrypt every gameplay message or remove the need for TLS.

The browser never needs the database URI, application signing key or private server identity. See [Security](SECURITY.md) for trust, storage, RLS, failure behavior and rotation.

## 8. Browser architecture

The client is static HTML/CSS and vanilla JavaScript modules, with a shared Canvas renderer and SVG piece themes. Routes/screens compose transport, session, store and reusable UI helpers.

Ordinary games, tournament rooms, watch, replay and puzzle views reuse board infrastructure. Request correlation isolates replies; screen teardown removes listeners. Signed-out state is distinct from pending restoration or a temporary network/database outage.

The [frontend implementation plan](../frontend_implementation_plan.md) separates delivered release features from optional mobile/community/accessibility work.

## 9. Deployment boundary and limitations

The intended release runs one backend instance with an external PostgreSQL database and a separately hosted static frontend.

- Rooms, seats, spectator sets and short-lived capabilities are process-local.
- A process restart preserves saved data but does not transparently restore an in-progress board.
- Multiple instances cannot safely share live games without routing/shared-state work.
- Minimum zero instances permits cold starts; it does not guarantee zero cost.
- One-CPU/512 MiB local container smoke proves startup/operation under that restriction, not a sustained player limit.
- Authentication hashing, engine jobs, database network waits and polling frequency compete for resources.

Capacity must be measured with the deployed workload. [Benchmarks](BENCHMARKS.md) distinguishes local burst results, hosted read latency and missing production evidence.
