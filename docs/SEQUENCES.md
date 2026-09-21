# Sequence Diagrams — LLD-7 documentation deliverable

Four flows the plan §LLD-7 asked to draw explicitly, showing lock
boundaries and DB commit points. Text-based (ASCII); a picture would
add nothing over what the arrows convey.

For the containing shapes and dependency directions see
[ARCHITECTURE.md §7](ARCHITECTURE.md#7-the-lld-refactor-2026-09-lld-0-through-lld-7).

Notation:

* `━━━` a call that RETURNS to the caller before continuing
* `─▶` a fire-and-forget or a downstream call whose response is not
  awaited by this arrow
* `[lock: X]` a critical section holding lock `X`
* `⋯` boundary between synchronous processing and a later event
* `┃` process-wide continuation of the same actor

---

## 1. Move acceptance / rejection

```
Client       Router        Pipeline           GameplayService     GameRoom
  │            │              │                     │                │
  │  frame     │              │                     │                │
  ├──ws──────▶│              │                     │                │
  │            │  route(fd,   │                     │                │
  │            │   type)      │                     │                │
  │            ├────────────▶│                     │                │
  │            │              │ SealOpen (nop for   │                │
  │            │              │   make_move)        │                │
  │            │              │ ParseJson           │                │
  │            │              │ (no auth on         │                │
  │            │              │   make_move — seat  │                │
  │            │              │   is implied by fd) │                │
  │            │              │ decode_make_move    │                │
  │            │              │                     │                │
  │            │              │ make_move(ctx, req, sink)             │
  │            │              ├───────────────────▶│                │
  │            │              │                     │  submit_move   │
  │            │              │                     ├──────────────▶│
  │            │              │                     │                │ [lock: room mutex_]
  │            │              │                     │                │  revision_++
  │            │              │                     │                │  check turn / clock
  │            │              │                     │                │  make_move (chess/board.cpp)
  │            │              │                     │                │  switch Fischer clock
  │            │              │                     │                │  status = ONGOING?
  │            │              │                     │                │    │
  │            │              │                     │                │    ├─ yes: return MoveResult{ok}
  │            │              │                     │                │    └─ terminal: build_snapshot_locked
  │            │              │                     │                │       queue GameCompleted
  │            │              │                     │                │ [unlock] + LockAndDrain fires
  │            │              │                     │                │   on_game_completed on listeners
  │            │              │                     │◀───────────────┤   (GameCompletionService, see §2)
  │            │              │                     │                │
  │            │              │  encode move_made   │                │
  │            │              │◀────────────────────┤                │
  │◀───ws──────┤              │  caller_sink.send   │                │
  │            │              │  send to opponent   │                │
  │◀───ws──────┤              │  spec broadcast     │                │
  │            │              │                     │                │
  │            │              │  (game_over frame   │                │
  │            │              │   if terminal)      │                │
  │◀───ws──────┤              │                     │                │

REJECTION variant:
  If Board::make_move refuses (not legal), the GameRoom
  returns MoveResult{success=false, error="..."} still under
  its lock; the service maps to a move_rejected frame and
  only the caller sees it. No opponent broadcast, no
  spectator broadcast, no completion event.
```

Lock scope: room mutex covers only `submit_move` itself. The caller
sink and any foreign-fd sends happen after `LockAndDrain` releases.
No listener callback runs with the room mutex held.

---

## 2. Game completion with failed save + retry

```
GameRoom            Listener list       GameCompletionService     PostgresGameStore     Postgres
  │ (see §1)           │                        │                        │                  │
  │ finish_game        │                        │                        │                  │
  │ build_snapshot_    │                        │                        │                  │
  │  locked            │                        │                        │                  │
  │   (stamps          │                        │                        │                  │
  │    completion_uuid │                        │                        │                  │
  │    + revision)     │                        │                        │                  │
  │ queue GameCompleted│                        │                        │                  │
  │ [unlock]           │                        │                        │                  │
  │                    │                        │                        │                  │
  │  on_game_completed(ev) — snapshot by const& │                        │                  │
  ├───────────────────▶├──────────────────────▶│                        │                  │
  │                    │                        │ ai / unauth / !capable │                  │
  │                    │                        │   short-circuit        │                  │
  │                    │                        │                        │                  │
  │                    │                        │ save_completed_game(   │                  │
  │                    │                        │   snapshot)            │                  │
  │                    │                        ├──────────────────────▶│                  │
  │                    │                        │                        │ SELECT id        │
  │                    │                        │                        │ WHERE            │
  │                    │                        │                        │ completion_uuid  │
  │                    │                        │                        │  = $1            │
  │                    │                        │                        ├────────────────▶│
  │                    │                        │                        │◀────────────────┤ empty
  │                    │                        │                        │                  │
  │                    │                        │                        │ BEGIN            │
  │                    │                        │                        │ INSERT games,    │
  │                    │                        │                        │        move_times│
  │                    │                        │                        │ UPDATE players   │
  │                    │                        │                        │  (ELO+stats)     │
  │                    │                        │                        │                  │ ─ ✗ DB down ─
  │                    │                        │                        │◀────────────────┤ ROLLBACK
  │                    │                        │                        │ StorageError::   │
  │                    │                        │                        │  Disconnected    │
  │                    │                        │                        │                  │
  │                    │                        │◀───────────────────────┤                  │
  │                    │                        │ log line names typed   │                  │
  │                    │                        │  StorageError code     │                  │
  │                    │                        │  (LLD-3.1 classify)    │                  │
  │                    │                        │                        │                  │
  │  Game stays FINISHED in-memory. Client already saw game_over.        │                  │
  │  No automatic retry today — that would require a durable outbox     │                  │
  │  (plan §6.1: "no promise of crash-durable retries without           │                  │
  │  durable storage/outbox").                                          │                  │
                                                                                     ⋯

RETRY BY OPERATOR (later):
  Second call to save_completed_game with the SAME snapshot.
  Idempotency plays out here:
  ┃                                                                     │                  │
  ┃                                                                     │ SELECT id        │
  ┃                                                                     │ WHERE            │
  ┃                                                                     │ completion_uuid  │
  ┃                                                                     ├────────────────▶│
  ┃  first run  ─ empty ─ INSERT proceeds ─ COMMIT ─────────────────────┤◀────────── row  │
  ┃  second run ─ hit ─ short-circuit ────────────────────────────────▶│ already_persisted│
  ┃                                                                     │                  │
  ┃  Concurrent race: two workers with the same uuid both pass SELECT,  │                  │
  ┃  one wins INSERT, the loser hits the partial-unique-index violation │                  │
  ┃  on completion_uuid, catches UniqueViolation, re-runs SELECT which  │                  │
  ┃  now hits, returns already_persisted=true. Net: one row, one ELO    │                  │
  ┃  update — regardless of order.                                      │                  │
```

Persistence commit is the DB `COMMIT`. Between snapshot construction
and that commit, the game is durable *in memory* only. This is
documented in the log as an explicit deferral of durable-outbox
work.

---

## 3. Sealed login

```
Client         TcpServer/Router      Pipeline         SealedRegistry     AuthHandler        Postgres
  │                 │                   │                   │                 │                │
  │ 1. seal_request │                   │                   │                 │                │
  ├──ws───────────▶│                   │                   │                 │                │
  │                 │ route→pipeline    │                   │                 │                │
  │                 ├──────────────────▶│                   │                 │                │
  │                 │                   │ SealOpen (nop —   │                 │                │
  │                 │                   │  seal_request     │                 │                │
  │                 │                   │  is not sealed)   │                 │                │
  │                 │                   │ raw route:        │                 │                │
  │                 │                   │  handle_seal_req  │                 │                │
  │                 │                   ├──────────────────────────────────▶│                │
  │                 │                   │                   │                 │ rate-limit    │
  │                 │                   │                   │                 │  seal-req/IP  │
  │                 │                   │                   │◀────────────────┤ handle_seal_  │
  │                 │                   │                   │                 │  request(ip)  │
  │                 │                   │                   │ mint fresh      │                │
  │                 │                   │                   │  ML-KEM-768 KP  │                │
  │                 │                   │                   │  + X25519 KP    │                │
  │                 │                   │                   │  + ML-DSA-65    │                │
  │                 │                   │                   │  signature over │                │
  │                 │                   │                   │  the offer      │                │
  │                 │                   │                   │  bytes          │                │
  │                 │                   │                   │  store one-time │                │
  │                 │                   │                   │  key by key_id  │                │
  │                 │                   │                   │◀ reply JSON ────┤                │
  │◀ seal_key ──────┤                   │                   │                 │                │
  │  (contains      │                   │                   │                 │                │
  │   master_b64,   │                   │                   │                 │                │
  │   expires_in,   │                   │                   │                 │                │
  │   offer_sig,    │                   │                   │                 │                │
  │   identity_pk)  │                   │                   │                 │                │
  │                 │                   │                   │                 │                │
  │ 2. compose:     │                   │                   │                 │                │
  │  * verify_offer_signature(offer, identity_pk)             │                 │                │
  │  * derive AEAD key from master_b64                        │                 │                │
  │  * AES-CTR-256 + HMAC-SHA-384 over                        │                 │                │
  │    {type:"login", username, password}                     │                 │                │
  │  * wrap into sealed envelope with key_id                  │                 │                │
  │                 │                   │                   │                 │                │
  │ 3. sealed login │                   │                   │                 │                │
  ├──ws───────────▶│                   │                   │                 │                │
  │                 │ route→pipeline    │                   │                 │                │
  │                 ├──────────────────▶│                   │                 │                │
  │                 │                   │ SealOpen calls    │                 │                │
  │                 │                   │ inspect(type,     │                 │                │
  │                 │                   │  raw)             │                 │                │
  │                 │                   ├──────────────────▶│                 │                │
  │                 │                   │                   │ look up key_id  │                │
  │                 │                   │                   │ (consume once)  │                │
  │                 │                   │                   │ decrypt AEAD    │                │
  │                 │                   │                   │ check HMAC      │                │
  │                 │                   │                   │ Outcome::Opened │                │
  │                 │                   │◀──────────────────┤ plaintext:      │                │
  │                 │                   │                   │  {type:"login", │                │
  │                 │                   │                   │   username,     │                │
  │                 │                   │                   │   password}     │                │
  │                 │                   │ raw route:        │                 │                │
  │                 │                   │  handle_login     │                 │                │
  │                 │                   ├───────────────────────────────────▶│                │
  │                 │                   │                   │                 │ rate-limit    │
  │                 │                   │                   │                 │  login/IP     │
  │                 │                   │                   │                 │  parse JSON   │
  │                 │                   │                   │                 │  rate-limit   │
  │                 │                   │                   │                 │   login/acct  │
  │                 │                   │                   │                 │  authenticate │
  │                 │                   │                   │                 ├──────────────▶│
  │                 │                   │                   │                 │ Argon2id      │
  │                 │                   │                   │                 │  verify       │
  │                 │                   │                   │                 │  (dummy hash  │
  │                 │                   │                   │                 │   defence     │
  │                 │                   │                   │                 │   on miss)    │
  │                 │                   │                   │                 │◀─ok/no ──────│
  │                 │                   │                   │                 │ issue_session │
  │                 │                   │                   │                 │  → JWT access │
  │                 │                   │                   │                 │  + opaque     │
  │                 │                   │                   │                 │    refresh    │
  │                 │                   │                   │                 ├──────────────▶│
  │                 │                   │                   │                 │◀─ ok ────────│
  │◀ auth_ok ───────┤                   │                   │                 │                │
  │   username, elo, access_token, refresh_token             │                 │                │
  │                                                                                            │
  │  If any step failed → auth_error{code: invalid_credentials|rate_limited|internal}.        │
  │  If the message arrived UNSEALED → SealedRegistry::Outcome::Rejected                     │
  │  → pipeline drops silently, handle_login never runs.                                     │
```

The three pieces that make this secure:

1. `require_sealed("login")` at startup — an attacker who strips the
   envelope hits `Rejected`, not `Continue`.
2. The one-time key store — each `seal_key` reply mints fresh KPs;
   the key id is consumed on first successful open.
3. ML-DSA-65 signature over the offer bytes — the client can prove
   the reply came from the server identity key on disk before
   trusting it enough to bind a password to it.

---

## 4. Disconnect + revision-guarded stale result (design)

The current AI move path is synchronous — no stale results happen
today. This diagram is the design LLD-6.4's revision counter
supports if a future slice extracts the search into a bounded
executor.

```
Worker A            GameRoom             Executor pool     Worker B (executor)
  │                    │                       │                   │
  │ submit_move        │                       │                   │
  ├──────────────────▶│                       │                   │
  │                    │ [lock: room mutex_]   │                   │
  │                    │  revision_ = R0       │                   │
  │                    │  human move accepted  │                   │
  │                    │  build snapshot S     │                   │
  │                    │   (S.revision = R0)   │                   │
  │                    │ [unlock]              │                   │
  │◀───────────────────┤                       │                   │
  │                                                                │
  │  submit AI job (S)                                             │
  ├──────────────────────────────────────────▶│                   │
  │                                            │ dequeue           │
  │                                            ├──────────────────▶│
  │                                            │                   │ SearchLimits from
  │                                            │                   │  difficulty
  │                                            │                   │ EngineMoveSelector.select(
  │                                            │                   │   S.board, limits)
  │                                            │                   │  (isolated Engine,
  │                                            │                   │   its own TT)
  │                                            │                   │
Meanwhile:                                                          │
  │ resign(fd)         │                       │                   │
  ├──────────────────▶│                       │                   │
  │                    │ [lock]                │                   │
  │                    │  revision_++          │                   │  ⋯ search still running
  │                    │  = R1                 │                   │
  │                    │  finish_game(         │                   │
  │                    │    RESIGNATION)       │                   │
  │                    │  build & fire         │                   │
  │                    │   GameCompleted       │                   │
  │                    │ [unlock]              │                   │
  │◀───────────────────┤                       │                   │
  │                                                                │
  │                                            │◀──────────────────┤ MoveChoice{from,to,promo}
  │                                            │                   │
Worker A picks up the result:                                       │
  │                    │                       │                   │
  │ result.snapshot.revision == R0                                  │
  │ room->revision()   == R1                                        │
  │ ⇒ STALE, DISCARD                                                │
  │                    │                       │                   │
  │  (log line;                                                     │
  │   no submit_move_ai;                                            │
  │   no client frame)                                              │
```

Three preconditions for this design to be correct — all in place
today:

1. **Immutable snapshot.** `GameSnapshot` is a value type; the
   executor thread never re-reads the room.
2. **Isolated engine state per job.** `EngineMoveSelector` (LLD-6.2)
   owns its own `Engine` with private TT. One job per selector.
3. **Room revision at ingest and dispatch.** `GameSnapshot.revision`
   pinned under the room mutex at snapshot build; `room->revision()`
   is an atomic acquire — Worker A can compare without taking the
   room lock.

The plan §6.4 also lists: bounded queue depth for the executor,
admission limits, cancellation on room-state-change, shutdown drain
that discards pending jobs without applying them. These are the
work items an async-executor slice would add on top of the current
groundwork; none of them are needed under the shipping serialized
path.
