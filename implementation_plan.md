# Multiplayer Chess Platform — Implementation Plan

A release roadmap for a C++17 multiplayer chess platform with a static browser client, Supabase PostgreSQL persistence and authenticated real-time play.

**Status:** core implementation and the reported local tournament acceptance campaign are complete. Container validation is complete locally. Deployment and production-only validation are still pending.

## Reading guide

- [Frontend implementation plan](frontend_implementation_plan.md): browser architecture, delivery sequence and future UX work.
- [Architecture](docs/ARCHITECTURE.md): boundaries, concurrency and deployment limitations.
- [Protocol](docs/PROTOCOL.md): messages and authorization.
- [Security](docs/SECURITY.md): authentication, secrets and operational controls.
- [Benchmarks](docs/BENCHMARKS.md): measured performance and its limitations.

Checked items describe implemented or verified work, not a promise of production capacity. Unchecked items are outstanding release gates or explicitly deferred work.

## Delivery overview

| Phase | Scope | State |
| --- | --- | --- |
| 1 | Foundation and TCP transport | Complete |
| 2 | WebSocket protocol and dispatch | Complete |
| 3 | Chess rules and notation | Complete |
| 4 | Multiplayer rooms, clocks and matchmaking | Complete |
| 5 | Browser application | Release features complete; growth backlog separate |
| 6 | Chess engine and analysis | Complete for current scope |
| 7 | Authentication and security | Implemented; production configuration validation pending |
| 8 | Durable storage and ratings | Complete |
| 9 | Spectating, replays and live tournaments | Complete for current release scope |
| 10 | Automated verification, benchmarks and packaging | Local work complete; cloud validation pending |

## Architecture and implementation principles

The backend remains a modular monolith. Transport delivers requests to a structural JSON protocol boundary; application services orchestrate domain objects through narrow ports. PostgreSQL, sockets, crypto and clocks are adapters, not dependencies of the chess rules.

- Server authority determines legal moves, clocks, seats, results and tournament advancement.
- Ordinary games and tournament games use the same room, board, clock and player controls.
- Mutable room state is guarded by a per-room mutex; listener delivery occurs after unlocking.
- Blocking database work must not hold the transport-wide connection lock.
- Every transaction owns its database connection until commit or rollback.
- Persistent game identity and live room identity are distinct.
- Missing data is unavailable or empty, never fabricated production content.
- Complexity and capacity claims require measurements.

## Phase 1 — Foundation and TCP transport

### Deliverables

- [x] C++17 project and CMake targets with strict compiler warnings.
- [x] Core types, explicit result handling and thread-safe logging.
- [x] POSIX TCP sockets, connection buffers and epoll event loop.
- [x] Worker pool with mutex/condition-variable task queue.
- [x] RAII ownership for sockets and domain objects.
- [x] Bounded worker configuration rather than an unbounded host CPU count.
- [x] Signal-safe shutdown request and ordinary-thread resource cleanup.

### Acceptance

- [x] Connections can be created, serviced and closed without stale-handle reuse.
- [x] Slow or blocked durable maintenance does not prevent another client's handshake.
- [x] Local shutdown tests cover traffic and blocked database work.

**Design decision:** the queue uses conventional synchronization; a lock-free implementation is not required for this release.

## Phase 2 — WebSocket protocol and request pipeline

### Deliverables

- [x] HTTP upgrade and RFC 6455 framing, masking and control frames.
- [x] JSON text messages with bounded input and output buffering.
- [x] RequestPipeline with route policies, seal inspection and identity extraction.
- [x] Typed request codecs and response serialization.
- [x] Generation-guarded message delivery and stale connection rejection.
- [x] Redacted diagnostics for sensitive or unknown frames.
- [x] Frontend request correlation and isolation of unrelated replies/errors.

### Acceptance

- [x] Malformed requests fail safely without crashing the server.
- [x] Unauthorized routes fail without granting seats or mutating results.
- [x] Concurrent requests cannot consume another request's reply.
- [x] Temporary identity-read failure is retryable, not proof of logout.

**Design decision:** JSON is the shipping wire format. A separate binary move protocol is outside this release.

## Phase 3 — Chess rules and notation

### Deliverables

- [x] Mailbox board, FEN parsing/serialization and move/undo state.
- [x] Pseudo-legal and legal move generation.
- [x] Castling, en passant and promotion.
- [x] Check, checkmate, stalemate and supported draw conditions.
- [x] SAN notation and replayable move history.
- [x] Zobrist position hashing and repetition tracking.

### Acceptance

- [x] Perft and rule regressions cover published reference positions.
- [x] Illegal moves do not change position, clocks or history.
- [x] Move/undo restores complete position state.
- [x] Terminal positions reject further play.

## Phase 4 — Multiplayer gameplay

### Deliverables

- [x] RoomManager and shared GameRoom lifecycle.
- [x] Authenticated create/join, quick-play and AI entry.
- [x] Rating-band matchmaking with widening search.
- [x] Server-authoritative Fischer clocks using monotonic time.
- [x] Resignation, draw offers and ordinary-game rematches.
- [x] Authenticated seat recovery and active-game discovery.
- [x] Player/opponent names and read-only spectator controls.
- [x] Immutable completion snapshots and revision guards for engine work.

### Acceptance

- [x] Only the correct player can move on the correct turn.
- [x] Missing seats do not start playable tournament clocks.
- [x] Draw/rematch expiry and duplicate responses do not create duplicate outcomes.
- [x] Tournament games cannot bypass their schedule through ordinary join controls.

## Phase 5 — Browser application

### Deliverables

- [x] Static HTML/CSS and vanilla JavaScript ES modules.
- [x] Shared design tokens, responsive layouts and reusable UI primitives.
- [x] Canvas board with SVG piece assets, drag and tap-to-move.
- [x] Hash router, screen mount/unmount cleanup and meaningful deep links.
- [x] WebSocket reconnect and session restoration lifecycle.
- [x] Landing, lobby, game, authentication, profile and leaderboard.
- [x] Watch, replay, puzzle, settings and tournament screens.
- [x] Invite links, PGN/result sharing, named bot choices and local streaks.
- [x] Accessible form labels, focused error summaries and modal focus handling.
- [x] Google sign-in button with official icon and configuration/busy feedback.

### Acceptance

- [x] Reported local game/tournament journeys pass their acceptance sets.
- [x] Automated frontend suites cover sessions, gameplay, tournaments and auth transport.
- [x] Empty/error states reflect real backend availability.
- [ ] Complete board keyboard play and screen-reader position/move support as a follow-on accessibility improvement.
- [ ] Production browser verification after real origins and secrets are installed.

The detailed browser sequence and remaining growth work are in the [frontend plan](frontend_implementation_plan.md).

## Phase 6 — Engine and analysis

### Deliverables

- [x] Evaluation, alpha-beta search and iterative deepening.
- [x] Zobrist transposition table and move ordering.
- [x] Quiescence search, including check evasions.
- [x] Difficulty/depth/time limits for AI play.
- [x] Search snapshots separate from mutable live-room state.
- [x] On-demand position analysis and replay integration.
- [x] Engine and move-selection regression tests.

### Boundaries

Engine strength is not a certified rating. Bot labels are product choices mapped to search limits. Historical engine timings must not be presented as a fresh measurement of the current search implementation.

- [ ] Rebenchmark the current engine before publishing updated NPS or strength claims.
- [ ] Consider piece lists, additional pruning or an opening book only after profiling and targeted rule regressions.

## Phase 7 — Authentication and security

### 7.1 Accounts and sessions

- [x] Username validation and case-insensitive uniqueness.
- [x] Argon2id password hashing through OpenSSL.
- [x] HMAC-SHA-384 application access tokens.
- [x] Random opaque refresh tokens; store only their SHA-384 hashes.
- [x] Transactional refresh rotation, family revocation and logout-all token epochs.
- [x] Migration 0011 rotation timestamps and bounded refresh recovery.
- [x] Client restoration distinguishes connection trouble from definitive authentication failure.
- [x] Rate limiting and non-enumerating credential errors.
- [x] Verified-email activation and single-use password recovery with transactional session revocation.
- [x] Legacy password accounts can add a recovery email with current-password proof.
- [x] Bounded Gmail SMTP worker with mandatory STARTTLS and secret-backed configuration.
- [ ] Apply migration 0015, configure SMTP on the hosted revision and verify real email delivery.

### 7.2 Google sign-in

- [x] Supabase OAuth redirect flow.
- [x] ES256/P-256 validation against verified HTTPS public JWKS.
- [x] Issuer, audience, provider, identity and token-claim checks.
- [x] Bounded key cache, fetch timeout, response size and refresh cooldown.
- [x] No automatic downgrade from ES256 to legacy HS256.
- [x] Stable Google identity mapping; reuse a password account only when both stored and Google email ownership are verified.
- [x] Local Google login, repeat login, refresh and sign-out checks reported passed.

Supabase private signing keys are not required. Its project URL and public key information are configuration, not application secrets.

### 7.3 Sealed authentication

- [x] ML-KEM-768 + X25519 hybrid key agreement.
- [x] HKDF and AES-256-CTR + HMAC-SHA-384 authenticated payload sealing.
- [x] ML-DSA-65 signed server offers with a public frontend identity pin.
- [x] Single-use offers with expiry and per-IP/global limits.
- [x] Authenticated inner action rejects action relabeling and envelope replay.
- [x] Browser/C++ interoperability using a pinned, locally vendored crypto bundle.
- [x] No plaintext fallback after trust, signature, provider or sealing failure.
- [x] Production-required sealing fails startup when auth or identity configuration is missing/invalid.
- [x] Raw private identity files and strict marked base64 text files supported.
- [x] Text conversion preserves the original identity and refuses overwriting output.

The text wrapper is encoding, not encryption. TLS remains necessary; refresh and gameplay traffic are not transformed into a persistent post-quantum channel.

### 7.4 Database, browser and operational hardening

- [x] Parameterized SQL and transaction-bound persistence.
- [x] Migration 0014: backend-table default-deny RLS and browser-role privilege revocation.
- [x] Pinned username-trigger search path and six supporting foreign-key indexes.
- [x] Trusted backend access preserved without permissive browser policies.
- [x] Bounded identity-read pool with exclusive leases and transaction-safe cleanup.
- [x] Logs redact credentials and attacker-controlled unknown payloads.
- [x] CSP/security headers and production identity-pin policy.
- [x] Non-root container and secret-free build context.
- [x] Private identity, JWT key and database URI excluded from source and image contents.

### Production gates

- [ ] Install and validate scoped runtime secret access in the deployed revision.
- [ ] Replace backend URL/CSP placeholders with real HTTPS/WSS origins.
- [ ] Configure production OAuth origins and redirect allowlists.
- [ ] Verify production refresh/session stability beyond the access-token lifetime.
- [ ] Rehearse key rotation and cold-start/reconnect behavior in staging.

Security implementation is complete for the release scope; these are environment-specific checks, not claims that deployment has already happened. Operational details are in [Security](docs/SECURITY.md).

## Phase 8 — Persistence and ratings

### Deliverables

- [x] Supabase PostgreSQL through libpq.
- [x] Schema migrations and repository adapters.
- [x] Persisted players, sessions, completed games and move timings.
- [x] Atomic result/stat/rating writes.
- [x] Completion identity preventing duplicate durable saves.
- [x] Indexed profile/history/leaderboard queries.
- [x] Failure reporting and retry behavior without pretending a failed save succeeded.

### Connection ownership

- [x] Dedicated guarded connections retained for auth writes, tournament transactions and game persistence.
- [x] Fresh profile/epoch reads can borrow bounded independent connections.
- [x] Checkout has a timeout; abandoned transactions are rolled back on return.
- [x] Broken sessions are quarantined; failed queries are not blindly replayed.

Default identity-read pool size is two, giving five database connections per process including the three dedicated sessions. Maximum configured pool size is four, giving seven. This is narrow I/O concurrency, not a rewrite of every repository.

## Phase 9 — Replays, spectating and tournaments

### 9.1 Shared game features

- [x] Live-game directory and authenticated read-only spectating.
- [x] Saved-game replay with original players, moves, result and termination.
- [x] On-demand analysis and statistical anti-cheat reports.
- [x] Anti-cheat findings are review signals, not automatic bans.

### 9.2 Scheduled tournament lifecycle

- [x] Creator form, validation, local-time preview and tournament deep links.
- [x] Unique normalized tournament names.
- [x] Registration/unregistration and creator-controlled close/reopen.
- [x] Registration automatically closes at the deadline or pairing lock, whichever occurs first.
- [x] Pairings appear 90 seconds before the first scheduled round.
- [x] Reopening is denied after registration/pairing lock.
- [x] Early check-in reserves seats but cannot start play before scheduled time.
- [x] Concurrent participant check-ins succeed independently.
- [x] Shared ordinary-game controls, names, clocks and rules in tournament rooms.
- [x] Automatic checkmate, timeout, resignation and agreed-draw results.
- [x] Single/double no-shows resolve as forfeits, not fabricated draws.
- [x] Unregistered and eliminated users can spectate without acquiring seats.
- [x] Next stages wait until every current pairing resolves.
- [x] Durable replay identity is separate from live room identity.
- [x] Byes and unplayed forfeits do not link to unrelated replay games.
- [x] Creator corrections require authority, reason and audit history; saved game content is not rewritten.

### 9.3 Swiss format

- [x] Creator selects a fixed number of rounds.
- [x] Pairing preference avoids repeat opponents first, then minimizes total initial-rating difference, then score gap.
- [x] Deterministic color allocation and standard odd-player byes.
- [x] Win/draw/loss scoring and Buchholz standings.
- [x] No-show, multi-round, correction, completion and replay acceptance sets reported passed.

### 9.4 Winners Advance format

- [x] Automatic stages; the Swiss round-count field is hidden.
- [x] Decisive winners and bye recipients advance; losers are eliminated.
- [x] A bye is not a played win and never assigns an AI opponent.
- [x] First draw schedules a same-pair replay with reversed colors.
- [x] After two draws between the same players, both remain eligible to advance; that pair cannot play again.
- [x] If no legal pairing remains, finish without an infinite extra stage.
- [x] Placement follows advancement/elimination stage, not a global wins leaderboard.
- [x] Wins break ties only among the final survivors; equal final wins share placement/champion status.
- [x] Correction cannot retrospectively invalidate an already-advanced stage.
- [x] Decisive final, two-draw completion, odd-player bye and four-player progression acceptance sets reported passed.

### 9.5 Read-path responsiveness

- [x] Coherent tournament SQL snapshots and short per-event read coalescing.
- [x] Lifecycle work separated from transport maintenance.
- [x] Blocking SQL kept outside broad transport critical sections.
- [x] Identity reads overlap through the bounded pool.
- [x] Backend warnings expose slow request durations for diagnosis.

The exact small-field pairing search is not a verified large-field algorithm. Hosted database latency remains a capacity constraint.

## Phase 10 — Verification, performance and packaging

### Local verification

- [x] Rule, engine, crypto, authentication, storage and service regression suites.
- [x] Frontend non-UI tests for session restoration, request isolation and tournament lifecycle.
- [x] Pool lease/timeout/transaction/recovery tests.
- [x] Migration role-denial/search-path/index checks against a disposable database.
- [x] Sealed browser/C++ transport tests with synthetic identities.
- [x] User-reported local tournament acceptance campaign, including checkmate and timeout.
- [x] Short mixed move/tournament-read benchmarks and hosted read-only pool measurements.
- [x] Non-root multi-stage Docker Release image.
- [x] Local one-CPU/512 MiB restricted container smoke and graceful shutdown.
- [x] Formal architecture, protocol, security, sequence and benchmark documentation.

### What remains before calling the deployed release verified

- [ ] Publish the tested image and deploy the backend manually through the chosen cloud console.
- [ ] Publish the static frontend with production configuration.
- [ ] Verify deployed health, WSS upgrade, password/Google login and persistence.
- [ ] Run a focused production session/cold-start/reconnect soak.
- [ ] Measure a warm single-instance workload with the real network/database path.
- [ ] Confirm usage/cost monitoring and a rollback procedure.

Local benchmark success does not guarantee any particular simultaneous player count or a zero-cost cloud bill. Live rooms are process-local: persisted results survive restart, but in-progress boards are not transparently restored across a backend restart or distributed across instances.

## Major changes beyond the original plan

This section summarizes the substantial additions and revised design decisions after the initial implementation.

1. **Application boundaries:** narrow services and ports replaced oversized transport/game handlers; request decoding and delivery are explicit adapters.
2. **Shared live tournaments:** scheduled rooms reuse ordinary gameplay instead of introducing separate move/clock/control implementations.
3. **Winners Advance:** automatic elimination stages, reversed-color draw replay, a two-draw pair cap and advancement-based placement complement fixed-round Swiss.
4. **Durable tournament identity:** live room IDs and saved replay IDs are separate, with audited corrections and no unplayed-game replay links.
5. **Responsive release flows:** registration locks, simultaneous check-in, missing-seat waits, request correlation and bounded state-read coalescing.
6. **Database concurrency:** Supabase retained; a bounded, transaction-safe identity-read pool overlaps network waits while writes keep their existing guards.
7. **Security hardening:** ES256 public-key Google verification, browser sealed authentication, strict text-safe identity loading and backend-only database access are implemented in Phase 7.
8. **Release packaging:** non-root Docker, constrained local runtime checks, public benchmark methodology and separate deployment validation gates.
9. **Frontend expansion:** landing/invite/puzzle/bot journeys and live release screens are consolidated in the root frontend plan; unfinished growth/accessibility work is explicitly separated.
10. **Verified email recovery:** username/email-only registration, mailbox-owner password setup followed by automatic sealed login, new-password complexity validation, expiring reset links, session revocation, legacy recovery-email enrollment and verified Google/password account reuse. Hosted rollout and real SMTP delivery remain a separate acceptance gate.
