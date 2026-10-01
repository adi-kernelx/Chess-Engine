# Live Tournament Play — Implementation Plan

## 1. Objective and current gap

Replace the Phase-9.4 creator-reported-result scaffold with real scheduled
Swiss tournament games.

The existing code correctly stores tournaments, registrations, Swiss pairings,
standings, deep links, and creator-authenticated overrides. Its critical gap is
that a pairing is only a database row: it does not reserve two seats in a real
`GameRoom`, and ordinary game completion does not update the pairing. This plan
implements the missing `implementation_plan.md` requirement:

> Auto-create game rooms for each round's pairings.

This is a feature change, not an LLD-only refactor. It keeps the existing
single-server/WebSocket architecture and adds no framework, Redis, queue, or
microservice.

## 2. Product rules and invariants

### Creator configuration

Creation collects:

- name;
- number of Swiss rounds;
- game base time and increment;
- registration deadline;
- first-round scheduled start; and
- round duration (the minimum spacing between round starts and the check-in / no-show window).

Validation:

- `registration_deadline < first_round_starts_at`;
- the deadline-to-first-round gap is at least 30 seconds;
- every timestamp is stored in UTC and displayed in the browser's local time;
- rounds remain within `1..30`;
- base time is positive; increment is non-negative; and
- round duration is at least 60 seconds. It is a scheduling/check-in window,
  not a forced chess-game adjudication deadline.

The creator may manually close registration early. They may reopen it only
before the deadline and before round-one pairings are locked. At the deadline,
the server closes registration authoritatively; a stale browser cannot join.

### Round scheduling and check-in

For round `N`, its earliest start is:

```text
first_round_starts_at + (N - 1) × round_duration
```

If the preceding round is still unresolved, the next round waits even after
that time. It starts only when both conditions are true: the earliest start has
arrived and all preceding pairings have terminal results.

All non-withdrawn registered players are paired. Check-in does not change the
Swiss pool:

- early check-in places the player in a waiting state;
- when the round opens, one reserved room is created per non-bye pairing;
- a checked-in player is bound to their reserved seat;
- a registered player arriving after the start enters the already-open room
  immediately while the round window remains open; and
- an unregistered user can neither check in nor occupy a tournament seat.

The chess clock starts only when the scheduled start has arrived and both
paired players are connected. A disconnect after the game starts continues to
use the platform's existing 120-second reconnection rule.

Default no-show policy for implementation:

- exactly one player checked in before the round window closes: that player
  wins by forfeit;
- neither player checked in: double forfeit, worth zero tournament points to
  both; and
- a game already in progress is never adjudicated from board evaluation merely
  because the round window closed. It finishes normally, and the next round
  waits for it.

### Results and overrides

Checkmate, timeout, resignation, draw, abandonment, or disconnect completion
uses the existing game-completion path. The saved game updates normal profile
statistics, rating, history, and replay once; the linked tournament pairing is
then resolved once.

Creator override is retained as an audited correction mechanism, not the normal
play path. It requires a reason and records actor, old result, new result, and
timestamp. Changing an old round does not rewrite the immutable replay, global
rating, or profile win/loss record. Tournament scores and Buchholz are
recomputed transactionally from pairing results so an override cannot apply a
score delta twice. Already-played pairings remain historical; only future,
not-yet-generated rounds use corrected standings.

### Concurrency invariants

- Registration closes once at the deadline.
- A player has at most one check-in per tournament round.
- A pairing has at most one reserved room.
- A room completion resolves its pairing at most once.
- A round's pairings are generated at most once, even if maintenance ticks or
  creator actions race.
- Round advancement occurs only after every pairing is terminal.
- Creator overrides are append-only audit records.

Database uniqueness constraints and a transaction/row lock remain the final
authority; frontend button disabling is only usability protection.

## 3. Implementation phases

Only three phases are used. Complete, test, and log each phase before starting
the next one.

### Phase 1 — Durable tournament lifecycle and scheduling

**Status: Complete — 2026-10-01.** The durable schema, clock-driven lifecycle,
registration controls, round check-in, no-show decisions, audited overrides,
and standings recomputation are implemented and verified. Phase 2 subsequently
connected those durable pairings to reserved live `GameRoom` instances.

Build the database-backed state machine before creating rooms.

#### Database

Add `src/storage/migrations/0010_live_tournament_runtime.sql`:

- `tournaments.registration_deadline`;
- `tournaments.first_round_starts_at`;
- `tournaments.round_duration_seconds`;
- `tournaments.registration_open`;
- `tournaments.registration_closed_at`;
- expand the tournament status constraint to
  `registration | scheduled | in_progress | completed`;
- `tournament_rounds`, unique on `(tournament_id, round)`, containing earliest
  start, actual start, check-in close, completion time, and
  `scheduled | live | completed` status;
- pairing `result_source`, result/completion timestamps, and a unique non-null
  `game_id` mapping;
- `tournament_round_checkins` with unique
  `(tournament_id, round, player_id)`; and
- `tournament_result_overrides` containing actor, old/new result, reason, and
  timestamp.

Extend the pairing result domain for `double_forfeit` without pretending it is
a normal chess-game result.

#### Backend

Change:

- `src/tournament/tournament_repo.h/.cpp`;
- `src/tournament/tournament_manager.h/.cpp`;
- `src/application/tournament_service.h/.cpp`;
- `src/game/handlers/tournament_handler.h/.cpp`;
- `src/protocol/json_codec.*` or the existing route decoder where required;
- `frontend/js/net/protocol.js`; and
- `src/storage/schema_phase7.sql` only if it remains the canonical clean-install schema.

Add commands for:

- creator registration open/close;
- registered-player round check-in;
- scheduled lifecycle maintenance; and
- creator result override with a mandatory reason.

Inject the existing `application::ports::Clock` into scheduling logic so tests
advance fake time instead of sleeping. The server's existing maintenance
callback calls a new tournament tick beside disconnect/clock maintenance.

Replace incremental score mutation with a transactional
`recompute_tournament_standings` operation after every automatic result or
override.

#### Tests and exit criteria

- deadline boundary and stale-join rejection;
- manual close/reopen permissions;
- earliest-round-start calculation;
- idempotent check-in and round generation;
- early/late check-in states;
- no-show and double-forfeit scoring;
- reversible result override and audit row;
- concurrent tick/start requests generate one round only; and
- existing Swiss and tournament tests continue to pass.

Phase 1 exits with a correct durable schedule and state machine, but no live
rooms yet.

### Phase 2 — Reserved GameRooms and automatic results

**Status: Complete — 2026-10-01.** Pairings now reserve durable-identity game
seats, checked-in players bind/reconnect to their assigned colors, scheduled
rooms start once, persisted game completion resolves tournament standings, and
scheduled/unstarted rooms recover after a server restart. Phase 3 UI work has
not started.

Connect each non-bye pairing to real chess play.

#### Runtime design

Add a cohesive `TournamentRuntimeService` (Facade/Mediator) that coordinates
the tournament manager and `RoomManager`; do not put scheduling SQL inside
`GameRoom`.

Change:

- `src/application/tournament_runtime_service.h/.cpp` (new);
- `src/game/room_manager.h/.cpp`;
- `src/game/game_room.h/.cpp`;
- `src/game/game_snapshot.h` and typed completion events only for stable pairing identity;
- `src/application/game_completion_service.h/.cpp`;
- `src/application/gameplay_service.*` for tournament-seat recovery;
- `src/game/game_handler.*` and `src/main.cpp` for composition/tick wiring; and
- `CMakeLists.txt` plus focused tests.

Add a reserved-room construction path with both authenticated database player
ids, usernames, ratings, colors, configured time control, tournament id,
pairing id, and scheduled start. A reserved empty seat is identified by durable
player identity, not by a socket descriptor. Check-in/reconnect binds the
current socket only when its authenticated database id matches that seat.

Use the existing Observer completion path. After the game save succeeds (or an
idempotent duplicate is confirmed), notify a tournament-completion port with
`pairing_id + immutable game result`. That observer resolves the pairing and
recomputes standings. It must never infer results from frontend messages.

The mapping `pairing.game_id` is persisted before clients are sent to a room.
Scheduled/unstarted reserved rooms can be recreated from Postgres after a
server restart. Recovery of an already-moving in-memory game remains the
platform-wide room-durability limitation and must be called out before cloud
deployment rather than hidden as a tournament feature.

#### Tests and exit criteria

- one pairing creates one reserved room;
- only the two paired accounts can bind seats;
- early player waits; late player joins the same room;
- both present after scheduled start begins clocks exactly once;
- reconnect restores the reserved color/board;
- checkmate, timeout, resignation, draw, and abandonment each resolve the
  pairing automatically;
- completion retry does not duplicate game persistence, ELO/stat updates, or
  tournament points;
- one/no-player no-show policies work; and
- a two-round integration test advances from played games without creator
  result entry.

Phase 2 exits when tournament standings can be produced entirely by real games.

### Phase 3 — Creator/player UI, end-to-end verification, and documentation

**Status: Implementation complete; manual three-browser verification pending —
2026-10-01.** The scheduled creator/player workflow, tournament game context,
protocol normalization, responsive styles, focused automated tests, and
architecture/protocol documentation are complete. Live tournament play remains
a release candidate until Aditya reports the checklist below as passed.

Replace the current administrative-first tournament screen with the scheduled
live workflow.

Change:

- `frontend/js/screens/tournament.js`;
- `frontend/js/screens/game.js` only for tournament context/status;
- `frontend/js/net/protocol.js`;
- existing CSS component/screen files using current tokens;
- `MANUAL_VERIFICATION_CHECKLIST.md`; and
- architecture/protocol documentation after code passes.

Creator UI:

- creation fields for all schedule settings;
- local-time preview with UTC transmission;
- Open/Close Registration;
- registration and next-round countdowns;
- participant/check-in status;
- current pairings with **Open game/Spectate**;
- audited override dialog with mandatory reason; and
- clear warnings that overrides affect tournament standings, not global rating
  or the saved replay.

Player UI:

- register/unregister while allowed;
- **Join round**;
- early waiting state with scheduled-start countdown;
- immediate navigation to the reserved game when it becomes available;
- late-entry recovery to the same seat;
- opponent/no-show status; and
- durable refresh/deep-link recovery.

UI states must cover loading, empty, signed-out, registration-open,
registration-closed, waiting, live, completed, unavailable, and authorization
failure without preview/fake data.

#### Final verification

- automated eight-player/four-round Swiss with real room completions;
- creator and non-creator authorization/failure paths;
- deadline and round-boundary fake-clock tests;
- disconnect/rejoin and no-show tests;
- override audit/recompute tests;
- existing auth, gameplay, persistence, replay, spectator, rating, and
  concurrency suites; and
- Aditya's manual three-browser UI checklist.

Only after all automated checks and reported manual checks pass should the
implementation log mark live tournament play complete and cloud deployment
testing begin.

## 4. Design principles and patterns

- **State:** registration, scheduled/waiting, live round, and completed
  transitions are explicit and server-authoritative.
- **Strategy:** the existing Swiss pairing algorithm remains replaceable and
  pure; scheduling and room creation do not enter it.
- **Observer:** durable game completion drives tournament result propagation.
- **Facade/Mediator:** `TournamentRuntimeService` coordinates repository,
  manager, rooms, and completion without making them depend directly on one
  another.
- **Command:** create, register, check in, registration control, and override
  remain authenticated commands with idempotent boundaries.
- **Repository/Adapter:** PostgreSQL and the injected clock stay behind focused
  interfaces for deterministic tests.

SOLID boundary: `GameRoom` owns chess and clocks; `TournamentManager` owns
durable tournament rules; `TournamentRuntimeService` coordinates the use case;
handlers translate protocol only; the frontend renders server state and never
decides deadlines, pairings, or results.

## 5. Deliberate non-goals

- No creator power to alter global ELO, profile win/loss counts, or replay data.
- No board-evaluation adjudication at a round deadline.
- No Redis, distributed scheduler, or multi-instance room sharing in this
  feature. Cloud Run must initially use the project's single-instance
  WebSocket deployment constraint until room durability is separately solved.
- No automatic anti-cheat job scheduling; that remains a separate feature.
