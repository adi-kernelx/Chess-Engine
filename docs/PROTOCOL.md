# Wire Protocol

The chess server speaks JSON over an [RFC 6455](https://datatracker.ietf.org/doc/html/rfc6455) WebSocket at `ws://<host>:<port>` locally and `wss://<host>` behind production TLS (default port `9000`, overridable through the `PORT` environment variable — Cloud Run sets it). One TCP connection carries every message a client sends after the HTTP upgrade completes.

The shipping format is JSON, not a separate binary move encoding. Source route policies and codecs are authoritative for exact validation.

---

## Framing

Every wire message is one WebSocket **text frame** carrying one UTF-8 JSON object. Fragmented frames are supported by the parser but the server never emits them. Binary frames are rejected. `ping` and `pong` opcodes are handled at the WebSocket layer (see [`src/net/websocket.cpp`](../src/net/websocket.cpp)) and never surface to a handler.

A frame's JSON object always carries a string field named `type` — structural parsing and RequestPipeline dispatch on this field; JSON key order is not significant. Unknown types are logged and ignored; the socket is not closed.

An **inbound size cap** (from Phase 7.9) rejects any frame whose payload exceeds a small, per-message-type limit — the server never allocates unbounded buffers on behalf of a client.

---

## Authentication model

The server has two identity paths:

- **Password**: register with username/email only, then activate through the email link and choose the password there. Registration returns email_sent, never a session. Login uses username/password. Sensitive requests travel inside a **sealed envelope** (see [Security](SECURITY.md)) — an ML-KEM-768 + X25519 hybrid KEM sealing the request payload with AES-256-CTR + HMAC-SHA-384. Login produces an **access token** (JWT, HS384) and a **refresh token** (32 random bytes; the server stores its SHA-384).
- **Google Sign-In**: routed through Supabase Auth. The frontend gets a Supabase-signed JWT, sends it to the server as `{"type":"google_auth", ...}`, and the server verifies it and issues its own session tokens. See [SECURITY.md](SECURITY.md) §Google Sign-In.

Every message that starts or joins a game — `create_game`, `join_game`, `quick_play`, `play_ai` — must carry an `access_token` field. Missing, expired, tampered, or "epoch-stale" tokens are answered with `{"type":"error","code":"auth_required"}`. Public directory/query routes such as `list_games`, `list_live_games` and `get_leaderboard` do not require auth. `spectate`, `game_state` and `get_active_game` do require an access token. `make_move` and `resign` are authorised by the fact that the socket's file-descriptor is seated in a specific `GameRoom`; that mapping was established at auth-time by `create_game`/`join_game`/`quick_play`.

---

## Message index

### Auth messages (registered in `src/auth/auth_handler.cpp`)

| `type`                | Sealed | Description |
|-----------------------|:------:|-------------|
| `seal_request`        | –      | Client asks the server for a fresh one-time ML-KEM public key + ML-DSA signature. Reply body is JSON with the key material for the client to encapsulate against. |
| `register`            | ✓      | Request activation with username and email only. Replies email_sent; choose a password through the email link before account activation. |
| `login`               | ✓      | Password login. Sealed. Returns access + refresh tokens. |
| `refresh`             | –      | Exchange a refresh token for a new access + rotated refresh token pair. |
| `logout`              | –      | Revoke the caller's current refresh-token family. |
| `logout_all`          | –      | Bump `players.token_epoch` — every unexpired access token for the player becomes invalid immediately. |
| `google_auth`         | ✓      | Verify a Supabase Google JWT and issue an application session. Enabled with valid provider configuration; ES256 uses public JWKS, not a shared JWT secret. |
| `link_google`         | ✓      | Attach a Google identity to an authenticated password account. |
| `request_password_reset` | ✓   | email → generic email_sent for eligible and unknown addresses. |
| `verify_email`        | ✓      | email_token, plus password for new-account activation → auth_action_ok with committed username. Browser then sends sealed login for automatic sign-in. Recovery-email confirmation does not log in. |
| `reset_password`      | ✓      | email_token and new password → auth_action_ok; revokes existing sessions. |
| `set_recovery_email`  | ✓      | access_token, current password and email; legacy password accounts without an email only. |
| `unlink_google`       | –      | Detach the Google identity from the authenticated account (only if password login is still viable). |

`seal_request`, `register`, `login`, and `google_auth` are all rate-limited per source IP (see `src/auth/rate_limiter.h`). The check runs **before** the expensive work (Argon2id verify, JWT verify) so a floodable path can never be used to burn CPU.

### Game messages (registered by `src/game/handlers/`)

Grouped by phase for readability:

| `type`                    | Auth  | Description |
|---------------------------|:-----:|-------------|
| `create_game`             |   ✓   | Create a waiting room; caller acquires a player seat. This is not a private invite-token contract. |
| `join_game`               |   ✓   | Join a room by `game_id`. |
| `quick_play`              |   ✓   | Join the ELO-bucketed matchmaking queue. Server pairs and dispatches `match_found` when a partner arrives. |
| `cancel_queue`            |   –   | Leave the queue if currently in it. |
| `play_ai`                 |   ✓   | Start a game against the built-in engine. `difficulty` picks max search depth + time budget. |
| `make_move`               |   –*  | Submit a move as `{from, to, promotion?}`. Server validates against `Board::make_move` and broadcasts a `move_made` frame to both seats and all spectators. *Auth is implicit — the fd must already be seated in a room. |
| `resign`                  |   –*  | Concede the game. Same seat-implied auth. |
| `offer_draw`              |   ✓   | Offer a draw to the opponent. The offer remains valid for at most 45 seconds and is also cancelled when the opponent makes a legal move. |
| `draw_response`           |   ✓   | Accept or decline the current draw offer with `{accept: true|false}`. Late or otherwise stale acceptance is rejected. |
| `offer_rematch`           |   ✓   | Offer the opponent a new game after a completed human game, identified by `game_id`. The offer expires after 45 seconds. |
| `rematch_response`        |   ✓   | Accept or decline a pending rematch with `{game_id, accept}`. Acceptance creates a new room with the same time control and swapped colors. |
| `get_pending_rematch`     |   ✓   | Recover an authenticated player's outstanding rematch offer after navigation or reconnection. Used by Replays to render Accept/Decline controls. |
| `game_state`              |   ✓   | Shared normal/tournament snapshot: FEN, move history, clocks, state, `white_username`, `black_username`. |
| `get_active_game`         |   ✓   | Discover/recover the authenticated player's active seat. |
| `list_games`              |   –   | List rooms in `WAITING` state. |
| `list_live_games`         |   –   | List rooms in `IN_PROGRESS` state (Phase 9.1 spectator lobby). |
| `spectate`                |   ✓   | Join a live room as a read-only observer. |
| `stop_spectating`         |   –   | Leave the spectator set. |
| `get_game`                |   –   | Full replay payload for a persisted game (moves, FENs, metadata). |
| `get_history`             |   –   | Persisted game history for a player. |
| `analyze_position`        |   –   | Run engine analysis on a FEN with bounded time and depth. |
| `analyze_game`            |   –   | Statistical anti-cheat analysis of a persisted game (Phase 9.3). |
| `get_profile`             |   –   | Public player profile: ELO, W/L/D, last login. |
| `get_leaderboard`         |   –   | Top-N players by ELO. |
| `create_tournament`       |   ✓   | Create a scheduled Swiss or Winners Advance event. |
| `join_tournament`         |   ✓   | Enter a tournament in `REGISTRATION` state. |
| `leave_tournament`        |   ✓   | Withdraw while registration rules allow it. |
| `set_tournament_registration` | ✓ | Creator closes/reopens registration before the deadline/pairing lock. |
| `check_in_tournament_round` | ✓ | Check in for a reserved round seat; early check-in cannot start clocks. |
| `start_tournament`        |   ✓   | Creator start request, subject to lifecycle/schedule validation; not a bypass for scheduled play. |
| `tournament_state`        |   –   | Schedule, standings, rounds, check-ins, and pairings. Pending live pairings expose runtime `game_id`; persisted completed games expose a distinct `replay_game_id`. |
| `list_tournaments`        |   –   | Tournaments in any status. |
| `report_tournament_result`|   ✓   | Creator-authorized audited result correction with a reason. Does not rewrite saved replay content; progression safety rules apply. |

### Server-initiated frames (no client message triggered them, but they may arrive at any time)

| `type`         | Delivered to                                | Description |
|----------------|---------------------------------------------|-------------|
| `match_found`  | both seats of a newly-matched quick-play    | Includes `game_id`, `color`, `opponent`, `white_time`, `black_time`. Emitted by `src/game/match_notify.cpp`. |
| `move_made`    | both seats + all spectators of a room       | Broadcast after any legal move. Includes the move, SAN, the resulting FEN, and the updated clock. |
| `game_over`    | both seats + all spectators                 | Result + reason (`checkmate` / `stalemate` / `timeout` / `resignation` / `draw_agreement` / `insufficient` / `repetition` / `fifty_move`). |
| `draw_offer_sent` | the offering player                      | Confirms that the offer was registered and starts the same server-authoritative 45-second validity window. |
| `draw_offered` | the receiving player                        | Announces the pending offer. The game remains interactive; the frontend presents this inline rather than as a blocking modal. |
| `draw_declined` | the offering player                        | The opponent declined, made a legal move, or allowed the offer to expire. An expired offer includes `reason: "expired"`. |
| `draw_offer_resolved` | the responding player                  | Confirms the response. An expired response has `accepted: false` and `reason: "expired"`. |
| `rematch_offer_sent` | the offering player                      | Confirms a 45-second rematch offer was registered for the completed room. |
| `rematch_offered` | the opponent                               | Announces who offered the rematch and which completed `game_id` it belongs to. |
| `rematch_declined` | the offering player                        | The opponent declined or the offer expired. |
| `rematch_offer_resolved` | the responding player               | Confirms decline/expiry without creating a game. |
| `rematch_started` | both players                                | A newly created game ID, swapped color, opponent, clocks, and preserved time control. |
| `spectate_start` | the joining spectator                     | Initial snapshot when a spectator joins mid-game — FEN, move list, spectator count. |
| `spectate_end`   | a spectator kicked because the room ended | – |

---

## Payload shapes (representative)

The exact schema lives with each handler's doc comment; a few characteristic examples:

**`quick_play`** →

```json
{
  "type": "quick_play",
  "access_token": "eyJhbGciOi...",
  "time_base": 600,
  "time_inc":  5
}
```

Success (server → caller, while pairing pends):

```json
{ "type": "queued", "queue_size": 3 }
```

Then, on pairing, both callers receive:

```json
{
  "type":       "match_found",
  "game_id":    17,
  "color":      "white",
  "opponent":   "bob",
  "white_time": 600000,
  "black_time": 600000
}
```

**`make_move`** →

```json
{ "type": "make_move", "from": "e2", "to": "e4" }
```

Broadcast:

```json
{
  "type": "move_made",
  "from": "e2",
  "to":   "e4",
  "san":  "e4",
  "fen":  "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
  "white_time_ms": 599872,
  "black_time_ms": 600000
}
```

**Error** — any handler may respond with:

```json
{ "type": "error", "code": "auth_required", "message": "..." }
```

Well-known error codes include `auth_required`, `rate_limited`, `not_your_turn`, `illegal_move`, `not_in_room`, `already_in_game`, `invalid_json`.

---

## Invariants worth knowing

- **Server-authoritative.** The client never announces a resulting board state. It says only `{from, to, promotion?}`. `Board::make_move` in `src/chess/board.cpp` is the sole source of truth for whether the move is legal, what pieces move, and what happens to castling rights, en passant, and the halfmove clock.
- **Server-authoritative clock decisions.** Clients interpolate received clocks for display; the server alone applies elapsed time, increment and timeout results.
- **Draw offers cannot be saved for later.** A pending offer is cleared when the receiving player responds, makes a legal move, the game ends, or 45 seconds elapse according to the server's clock. Acceptance at or after the deadline is rejected even if a stale client still displays an Accept button.
- **Rematches require both players.** A human rematch starts only after the opponent accepts within 45 seconds while both transports remain connected. It preserves the previous time control, creates a distinct game, and swaps colors. Decline, expiry, duplicate acceptance, and stale completed-game IDs never create a room.
- **A rematch offer is not an active game reservation.** Either player may leave the completed-game screen. Replays can recover the offer by authenticated player identity while it is valid, and after decline or expiry either player can create, join, or queue for another game normally.
- **Broadcast ordering.** Within a single room, a move is committed under a per-`GameRoom` mutex before its `move_made` frame is emitted. Two spectators may see the frame in a different order relative to each other's *other* live rooms, but within one room the sequence is total.
- **Delivery is generation-guarded.** A stale connection handle must not send a frame to a replacement connection. Room listeners deliver outside the room mutex; blocking database work must not hold broad transport locks.
- **Frame size cap.** The WebSocket layer refuses frames above a small ceiling (currently 64 KiB inbound). The largest legitimate messages are `get_game` payloads for long games; they fit.


## Scheduled tournament fields and identities

Creation supports `format: "swiss"` or `"winners_advance"`, `name`, `rounds`, `time_base`, `time_inc`, `registration_deadline`, `first_round_starts_at` and `round_duration_seconds`.

- Time control values are seconds; schedule timestamps are Unix seconds.
- Swiss uses a fixed round count. Winners Advance controls progression automatically; its UI omits Swiss rounds.
- Tournament state exposes the effective registration deadline, which cannot extend beyond the first-round pairing lock.
- Registration closes at its deadline or T−90 seconds, whichever comes first; reopening after the lock is denied.
- Pairing `game_id` identifies the live runtime room; `replay_game_id` identifies a saved played game.
- A bye or unplayed forfeit must not open an unrelated replay.
- Check-in and ordinary Join/Open game paths share the same scheduled-start gate.
- Unregistered/eliminated authenticated users may spectate; only reserved participants can join a tournament seat.

Standings include real usernames, rating, score, wins/draws, rank and Buchholz where applicable. Winners Advance placement follows elimination stage; final-survivor ties use wins, not rating.

## Request correlation and recovery

Request/reply operations can carry `request_id`; matching replies/errors remain scoped to the pending request. Unrelated broadcasts and other requests must not complete it.

Timeout/unavailable is not the same as unauthorized. While restoring a session, route navigation must not treat Connecting as a logout. A definitive invalid/revoked token can require reauthentication.

The exact current factories and normalizers are in [frontend protocol.js](../frontend/js/net/protocol.js); backend policies are in [gameplay handler](../src/game/handlers/gameplay_handler.cpp), [query handler](../src/game/handlers/query_handler.cpp) and [tournament handler](../src/game/handlers/tournament_handler.cpp).

## Sealing boundary

The sealed column describes routes requiring sealing when the server identity/sealing layer is configured. Production requires it. The plaintext auth examples/factories represent inner payloads or explicit local compatibility, not permission to send passwords/Google tokens unsealed in production.

Refresh is the existing TLS-protected rotating-token route. Gameplay does not use a persistent sealed channel.
