# Chess Platform — LLD Refactoring Implementation Plan

Date: 2026-09-19. Status: **proposal only; no application refactoring performed**.

This plan is based on `implementation_plan.md`, `docs/implementation_log.md`, the current source, tests, and architecture documentation. It complements the existing `lld_refactor_plan.md`, which is preserved. Paths below are relative to the repository root; proposed paths are explicitly marked new.

## 1. Outcome and scope

Turn the working platform into a maintainable modular monolith: small request adapters, cohesive application services, explicit infrastructure boundaries, and a fast value-oriented chess domain. Apply patterns where they solve an existing coupling, lifecycle, or testing problem—not to collect pattern names.

**Recommended core patterns:** Adapter, Facade, Command, Observer, Chain of Responsibility, and narrow Repository ports. Use ordinary composition and constructor injection throughout. Add Strategy, Decorator, Builder, and Null Object only at the specific boundaries described below.

Do not introduce microservices, Redis, Kafka, a DI framework, a new frontend framework, a virtual Piece hierarchy, new chess variants, or new cryptography. Do not change rating policy, authentication requirements, wire contracts, tournament behavior, or deployment topology silently. A safety fix discovered during refactoring must be identified and tested separately from a behavior-preserving extraction.

Proceed one approved phase at a time. All UI-interaction testing belongs to Aditya; automated non-UI tests remain part of our work.

## 2. Source-grounded baseline

| Area | What exists | Design consequence |
|---|---|---|
| Request handling | `src/game/game_handler.h/.cpp` combines gameplay, matchmaking, persistence, profile/leaderboard, spectating, replay, analysis, and tournaments | Highest-value SRP split; retain compatibility registration while extracting |
| Dependency wiring | `src/main.cpp` constructs services and injects database, signer, and connection lookup through setters | Keep one composition root; make mandatory dependencies explicit without turning every concrete type into an interface |
| Domain | `src/game/game_room.h/.cpp` protects state with a room mutex; `src/chess/*` uses board/move values | Keep domain state and legal transitions together; extract coherent immutable snapshots, not dozens of independently locked reads |
| Transport | `src/net/tcp_server.cpp::handle_client_data` holds `connections_mutex_` through handling | Synchronous DB/search work can delay unrelated connections. Do not remove this lock before replacing its connection-lifetime and serialization guarantees |
| Routing | `src/net/websocket.cpp::MessageRouter::route` scans strings to find JSON `type` and applies a pre-dispatch seal hook | Introduce structural parsing and explicit request context; preserve sealed-message security semantics |
| Persistence | `src/storage/game_repo.cpp::save_completed_game` performs game insert, timing insert, and ratings/stat updates in one SQL transaction | Preserve that atomic unit; generic CRUD repositories must not split it |
| Database ownership | `src/storage/database.h` owns one `PGconn`; public calls do not expose transaction ownership/synchronization | Audit all callers before increasing concurrency. Server-side pooling does not make one client connection concurrently usable |
| Completion | `GameHandler::persist_game` assembles data via separate getters and logs save failure; it has no persistence-idempotency argument | Add completion snapshot, operation identity, retry semantics, and durable duplicate protection before automatic retries |
| Ratings | Game save computes from supplied rating snapshots; SQL assigns absolute ELO but increments stats relatively | Snapshot-rating semantics are an explicit policy. Concurrent games involving the same player need a documented rule; relative stat updates do not solve stale rating overwrites |
| AI | `src/game/ai_player.h` documents a blocking, non-thread-safe owned engine; handler owns an AI player | Scope engine ownership explicitly before enabling parallel requests/jobs |
| Frontend | `frontend/js/core/events.js` already implements pub/sub; screens/net/core/board are separate modules | Strengthen subscription ownership and protocol boundaries; do not replace the frontend wholesale |

### Completion claims to reconcile, not assume

The implementation log records work through Phase 9.4. Its tournament implementation uses creator-reported results and does not yet automatically create live rooms for pairings. Anti-cheat analysis is on demand. These are **not already-running game-completion observers**. Automated tournament play and automatic anti-cheat scheduling are optional feature projects after this refactor.

Phase 10 documentation/benchmark files are present, including untracked work, while the original plan still contains unchecked items. Reconcile documentation with verified behavior in the baseline phase; do not interpret checkboxes alone as proof of missing or completed code.

The log reports two repository index/planner-test failures (`test_repositories`, 26/28), and a time-sensitive release engine assertion is noted in project material. These are historical reports, not tests freshly executed for this proposal. Establish the current baseline before changing code.

## 3. Target boundaries and ownership

| Boundary | Responsibilities | Must not own |
|---|---|---|
| Transport/protocol | WebSocket framing, bounded parsing, sealed envelope adapter, request/response DTOs, route lookup | SQL, ELO calculations, game orchestration |
| Application | Authorize a use case, coordinate room/repository/search operations, map domain outcomes to responses/events | Raw sockets, libpq types, hand-built JSON |
| Domain | Board legality, game transitions, clock rules, immutable outcomes, pure rating/pairing calculations | Database handles, network connections, token verification |
| Infrastructure | PostgreSQL implementations, socket delivery, real clock, engine executor, operational metrics | Decisions about whether a player is allowed to move |
| Bootstrap | Construct dependencies, register routes/policies/listeners, own shutdown order | Request business logic |

Dependency rule: application uses domain values and narrow ports; infrastructure implements ports. Keep `src/chess/` and `src/game/` rather than moving every file to a new directory in one change.

Proposed modules (create only in their phase):

- `src/protocol/request.h`, `response.h`, `json_codec.h/.cpp`: typed boundary data and codec.
- `src/application/request_context.h`, `result.h`: validated identity, connection handle, request metadata, explicit failures; C++17-compatible types.
- `src/application/ports/message_sink.h`, `game_store.h`, `player_queries.h`, `clock.h`, later `move_selector.h`: small interfaces only where substitution/testing pays for itself.
- `src/application/gameplay_service.h/.cpp`, `game_query_service.h/.cpp`, `analysis_service.h/.cpp`: use-case coordination. Keep tournament algorithms in `src/tournament/`.
- `src/game/handlers/gameplay_handler.h/.cpp`, `query_handler.h/.cpp`, `analysis_handler.h/.cpp`, `tournament_handler.h/.cpp`: protocol-to-use-case adapters.
- `src/game/game_snapshot.h`, `game_events.h`, `src/application/game_completion_service.h/.cpp`: stable transition/completion values and persistence coordination.
- `src/net/connection_handle.h`, `socket_message_sink.h/.cpp`: delivery adapter; resolve handles safely at send time rather than exporting raw connection pointers.
- `src/storage/postgres_game_store.h/.cpp`, `postgres_player_queries.h/.cpp`, `transaction.h/.cpp`: adapters over existing SQL and scoped transaction ownership.
- `src/bootstrap/application.h/.cpp`: explicit object lifetime and registration wiring extracted from `main.cpp` when useful.

No common base class for every service. Use non-owning references for required long-lived dependencies, unique ownership where appropriate, and shared ownership only where lifetime genuinely crosses tasks. Bootstrap destroys/drains jobs and subscriptions before referenced services.

## 4. SOLID and OOP, with deliberate relaxations

| Principle | Application here | Pragmatic limit |
|---|---|---|
| SRP | Separate protocol decoding, gameplay orchestration, SQL, notification delivery, analysis, and tournament endpoints | A cohesive GameRoom can own moves, clocks, and terminal transitions; do not create a class per field |
| OCP | New message registrations, notification subscribers, or move selectors do not require editing unrelated use cases | A small closed enum/switch for chess piece types or game status is fine |
| LSP | Real/fake stores and message sinks obey the same errors, ownership, and delivery contracts | An in-memory fake cannot claim PostgreSQL transaction/locking equivalence |
| ISP | Separate game completion persistence from player queries; inject only what a service needs | Avoid a giant `IRepository` or one-interface-per-method proliferation |
| DIP | Use cases depend on domain values and ports, not `Database`, `Connection`, or JSON strings | Pure helpers and stable concrete domain types need no interface |

Abstraction means exposing `complete_game(snapshot)` rather than SQL details. Encapsulation means legal transitions and snapshots remain under GameRoom control. Polymorphism is useful at infrastructure/test seams. Inheritance is optional: use interface inheritance where needed, not `Pawn : Piece` or deep handler/repository hierarchies.

Keep Board, Move, Piece, move lists, make/unmake, Zobrist keys, and transposition-table operations value-oriented. Do not add virtual dispatch, heap allocation, observers, or general-purpose containers in the per-node search loop without measured justification. No unsupported performance multiplier claims.

## 5. Pattern-by-pattern placement

“Conditional” means a documented future option, not an implementation requirement.

| Pattern | Decision and useful placement | Files to change/add |
|---|---|---|
| Strategy | Use a move-selection port for fake vs engine selection and future genuinely different algorithms. Difficulty alone is a SearchLimits/config value, not four classes. Rating policy remains a pure function unless a second policy exists | Existing `src/game/ai_player.*`, `src/chess/engine.*`; new `src/application/ports/move_selector.h`; `src/storage/elo.h` normally unchanged |
| Simple Factory | Small bootstrap construction functions for configured adapters or search workers; return explicit ownership | `src/main.cpp`; new `src/bootstrap/application.*` |
| Factory Method | Conditional: only if a base workflow needs subclasses to vary creation. Current startup does not need this inheritance pattern | No mandatory new file; revisit bootstrap only if justified |
| Abstract Factory | Defer: there is no current need to switch a family of coordinated infrastructure products. Explicit test wiring is simpler | No change |
| Singleton | Do not add global Database, GameManager, or service locator. Existing static-style utilities can remain pending need; one instance owned by main is not automatically the Singleton pattern | `src/core/logger.*`, `src/main.cpp`; avoid expanding global state |
| Observer | Typed post-transition events for participant/spectator notifications; retain frontend pub/sub with unsubscribe ownership. Persistence is explicitly coordinated, not an unreliable anonymous callback | `src/game/game_room.*`, `match_notify.*`, `game_handler.*`; new `game_events.h`; `frontend/js/core/events.js`, affected screens |
| Decorator | Optional message-sink metrics/tracing wrapper with identical delivery/error semantics; useful fake for tests | New `src/application/ports/message_sink.h`, later `src/observability/metered_message_sink.*` |
| Command | Typed MoveRequest/ResignRequest/etc. dispatched to use cases; first extract a few routes, not a class hierarchy per packet. Does not imply undoing authoritative multiplayer actions | New `src/protocol/request.h`; handler files; `src/net/websocket.*` |
| Adapter | PostgreSQL-to-store, socket-to-message-sink, engine-to-move-selector, real/fake clock | New port/adapter files above; existing `database.*`, `ai_player.*`, `tcp_server.*` |
| Facade | Small gameplay/query/analysis facades hide coordination complexity from handlers; replace the oversized all-purpose GameHandler role | New `src/application/*_service.*`; existing `src/game/game_handler.*` |
| Composite | Defer. Evaluation terms are a flat sum, not naturally a recursive part-whole tree. Keep shared evaluation helpers instead of an object tree | `src/chess/evaluator.*` only if measured duplication cleanup is worthwhile |
| Template Method | Defer for handlers/SQL. Middleware and RAII give shared mechanics without protected virtual hook hierarchies | No `BaseHandler`/`BaseRepository`; new `transaction.*` is RAII, not Template Method |
| Proxy | Conditional authorization/caching wrapper for a read port, only with invalidation and permission semantics. TT is already a specialized cache, not something to relabel as Proxy | Future `src/storage/cached_player_queries.*`; no cache in initial scope |
| Builder | Optional validated GameSetup construction if repeated multi-step room setup merits it; ordinary aggregate response DTOs plus codec normally suffice | `src/game/game_room.*`, `room_manager.*`; optional new `src/game/game_setup.h` |
| Prototype | Defer as a formal pattern: copying Board already supports search/replay without a polymorphic clone API | Preserve `src/chess/board.*`, replay logic |
| Visitor | Optional `std::visit` dispatch if typed requests use a variant; no virtual visitor across pieces. Do not choose a variant solely to demonstrate the pattern | New `src/protocol/request.h`, codec/dispatch tests |
| Memento | Search undo records and optional replay checkpoints are the useful analogue. Keep complete reversible board state; live multiplayer undo is out of scope | Existing `src/chess/board.*`, `move.*`; `frontend/js/screens/replay.js` only if checkpoint profiling justifies it |
| Null Object | Optional no-op telemetry only. Missing storage must return unavailable; missing auth must fail closed, not silently succeed | Optional observability adapter; never fake-success auth/game store |
| Iterator | Existing STL/range iteration is sufficient for moves/history. Add cursor pagination only if query scale requires it, not a custom iterator hierarchy | `src/storage/game_repo.*`, `player_repo.*`, query handlers if pagination later needed |
| Flyweight | Immutable lookup tables can be shared; retain existing table-driven evaluation/hash data as appropriate. A small Piece value is not itself evidence of Flyweight | `src/chess/evaluator.cpp`, `zobrist.*`; no interned Piece objects |
| State | Start with explicit GameStatus transition rules and exhaustive tests. Separate state classes only when states acquire substantial distinct behavior | `src/game/game_room.*`; optional new `src/game/game_transition.h` |
| Mediator | Application services coordinate rooms, matchmaking, persistence, and delivery. Keep each mediator cohesive rather than introducing a global event hub that knows everything | New gameplay/completion services; `src/game/matchmaker.*`, `match_notify.*` |
| Chain of Responsibility | Ordered request checks with explicit stop/error results; preserve route-specific seal/auth/permission policy | New `src/protocol/request_pipeline.h/.cpp`; `src/net/websocket.*`, `src/auth/auth_handler.*`, `src/main.cpp` |
| Bridge | Defer: current renderer/theme separation does not justify two independently extensible abstraction hierarchies. Revisit for multiple real renderers/backends | Existing `frontend/js/board/renderer.js`, `theme.js`; no initial rewrite |

### Anti-patterns to prevent

God handlers; global service locators; base classes with dozens of hooks; generic repositories that leak SQL everywhere; callbacks retaining mutable rooms/raw sockets; silent fake-success dependencies; event buses used for mandatory transaction steps; per-piece/per-node heap objects; speculative caching; pattern-per-course-topic implementation; changing behavior during file moves without separate tests.

The other machine-coding projects provide transferable ideas, not features to import: notification systems suggest Observer and delivery adapters; payment systems suggest idempotency and transaction boundaries; editors suggest Command/Memento; vending-machine/ATM exercises suggest explicit State transitions. None requires adding payments, live undo, or a document-style object tree to chess.

## 6. Critical contracts before removing coupling

### 6.1 Room transitions and completion

1. A room operation checks actor, turn, clock, and state under its mutex, then returns an immutable outcome with room identity and monotonically increasing revision.
2. Capture board/history/clocks/result/player IDs together where a coherent snapshot is required. Do not reconstruct a single logical event through unrelated locked getters.
3. Release the room lock before database, network, engine, or subscriber work. Subscriber payloads contain values, not a mutable `GameRoom&`.
4. Preserve ordering per room. Concurrent publishers need a serialized dispatch mechanism or ordered queue; merely incrementing a revision does not ensure delivery order.
5. Only the first legal terminal transition produces the completion operation. Distinguish terminal room state from persistence state: pending/saved/failed.
6. Give completion a stable unique key; live room IDs and database game IDs are different concepts. Add a unique database constraint before retries are enabled. Duplicate save must not increment stats/ELO again.
7. Define failure semantics: the game remains finished if storage is down; record/report persistence failure and allow safe retry. No promise of crash-durable retries without durable storage/outbox. Durable outbox is a later requirement, not inherent to Observer.
8. Emit a distinct persisted outcome only after database commit. Automatic tournament advancement, if later requested, consumes that identity and must itself be idempotent.

Keep existing client `game_over` behavior while exposing persistence status internally first. Any new client-visible status requires an explicitly versioned/additive protocol decision.

### 6.2 Transactions and ratings

- One transaction owns exclusive access to its connection for its **entire** lifetime, not just one `exec()` call. Use a non-copyable scoped transaction/connection lease with rollback on unfinished destruction.
- Start with a correctly serialized single connection if adequate; connection pooling is optional later. Route all DB users—including auth/session/tournament/analysis—through the ownership rule. A lock around game save alone is insufficient.
- Preserve atomic game/timings/rating/stat updates. RAII must cover exceptions as well as explicit errors; report original failures and handle a broken connection safely.
- Default refactor preserves documented rating snapshots. Before parallel game completion, choose and test either current-rating-at-completion with ordered player-row locking, or an explicitly defined snapshot/delta policy. Do not silently change rating math while extracting repositories.
- Use typed outcomes distinguishing not-found, unavailable, conflict, invalid input, and storage failure. Do not expose SQL internals/secrets to clients.

### 6.3 Protocol and security

- Frame/message size caps stay in the transport parser **before allocating/buffering oversized payloads**; middleware is not a substitute.
- Structurally parse a bounded outer message, validate route/envelope metadata, apply applicable cheap rate limits, verify/open required seals, validate the inner typed request, resolve required identity, and enforce use-case permissions. Parse each representation once; sealed inner plaintext is a separate representation.
- Validate consistency of outer route and decrypted content; retain existing authenticated envelope binding, freshness/replay checks, token verification, and error behavior.
- Route policies differ: public read endpoints, sealed auth operations, token-required starts, and connection/seat-authorized actions cannot share a blanket “all messages require JWT” rule.
- Identity is derived from verified credentials/seat binding, never trusted from client player IDs. Redact tokens, passwords, decrypted auth payloads, and connection strings from logs.
- Inventory existing JSON facilities before choosing a codec implementation. Reuse a suitable parser already present; adding a dependency requires a reasoned decision. Do not expand ad-hoc string scanners.

### 6.4 Connection lifetime and concurrency

- Introduce a connection handle containing identity plus generation; fd reuse must not deliver old events to a new client. Handle lookup/send must be synchronized within the transport adapter.
- Preserve EPOLLET draining, pending-write flush behavior, close cleanup, and current reentrant callback behavior during extraction.
- Do not remove the global connection lock just to improve a benchmark. First establish per-connection single-flight processing, safe ownership, bounded outgoing queues/backpressure, and shutdown/cancellation.
- Async AI/analysis jobs use immutable position snapshots, isolated engine state, and a room revision check before applying results. Late results after resign/disconnect/state change must be discarded.
- Separate CPU-heavy search from interactive request progress only after queue capacity, cancellation, and admission limits are specified. An unbounded future queue is not a latency fix.

## 7. Incremental implementation phases

Each phase ends with a build, relevant non-UI tests, review of the diff, and an honest entry in `docs/implementation_log.md`. Record UI verification as pending until Aditya supplies results. Do not mark later phases done because their interface skeleton exists.

### LLD-0 — Baseline and behavior inventory (start here)

**Change/add:** new `docs/lld_baseline.md`, new `tests/support/database_fixture.h` if isolation work is needed; relevant DB tests; `tests/test_engine.cpp` only for a separately justified deterministic test fix. Update `CMakeLists.txt` only as needed, preserving current changes.

- Record git state and protect existing edits in CMake, move generation, README, benchmarks, and the earlier LLD draft.
- Inventory every registered request, authorization rule, seal requirement, response/event type, error shape, and disconnect behavior. Note AI-game persistence and tournament/manual-result limitations.
- Re-run the existing C++ suite in the configured WSL toolchain and existing JS crypto-vector checks; distinguish infrastructure-unavailable tests from failures.
- Investigate the two historical planner/index assertions. Verify index existence and intended query behavior separately from cost-based planner choice; do not force a plan merely to make tests green.
- Replace timing-dependent exact-move assertions only with a justified deterministic-depth/position test; retain time-budget coverage separately.
- Centralize disposable DB test setup using a dedicated test database/schema and explicit safeguards. Never run destructive fixture setup against the normal development/production database.
- Capture release perft/search throughput and non-UI protocol latency baseline using existing benchmark tools after reviewing them. Verify tested perft depths; do not claim depth-5 coverage merely because the original plan mentions it.

**Exit:** reproducible baseline, enumerated known failures with causes/owners, protocol/security checklist, no unaccounted regressions. No production behavior changes.

### LLD-1 — Typed protocol boundary and delivery adapter

**Change:** `src/net/websocket.h/.cpp`, `tcp_server.h/.cpp`, `src/game/game_handler.*`, `src/game/match_notify.*`, `src/main.cpp`, `CMakeLists.txt`.

**Add:** `src/protocol/request.h`, `response.h`, `json_codec.*`; `src/application/request_context.h`, `result.h`, `ports/message_sink.h`; `src/net/connection_handle.h`, `socket_message_sink.*`; `tests/test_protocol_codec.cpp`, `test_message_sink.cpp`.

Migrate move/resign/state requests first behind existing route names. Characterize other routes before moving them. Centralize escaping and strict field/range validation; preserve compatible responses. Wrap current synchronized send/flush behavior rather than redesigning the event loop. Do not keep raw Connection pointers in application objects.

**Tests:** malformed JSON, nested/duplicate type policy, escaped strings, numeric bounds, unknown routes, matching old response semantics, disconnected recipient, generation mismatch, deferred writes; retain JSON escaping and sealed tests.

**Exit:** migrated use cases receive typed values and send through a fakeable boundary, with no auth/wire regression and no lock-removal change.

### LLD-2 — Cohesive handlers and use cases

**Change:** `src/game/game_handler.*`, `src/main.cpp`, existing `src/tournament/tournament_manager.*` only where dependency extraction requires it; `CMakeLists.txt`.

**Add:** the four `src/game/handlers/*_handler.*` modules and three `src/application/*_service.*` modules listed above; focused `tests/test_gameplay_service.cpp`, `test_query_service.cpp`.

Move one endpoint family at a time: gameplay/matchmaking; read/spectator/replay; analysis; tournaments. Keep the old GameHandler as a temporary registration facade, then shrink/remove delegating code once all callers migrate. AuthHandler remains separate; do not merge auth into gameplay. Keep small pure helpers as functions.

**Tests:** all existing game/auth/spectator/replay/analysis/tournament tests plus service tests for authorization and error mapping. Use lightweight fakes rather than linking sockets into every test.

**Exit:** endpoint adapters decode/authorize/delegate/encode; SQL and domain decisions are not duplicated across handler families. Pure domain code gains no network/storage include.

### LLD-3 — Persistence ports and transaction ownership

**Change:** `src/storage/database.*`, `game_repo.*`, `player_repo.*`; DB callers in `src/auth/service.*`, `session.*`, `auth_handler.*`, `src/analysis/cheat_report_repo.*`, `src/tournament/tournament_repo.*`, `tournament_manager.*`; application services and bootstrap wiring; CMake.

**Add:** `src/application/ports/game_store.h`, `player_queries.h`; `src/storage/postgres_game_store.*`, `postgres_player_queries.*`, `transaction.*`; `tests/test_transaction.cpp`, `test_store_contract.cpp`.

Wrap proven SQL functions initially; avoid rewriting all queries. Introduce typed failures and whole-transaction connection ownership. Explicitly enumerate all SQL call sites before changing Database access. Required persistence is not represented by an optional pointer that silently does nothing; capability-disabled mode is explicit.

**Tests:** transaction rollback at each write step, exceptions, connection loss, concurrent independent operations, fake/real port contracts, existing schema/repository/auth/tournament tests. Real PostgreSQL tests remain necessary.

**Exit:** no operation can accidentally execute inside another operation's transaction; game save remains atomic; read failures are distinguishable from empty results. No rating-policy change yet.

### LLD-4 — Coherent transitions, completion service, and notifications

**Change:** `src/game/game_room.*`, `game_handler.*`/extracted services, `match_notify.*`, `src/storage/game_repo.*`; `CMakeLists.txt`.

**Add:** `src/game/game_snapshot.h`, `game_events.h`, `src/application/game_completion_service.*`; a new migration for a stable completion key (choose next unused migration version at implementation time); `tests/test_game_events.cpp`, `test_game_completion.cpp`.

Replace scattered terminal handling with one completion path covering checkmate, draws, resignation, timeout, and existing disconnect semantics. Separate participant/spectator notification from mandatory completion persistence. Typed listeners must have clear lifetime/unsubscribe rules; a small listener list is enough, no general event-bus framework required.

**Tests:** duplicate terminal requests save once; DB failure then retry; independent completions; stable persisted ID on retry; no double ELO/stat increments; event ordering; snapshot consistency; listener failure isolation; callbacks outside room lock; AI persistence exclusions preserved.

**Exit:** coherent immutable outcomes, one terminal transition, idempotent durable save, explicit failure handling. No automatic anti-cheat/tournament behavior added.

### LLD-5 — Explicit request policy pipeline and composition root

**Change:** `src/net/websocket.*`, `src/auth/auth_handler.*`, `src/main.cpp`, extracted handlers; seal registry integration without rewriting primitives.

**Add:** `src/protocol/request_pipeline.*`, route policy table, `src/bootstrap/application.*` if main still needs extraction; `tests/test_request_pipeline.cpp`.

Move existing checks into ordered composable stages. Register route-specific policies alongside handlers. Constructor-inject mandatory services; keep optional capabilities explicit. Avoid both bypasses and double verification introduced by a transitional old/new route path.

**Tests:** every policy from LLD-0; missing/invalid/expired token, seat impersonation, unsealed required request, wrong envelope type, tampering/replay, rate-limit stop, oversized input rejected before allocation, exactly one handler invocation, capability-disabled mode.

**Exit:** one auditable path per request, no auth regression, no secret logging, no blanket policy that breaks public reads or seat-authorized moves.

### LLD-6 — Clock/search seams and measured concurrency (optional second milestone)

**Change:** `src/game/ai_player.*`, `game_room.*`, application analysis/gameplay services, `src/net/tcp_server.*`, `connection.*`, `src/concurrent/thread_pool.*` only after a reviewed concurrency design.

**Add:** `src/application/ports/clock.h`, `move_selector.h`, engine-selection adapter and bounded job executor if justified; deterministic clock/search tests and connection-lifetime concurrency tests.

Begin with fake clock and move selector; represent difficulty with configuration. Only then consider isolated engine jobs and reducing global lock scope. Specify per-connection serialization, stale job rejection, DB leases, outgoing queue limits, and shutdown before implementation. This is a separate concurrency project, not a prerequisite for learning every pattern.

**Tests:** deterministic clocks, concurrent AI requests with isolated engines, stale result rejection, disconnect/reconnect fd reuse, slow clients, saturated queues, shutdown with queued jobs, race checks where supported, release benchmarks before/after.

**Exit:** correctness under concurrency and measured latency improvement; otherwise retain the safer serialized implementation.

### LLD-7 — Frontend lifecycle, documentation, and final audit

**Change:** `frontend/js/net/protocol.js`, `socket.js`, `frontend/js/core/events.js`, `store.js`, affected `frontend/js/screens/{game,spectate,replay,tournament}.js`, `frontend/js/ui/screen.js` only where lifecycle fixes are needed; `docs/ARCHITECTURE.md`, protocol documentation, README, implementation log.

Keep frontend scope to typed/documented protocol mapping, subscription cleanup, and clear screen/store ownership. No visual redesign. Add non-DOM unit checks where feasible; do not run automated UI interactions.

**Documentation deliverables:** class/dependency diagrams for use cases, ports/adapters, GameRoom and completion; sequence diagrams for move acceptance/rejection, game completion with failed-save retry, sealed login, and disconnect/stale AI completion. Show lock boundaries, async ownership, and DB commit explicitly. Produce these during the relevant implementation phase using the applicable diagram workflow.

**Exit:** final dependency audit and non-UI regression suite complete; benchmark comparison recorded; manual UI results explicitly pending or supplied by Aditya. Remove temporary facades only after all usages migrate.

## 8. Verification and acceptance gates

- Every phase updates CMake for new source/tests without overwriting existing changes. Use the configured Linux/WSL toolchain, not a presumed native Windows epoll build.
- Run relevant existing tests in every phase; run the full suite at milestone boundaries. Record actual commands, toolchain, results, skips, and baseline failures in the implementation log.
- New service tests should mostly be fast and socket-free. PostgreSQL integration tests, seal vectors, search/perft tests, and non-UI protocol tests remain complementary, not interchangeable.
- Maintain a baseline of wire-level request/response fixtures. Do not freeze unordered JSON field order accidentally; do assert fields, types, authorization, and event sequencing.
- Measure release performance on the same machine/build/options/positions. Repeat samples and compare medians plus latency tails; do not present original aspiration targets as achieved measurements. Investigate material regressions before accepting abstractions; agree numeric thresholds from LLD-0 evidence.
- Keep sensitive profile/account data, passwords, tokens, and production DB contents out of fixtures, logs, diagrams, and public documents.

### Manual UI checklist — Aditya runs this

- Register/login/logout/refresh and capability-disabled screens.
- Create/join/quickplay/cancel; play a complete human game and an AI game.
- Illegal move, promotion, castling, en passant, timeout, draw and resign behavior.
- Disconnect/reconnect while playing; no ghost notification in a newly connected client.
- Join/leave spectator view and change screens repeatedly; no duplicate listeners/events.
- Profile/leaderboard/history updates after a persisted game; replay and analysis still work.
- Tournament creation/join/start and the currently supported result-reporting flow.
- Clear behavior during unavailable storage/network errors; no false successful save indication.

These checks stay **pending manual verification** until reported. Do not claim a full tournament live-room integration test while that feature remains deferred.

## 9. Decisions and stop points

Defaults: preserve existing UI/wire behavior; keep a modular monolith; interfaces at genuine seams; retain value-oriented engine; no new dependencies unless necessary; no automatic feature expansion; one phase per approval.

Decisions that need explicit agreement before their phase proceeds:

1. Rating semantics for concurrent games: preserve snapshots vs calculate from locked current ratings vs another specified policy.
2. Save durability requirement: process-lifetime retry vs crash-safe durable recovery/outbox.
3. Whether LLD-6 concurrency work is wanted after the maintainability milestone.
4. Whether automatic tournament rooms/results and anti-cheat jobs are separate next features.

Recommended first milestone: **LLD-0 through LLD-5**, implemented incrementally. LLD-6 is optional and higher risk; LLD-7 documents and verifies whichever milestone is chosen. Do not promise calendar estimates before baseline work reveals the real testing and concurrency costs.

Implementation-log entry template: phase and intent; changed files and boundaries; patterns used and why; behavior preserved/explicitly changed; automated results; manual checks pending/completed; performance observations; deviations/remaining risks; next approved step. This proposal itself does not mark any implementation phase complete.
