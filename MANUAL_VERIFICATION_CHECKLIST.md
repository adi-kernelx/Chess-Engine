# Manual Release Verification Checklist

Last updated: **2026-10-01**
Evidence: Aditya's WSL/server transcripts plus browser retest reports through 2026-10-01.
Scope: local browser + C++ server using the Supabase development database.

## Status legend

- `[x]` Verified by captured terminal output.
- `[ ]` Not tested or not proven by the supplied output.
- **BLOCKED** means a prerequisite must be fixed before the check is meaningful.
- A successful request appearing in the server log proves routing/transport only when its response was not captured.

## Current release-gate summary

| Area | Status | Evidence / limitation |
|---|---|---|
| Release build | PASS | Configure and all CMake targets reached 100%. |
| Supabase connection | PASS | `psql` returned database `postgres`, user `postgres`, PostgreSQL 17.6. |
| Base schema | PARTIAL | `players`, `sessions`, `games`, and `move_times` exist. |
| Advanced schema | **BLOCKED** | `cheat_reports`, tournament tables, and completion UUID/index are absent. |
| Server startup | PASS | Auth, persistence, sealed envelopes, and port 9000 all enabled. |
| Frontend serving | PASS | Main HTML, CSS, JS, chess assets, and puzzle data returned HTTP 200. |
| Browser ↔ server transport | PASS | Several WebSocket handshakes and requests were logged. |
| Authentication behavior | PASS LOCALLY | Live registration succeeded and returned **Account created. Welcome!**; protected match creation no longer redirects to login. Local auth remains unsealed and is not production-ready. |
| Authentication screen UI | PASS | Registration flow and success feedback were verified in the browser. |
| Profile data contract | PASS FOR NEW ACCOUNT | New account shows rating 800, zero record, no games, and no fabricated rating chart. |
| Spectator UI | PASS LOCALLY | Listing, live updates, navigation/re-entry, hard-refresh session recovery, leave/close cleanup, and completion behavior pass. |
| Leaderboard UI | FAILED; FIX APPLIED | Frontend sent `leaderboard` while the server route is `get_leaderboard`, then displayed a fabricated roster. Route, normalizer, and live-only UI are fixed; manual retest pending. |
| Initial rating | PASS FOR NEW ACCOUNTS | Browser retest confirms 800. Migration 0007 is still needed only for old unplayed accounts left at 1200. |
| Gameplay | PARTIAL | Core rules, rematch, AI, matchmaking cleanup, waiting-host transition, and post-game room creation pass. Numeric invite-link joining is fixed and awaits browser retest. |
| Persistence correctness | PARTIAL | Ordinary completion and a newly retested 120-second abandonment both appear in Replays. Row-level idempotency, timing, and rating queries still need verification. |
| Google Sign-In | NOT CONFIGURED | Expected log: `SUPABASE_JWT_SECRET is not set`. Local frontend hides Google login. |

## 1. Build and toolchain

- [x] Project opened in WSL2 Ubuntu 26.04.
- [x] Release configuration completed successfully.
- [x] OpenSSL 3.5.5 detected.
- [x] libpq 18.6 detected.
- [x] `chess_server` built.
- [x] All listed test/tool targets built through 100%.
- [ ] Full automated test binaries rerun during this manual session. The earlier 40/42-binary baseline is separate evidence.

Evidence command:

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release -j4
```

## 2. Supabase connectivity and schema

- [x] First malformed Session Pooler URL was rejected without changing the database.
- [x] Corrected Session Pooler URL connected successfully.
- [x] Connected database is `postgres`.
- [x] Connected database role is `postgres`.
- [x] PostgreSQL server reports version 17.6.
- [x] `public.players` exists.
- [x] `public.sessions` exists.
- [x] `public.games` exists.
- [x] `public.move_times` exists.
- [ ] `public.cheat_reports` exists. **Missing in captured query.**
- [ ] `public.tournaments` exists. **Missing in captured query.**
- [ ] `public.tournament_players` exists. **Missing in captured query.**
- [ ] `public.tournament_pairings` exists. **Missing in captured query.**
- [x] `games.completion_uuid` exists. The later Game 2 insert using this column persisted successfully.
- [ ] `idx_games_completion_uuid` exists. **Query returned false.**
- [ ] Migration versions are recorded consistently in `schema_migrations`.
- [ ] RLS state tested. Uncommitted migrations 0005/0006 must not be applied yet.

### Important result from the failed migration command

The combined 0001–0004 command used `--single-transaction`. Migration 0001 failed because `players.games_played` already existed. PostgreSQL therefore rolled back the complete command. The later files 0002, 0003, and 0004 were **not applied**. The existing schema was not damaged by this failed attempt.

### Next schema check

After exporting `DATABASE_URL`, inspect the existing Phase-8 objects before applying anything:

```bash
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -c "
SELECT
  EXISTS (SELECT 1 FROM information_schema.columns
          WHERE table_schema='public' AND table_name='players' AND column_name='games_played') AS stats_column,
  to_regclass('public.idx_players_elo') AS elo_index,
  to_regclass('public.games') AS games,
  to_regclass('public.move_times') AS move_times,
  to_regclass('public.schema_migrations') AS schema_migrations;
"
```

If those Phase-8 objects are present, apply only the missing feature migrations in one transaction:

```bash
psql "$DATABASE_URL" \
  --single-transaction \
  -v ON_ERROR_STOP=1 \
  -f src/storage/migrations/0002_phase9_3_cheat_reports.sql \
  -f src/storage/migrations/0003_phase9_4_tournaments.sql \
  -f src/storage/migrations/0004_lld4_completion_uuid.sql
```

Do not rerun 0001. After the command succeeds, rerun the table and completion-column/index queries and mark the missing items above only when they return present/true.

> Migration bookkeeping remains a known operational gap when SQL files are applied directly with `psql`: this does not automatically insert their versions into `schema_migrations`. Do not invent version rows until the physical schema has been verified. A dedicated migration runner is recommended before production deployment.

Latest persistence evidence: ordinary Game 2 saved as DB row 53, proving the
completion column is now available. Disconnect Game 1 failed only because the
deployed `games_termination_check` predates reconnect recovery. Apply migration
0008 before retesting abandonment persistence:

```bash
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 \
  -f src/storage/migrations/0008_allow_abandonment_termination.sql
```

AI replay history additionally requires migration 0009:

```bash
psql "$DATABASE_URL" -v ON_ERROR_STOP=1 \
  -f src/storage/migrations/0009_persist_unrated_ai_games.sql
```

## 3. Server identity and startup

- [x] No previous `secrets/server_identity.key` was present.
- [x] A new ML-DSA-65 server identity was generated.
- [x] Public SHA-384 pin was generated.
- [x] Private-key directory is ignored by Git (`secrets/`).
- [ ] Private key has restrictive effective filesystem permissions.
  - The generator reported mode `0600`.
  - `ls -l` on the Windows-mounted `/mnt/c` path displayed `-rwxrwxrwx`.
  - Windows/DrvFS permissions therefore need separate review; do not treat the displayed mode as production-safe.
- [x] `SERVER_IDENTITY_KEY_PATH` was supplied during the failed auth attempt.
- [x] `Sealed envelopes enabled` appeared, proving the cause of the silent register/login drops.
- [ ] Browser sealed-auth works end to end. **BLOCKED:** the planned browser PQC provider remains deferred and absent.
- [ ] For local manual auth only, restart with `SERVER_IDENTITY_KEY_PATH` unset.
- [x] `Game persistence enabled` appeared in startup logs.
- [x] `Auth handlers registered` appeared in startup logs.
- [x] Server listened on `ws://0.0.0.0:9000`.
- [x] Twelve worker threads started.
- [x] `Ctrl+C` produced an orderly shutdown.
- [x] Connections closed and all workers exited.
- [ ] Stable production `JWT_SIGNING_KEY` stored in a secret manager. The generated shell key is suitable only for this local session.
- [ ] Production identity pin copied into `frontend/js/config.js`. Not required for localhost because local mode permits an empty pin list.

Public pin from this test identity (safe to record; this is not the private key):

```text
OC9DOU673AWa9T/Gttb7Ogz/cUmXE0HLc5rAKTzLC5bH0BBPyxCN3axlGTZZzSf8
```

## 4. Frontend and WebSocket transport

- [x] Frontend HTTP server started on `127.0.0.1:8000`.
- [x] `/` returned HTTP 200.
- [x] CSS files returned HTTP 200.
- [x] JavaScript modules returned HTTP 200.
- [x] `assets/pieces/classic.svg` returned HTTP 200.
- [x] `assets/puzzles.json` returned HTTP 200.
- [x] At least three WebSocket connections completed the handshake.
- [x] Closing and reopening a browser connection reused fd 7 without crashing the server.
- [x] Server received repeated `list_games` requests.
- [x] Server received `list_live_games` requests.
- [x] Server received `get_profile` requests.
- [x] Browser close frames were processed and connections closed.
- [x] `/favicon.ico` returned 404. Harmless cosmetic omission.
- [x] Chrome's `/.well-known/appspecific/chrome-devtools.json` returned 404. Harmless browser probe.
- [ ] Browser console has no uncaught JavaScript errors. Terminal HTTP logs cannot prove this.
- [ ] WebSocket responses have the expected payloads. Only incoming server-side request logs were captured.
- [ ] Frontend left preview/demo mode after a real `auth_ok` response.
- [x] Register and login requests reached the server during the first attempt.
- [x] No `auth_ok` arrived because the server required sealed frames the browser could not produce.
- [x] Fake demo authentication was removed; auth now succeeds live or fails visibly.
- [x] Startup clears stale preview identity by reconciling UI state from `Session`.
- [x] Credential-bearing WebSocket debug frames are now redacted in code.
- [x] Rebuilt server log redaction verified through live authentication and protected game requests.

## 5. Authentication manual checks

- [x] Clicking **Register** visibly activates the Register tab.
- [x] Register mode shows the **Create account** title, password hint, and **Create account** button.
- [x] Clicking **Sign in** restores the Sign in title and form.
- [ ] Auth screen does not show **Preview — backend pending** merely because no login has happened yet.
- [x] Global connection indicator shows the backend as connected before submitting credentials.
- [x] Register Player A successfully (`adi8989` in captured server evidence).
- [x] Register Player B successfully in a separate browser session (`player2` in captured server evidence).
- [ ] Register Spectator C successfully.
- [x] Real authentication proven: both identities entered authenticated matchmaking and were seated in Game 1.
- [x] Duplicate username is rejected.
- [x] Username matching is case-insensitive.
- [x] Weak password is rejected.
- [x] Wrong password returns `invalid_credentials` without an indefinite wait or a local session.
- [ ] Unknown username also returns `invalid_credentials`.
- [x] Reload preserves an authenticated session.
- [ ] After applying migration `0011`, repeatedly hard-refresh Profile,
      Replays, Tournaments, and Leaderboard while the connection indicator is
      still **Connecting**, and switch routes between refreshes. The account
      remains signed in once connectivity settles; no stale refresh response,
      unrelated `auth_error`, or transient timeout clears the session.
- [ ] Open two tabs for the same signed-in browser profile and hard-refresh
      both close together. Refresh-token rotation is serialized across tabs,
      both recover, and the backend emits no `invalid_refresh` loop.
- [x] Logout removes the session.
- [x] Refresh after logout does not restore the session.
- [ ] Registration/login WebSocket traffic does not expose the plaintext password. **BLOCKED:** local loopback auth is unsealed until the deferred browser PQC provider is added; do not use this configuration for deployment.
- [x] New server logs contain redacted placeholders instead of credentials or tokens.
- [x] A newly registered profile shows rating `800`.
- [x] Games, wins, losses, and draws are all `0`.
- [x] Recent games says **No games yet**.
- [x] Rating history says **More games needed for a chart**.
- [x] Registration reports **Account created. Welcome!**.

The first failed attempt printed credential-bearing JSON in its debug log. The
logging code is fixed and the Release rebuild passes, but the credential used
in that transcript should be treated as exposed and not reused.

Google Sign-In is intentionally pending for deployed-environment testing. The
option remains visible locally and explains that cloud configuration is still
required.

## 6. Multiplayer gameplay checks

- [x] Player A can create a match without being redirected to login.
- [x] Player B sees and joins the open game without being redirected to login.
- [x] Both players receive the correct colors.
- [x] Both clocks start and switch correctly without increasing or visibly diverging between clients.
- [x] Legal moves were accepted and both clients requested synchronized game state throughout Game 1.
- [x] An illegal destination is not highlighted and a manually submitted illegal move is rejected without changing either board.
- [x] The waiting player cannot move out of turn.
- [x] Clicking the opponent's piece does nothing.
- [x] A move leaving the king in check is rejected.
- [x] Kingside castling moves both king and rook and synchronizes both boards.
- [x] En passant works only on the immediately following move.
- [x] Promotion offers visible queen, rook, bishop, and knight choices; the selected knight appears on `a8` for both players.
- [x] Game 1 reached checkmate and the server completed it once as `1-0 (checkmate)`.
- [x] Resignation ends the game for both players, disables the board, and appears once in Replays.
- [x] A 1+0 game ends automatically at zero for both players and appears once in Replays as `timeout`.
- [x] Finished resignation games reject further moves through the disabled board.
- [x] After `e4 d5 exd5`, White's bar shows a black-pawn icon and `+1`, with no broken-image icon.
- [x] Both boards show the same position after the capture.
- [x] Clicking `e2` highlights only `e3` and `e4`; clicking `g1` highlights only `f3` and `h3`.
- [x] Pinned pieces and positions in check do not show moves that expose the king.
- [x] Castling and en-passant target highlights match what the server accepts in the tested positions.
- [x] Immediately after the opponent moves, legal dots appear after the animation without waiting for a second network response.
- [x] Moving immediately after the turn changes is accepted without visible highlight-calculation delay.
- [x] **Offer draw** sends a real offer.
- [x] An incoming draw offer appears inline beside the board and never opens a modal or blocks moving.
- [x] An unanswered draw offer disappears for both players after 45 seconds and cannot be accepted afterward.
- [x] The opponent can decline; both boards remain playable and the sender is notified.
- [x] The opponent can accept; both clients finish the game by draw agreement.
- [x] Ignore the inline offer and make a legal move as the offeree; the
      response buttons disappear and the sender is
      told that the move declined the offer automatically.
- [x] After the offeree makes that move, a delayed/stale acceptance attempt is
      rejected and the game remains in progress.
- [x] Refresh/rejoin restores a pending draw offer to the correct player.
- [x] Clicking **Rematch** after a human game sends one offer to the opponent; it does not create a public waiting game.
- [x] The opponent can decline from Replays; no new game is created and the sender is notified.
- [x] The opponent can accept; both clients enter the same new game with the previous time control and swapped colors and can continue moving.
- [x] An unanswered rematch offer disappears for both players after 45 seconds.
- [x] A stale acceptance submitted after rematch expiry cannot create a game.
- [x] After expiry, the sender can immediately create, join, or queue for another game and receives no `already in a game` error.
- [x] If the recipient leaves the game screen while the offer is valid, Replays shows the recovered offer with working Accept/Decline controls.
- [x] Refreshing before opening Replays still recovers the offer by authenticated identity; accepting starts the same rematch for both players.
- [x] Repeated Rematch clicks while an offer is pending do not create duplicate rooms.
- [x] Rematch against the AI starts immediately with the same difficulty and time control.

## 7. Matchmaking and AI checks

- [x] Two users entering the same quick-play control are matched exactly once,
      both automatically enter the same `#/game/<id>` board, and neither is
      redirected to Replays.
- [x] If either matched player briefly receives a stale room-state response,
      the Game screen shows **Confirming your active game…**, recovers the
      durable seat, and remains on the board. The Replay **Resume game** action
      must recover the same room rather than loop back to Replays.
- [x] Queue cancellation prevents a later match.
- [x] Disconnecting while queued removes the entry.
- [x] A waiting host remains on **Waiting for a friend** until an opponent
      joins, then automatically enters the game without refreshing or
      performing another action.
- [x] After a completed game, either player can create another game. Its invite
      panel and link remain visible through at least two lobby refresh cycles
      (more than 10 seconds), with no stale `already in a game` error.
- [x] **Copy link** writes the complete join URL to the clipboard. If browser
      clipboard permission blocks it, the URL field is selected and Ctrl+C
      copies it.
- [x] Opening the copied `#/join/<id>` URL as an authenticated second player
      joins that exact room and takes both players to the board.
- [x] Easy AI game starts and remains on the Game screen instead of redirecting to Replays.
- [x] AI responds with legal moves.
- [x] Human cannot move twice while waiting for AI.
- [x] Resigning an AI game works.
- [x] After ending an AI game, another AI or human game can be started without a stale `already in a game` error.
- [x] On Hard/Max, the accepted human move appears immediately while the AI is still thinking.
- [x] AI game does not change rated-game statistics.
- [x] After migration 0009, a completed AI game appears once in Replays as **AI game · Unrated**, and its full move replay opens.

AI search is currently synchronous. The accepted human move is flushed before
search starts; only the AI response itself should wait for the selected search
budget.

## 7A. Puzzle checks

- [x] Clicking the rook in today's puzzle highlights its legal destinations.
- [ ] An illegal move is not animated and leaves the puzzle available for another attempt.
- [x] Legal but incorrect `a1a2` remains visible and shows **Retry**.
- [x] **Retry** restores the puzzle sufficiently to attempt the solution again.
- [x] Correct `a1a8` remains visible and changes the puzzle to **Solved** instead of resetting.
- [ ] The castling puzzle moves both king and rook correctly.

## 8. Spectator and lifecycle checks

- [x] Spectator C sees the active game in the live list with its numeric ID and
      real player names.
- [x] Spectator joins and receives the current board and clocks.
- [x] Each later move appears exactly once in the initial spectator session.
- [x] Spectator cannot move, resign, or offer a draw for a player.
- [x] Leaving spectate stops updates.
- [x] After Profile/Replays navigation, reopening the same live game restores
      its current position, clocks keep counting, and later moves arrive once
      without duplicate subscriptions.
- [x] Closing a spectator window removes it safely and does not affect either
      seated player.
- [x] A spectator watching through completion sees the final result, the clock
      stops, the board rejects interaction, and the game leaves the live list.
- [x] The spectator result names both the winner and color (for example,
      **Alice (White) wins · Resignation (1-0)**); draws are labelled **Draw**.
- [x] If C leaves a watched game before completion and later returns to the
      Spectate list, a **Recently watched result** card names both players,
      the winner/color, termination, and result, with a working replay link.
- [x] Hard-refreshing an active spectator view restores Player C's saved
      session, rejoins the same live view, and continues clocks/moves without
      showing Sign in.

## 8A. Responsive navigation

- [x] At 768 px wide or narrower, opening the menu shows all navigation links
      on one dark frosted-glass panel rather than directly on page content.
- [x] Menu labels retain readable contrast, visible keyboard focus, and no
      horizontal overflow at phone and narrow-window widths.
- [x] Selecting a link closes the menu and opens the requested screen.
- [x] Player disconnect does not crash the server.
- [x] When Player B closes the tab mid-game, Player A sees a disconnect message
      stating that B has 120 seconds to return.
- [x] Player B returns before 120 seconds, is rebound to the same seat, and sees
      the authoritative board, clocks, and move history.
- [x] Player A sees the opponent-reconnected confirmation and play continues
      without a duplicated move.
- [x] With Player B absent for 120 seconds, the game completes by abandonment and
      appears in Replays. Player A receives a win by
      **Opponent disconnected** no later than about 120.25 seconds after close.
- [ ] On the deployed build, repeat using a hard client-network interruption
      (disable Wi-Fi/network without closing Player B's tab). This is not a
      valid localhost test because loopback remains available when Wi-Fi is
      disabled. The server detects the silent WebSocket within roughly 60
      seconds; Player A then sees the 120-second return warning, and abandonment
      completes within roughly 180.25 seconds of the network cut.
- [ ] Restore Player B's network and resume while the displayed 120-second grace
      is still active; the same board, clocks, color, and move list return.
- [x] Reconnecting, making a move, and remaining beyond the original disconnect
      deadline does not trigger a stale timer or forfeit.

## 9. Persistence and rating checks

**Disconnect-abandonment persistence was manually retested successfully after
migration 0008.**

Captured evidence: ordinary Game 2 persisted successfully. Game 1 completed by
disconnect, but PostgreSQL rejected `termination='abandonment'` through the old
`games_termination_check`, so that specific game cannot appear in history.

- [ ] One authenticated human completion creates exactly one `games` row.
- [ ] `completion_uuid` is non-null.
- [ ] Repeated terminal action does not create a duplicate row.
- [ ] `result` and `termination` are correct.
- [ ] `move_count` matches the game.
- [ ] `move_times` row count matches `move_count`.
- [ ] Winner ELO increases and loser ELO decreases.
- [ ] Both players increment `games_played` exactly once.
- [ ] `games_played = wins + losses + draws` for each player.
- [ ] AI game persists once with `rated=false`, a nullable computer player ID,
      and no ELO/statistics change for the human.

Verification queries:

```bash
psql "$DATABASE_URL" -c "
SELECT id, result, termination, move_count, completion_uuid, started_at, ended_at
FROM games ORDER BY id DESC LIMIT 5;
"

psql "$DATABASE_URL" -c "
SELECT g.id, g.move_count, COUNT(mt.ply_number) AS timing_rows
FROM games g LEFT JOIN move_times mt ON mt.game_id=g.id
GROUP BY g.id, g.move_count ORDER BY g.id DESC LIMIT 5;
"

psql "$DATABASE_URL" -c "
SELECT username, elo_rating, games_played, wins, losses, draws
FROM players ORDER BY last_login DESC NULLS LAST;
"
```

## 10. Profile, leaderboard, replay, and analysis

- [x] Profile shows the correct username, ELO, and game counts after a rated
      checkmate; both players changed exactly once and
      `games_played = wins + losses + draws`.
- [x] Profile rating history shows the initial pre-game rating and each durable
      post-game rating.
- [x] Leaderboard is ordered by ELO descending and reflects both players'
      updated ratings without fictional or duplicate rows.
- [x] Tournaments, Leaderboard, and Replays show no Preview badge or fictional rows.
- [ ] A disconnected/database-error state is explicit and does not masquerade as empty live data.
- [x] Completed rated checkmate appears in both players' histories with the
      correct opponent, color, result, and termination.
- [x] While a game is ongoing, Replays shows one **Live game in progress** card,
      and **Resume game** restores the correct position, moves, and clocks.
- [x] Hard-refreshing `#/replay` keeps the saved session and reloads live and
      completed games after **Restoring your session…** instead of showing the
      signed-out view.
- [x] After the 120-second abandonment completes, the live card disappears and
      the game appears once in completed history with termination
      `abandonment`.
- [ ] Reloading the same browser during the grace period can resume the game.
- [ ] Restarting the backend is not treated as reconnect recovery: unfinished
      rooms are in-memory and are currently lost on process restart.
- [x] Replay metadata identifies both players, result, termination, time
      control, and the complete move list.
- [x] Replay reconstructs every move and the final position; stepping through
      all four plies of the tested Fool's Mate reaches the checkmated board.
- [x] Replay navigation works at both boundaries without moving before the
      initial position or beyond the final position.
- [x] Automatic playback advances each move once, can be paused/resumed, and
      stops at the final position.
- [x] Flipping the board preserves the current replay ply and position.
- [x] PGN copy includes the tested players, `0-1` result, `180+2` time
      control, and `1. f3 e6 2. g4 Qh4# 0-1`.
- [x] Hard-refreshing `#/replay/<id>` at the final ply restores the saved
      session and the replay without duplicating moves, replay rows, or live
      cards.
- [ ] Position analysis returns an evaluation/move.
- [ ] Persisted-game analysis completes.
- [ ] Anti-cheat report is stored without uncontrolled duplicates.

Anti-cheat persistence is **blocked until migration 0002 is applied**.

## 11. Tournament checks

Tournament persistence is **blocked until migration 0003 is applied**.

- [x] Creator creates a tournament with the requested rounds and time control;
      refresh preserves one Open row without duplication.
- [x] Other authenticated players join.
- [x] Duplicate joining is idempotent and never creates a second standings row.
- [x] Non-creator cannot start the tournament.
- [x] Creator starts it and Swiss pairings appear exactly once for round 1.
- [x] Standings and pairings show real usernames instead of `Player #<id>`.
- [x] A non-creator Start attempt says **Only the tournament creator can start
      the tournament.** rather than exposing `not_creator`.
- [x] **Share** exposes a copyable `#/tournaments/<id>` link. Opening it in a
      different signed-in browser loads the exact tournament and allows the
      player to join; signed-out visitors can sign in and return to the same
      tournament.
- [x] Only the creator sees result-entry controls for pending pairings; choosing
      `1-0`, `0-1`, or `½-½` and clicking **Record** persists that result.
      Non-creators see the result but no Record control.
- [x] The manual-result scaffold updates standings correctly.
- [x] The manual-result scaffold advances later rounds and final standings.

The tournament frontend now uses the live `tournament_list`,
`tournament_state`, `tournament_created`, `tournament_joined`, and
`tournament_started` messages. It must show an explicit backend error rather
than a Preview badge or fictional tournament rows.

## 11A. Session controls and Google Sign-In

- [x] While signed in, hard-refresh Profile, Settings, Tournaments,
      Leaderboard, Replay list/detail, Spectate list/watch, Puzzle, Play, and
      an active Game. Each route may briefly show **Restoring your session…**,
      then retains the same account and route.
- [x] Signed-out navbar shows **Sign in**.
- [x] Signed-out Profile renders immediately with Sign in and Create account;
      it does not wait five seconds or show a Player/800 pseudo-profile.
- [x] Signed-out Settings shows Sign in and Create account.
- [x] Signed-in navbar, Profile, and Settings each expose Sign out.
- [x] `#/register` opens directly on the Register tab.
- [x] Wrong password shows a definitive error and never creates a local session.
- [ ] After following `docs/SUPABASE_GOOGLE_AUTH_SETUP.md`, Continue with Google
      appears and returns to a real authenticated chess session.

Current scope uses creator-reported results. Automatic live-room creation from pairings is not implemented.

## 12. Security and failure checks

- [ ] Malformed JSON does not crash the server.
- [ ] Unknown message type fails safely.
- [ ] Missing/invalid/expired token is rejected.
- [ ] Repeated failed login is rate-limited without user enumeration.
- [ ] Slow/oversized client is disconnected without unbounded memory growth.
- [ ] Database outage produces an explicit unavailable/error path.
- [ ] Server remains usable by other clients after one client misbehaves.
- [ ] RLS denies direct `anon` and `authenticated` table access after the reviewed RLS migration is eventually applied.

## 13. Deferred features — do not mark as regressions

- [ ] Automatic tournament pairing → live GameRoom creation. Deferred feature.
- [ ] Automatic anti-cheat scheduling after completion. Deferred feature.
- [ ] Durable automatic retry/outbox after a failed game save. Deferred feature.
- [ ] Asynchronous AI execution. Deferred pending a benchmark and concurrency design.

## Next manual session

### AI follow-up (2026-10-01, pending manual verification)

- [x] Restart the rebuilt backend and hard-refresh the browser. Play Champion at 1+0 and a longer control; AI thinking shortens with low remaining time.
- [x] Automated: at exact flag fall the AI cannot move, the board is unchanged, the human wins 1-0, no increment revives the clock, and exactly one timeout completion fires.
- [ ] Finish an AI game by checkmate (both winning and losing cases): remain on the Game screen, see the final move and a non-blocking result message, and reject further moves.
- [x] After an AI result, wait several seconds: no late state response redirects to Replays. Rematch and lobby navigation still work.
- [x] Completed AI games appear once in Replays as unrated (migration 0009 applied); ratings remain unchanged.
- [x] Resign an AI game before making any move. The result appears immediately; no extra board move/request is needed.


1. Retest AI start/recovery, AI response/turn gating, resignation, and starting a second game after the AI game ends.
2. Verify the two remaining rematch edges: stale acceptance after expiry and duplicate-click protection; then test AI rematch.
3. Verify king-safety move filtering and the remaining castling restrictions.
4. Create a two-player game, open Spectate in a third authenticated window, and verify the real numeric game ID/names/moves with no Preview badge.
5. Complete one game and verify exactly one persisted row, non-null `completion_uuid`, timings, statistics, ELO, history, and the real leaderboard.
6. Continue with replay/analysis, authentication edge cases, and tournaments.
7. Apply and verify still-missing migrations **0002, 0003, and 0007** separately before testing their dependent features; do not rerun 0001.
8. Add notes beside failed checks and preserve relevant browser console, WebSocket frame, and server log evidence.

## 14. Scheduled live tournament — Phase 3 release candidate

Automated verification is complete. These browser checks are intentionally
left for Aditya. Before starting, apply migration `0010_live_tournament_runtime.sql`
to the development database, restart the rebuilt backend, hard-refresh the
frontend, and use three independent authenticated browser sessions: Creator,
Player B, and Player C.

### 14.1 Creation, local time, and registration

- [ ] Sign in from at least two browser sessions while the tournament page is
      open for one minute. The backend shows no `SSL SYSCALL`, `packet length
      too long`, `another command is already in progress`, or repeated
      `maintenance failed` errors.
- [ ] Creator opens **Create tournament**. The dialog contains name, rounds,
      base time, increment, registration deadline, first-round start, and
      round spacing/check-in window.
- [ ] Invalid or past dates show inline errors and a focusable error summary;
      the request is not sent. Round one must be at least 30 seconds after the
      registration deadline.
- [ ] Valid dates are previewed in the browser's local timezone. After
      creation, the detail view and a copied `#/tournaments/<id>` link show the
      same schedule after refresh and in another browser.
- [ ] Player B and Player C register. The real usernames and participant count
      update without fabricated rows or a Preview badge.
- [ ] Player C unregisters and can register again while registration is open.
      After the creator closes registration, unregister/register is no longer
      offered and a stale request is rejected.
- [ ] Creator can reopen registration before the deadline. It closes
      automatically at the deadline and cannot be reopened afterwards.
- [ ] A signed-out deep-link visitor can inspect the schedule, is asked to sign
      in before registering, and returns to the same tournament after sign-in.

### 14.2 Round check-in and reserved games

- [ ] After registration closes, every registered player sees the same round,
      pairing, scheduled start, and one live countdown.
- [ ] Player B clicks **Join round** early and sees a truthful waiting state.
      Refreshing the tournament deep link preserves check-in status.
- [ ] An account not registered for the event cannot check in or occupy either
      reserved seat.
- [ ] Both paired players check in. At the scheduled start both automatically
      enter the same new `#/game/<id>`; assigned colors match the pairing and
      clocks start once, not before both players are present.
- [ ] The game screen names the tournament and round and provides **Tournament**.
      A tournament game never offers **Rematch**.
- [ ] One paired player checks in late but before the check-in window closes.
      **Join round/Rejoin round** returns that player to the same reserved
      color and current position rather than creating another room.
- [ ] Disconnect a player after play begins and reconnect within 120 seconds.
      Board, moves, color, and clocks recover; the original disconnect deadline
      no longer forfeits the reconnected player.

### 14.3 Automatic results, no-shows, and recovery

- [ ] Finish separate tournament games by checkmate/resignation and timeout or
      draw. Each game appears once in Replay and normal profile/rating history;
      its pairing resolves automatically without creator result entry.
- [ ] After game over, **Back to tournament** opens current standings and
      **Open replay** opens the saved game. No invite/rematch control appears.
- [ ] A later round is generated only after every prior pairing is terminal
      and its scheduled start has arrived. Players are never rematched when a
      valid fresh Swiss opponent exists.
- [ ] In one pairing only one player checks in before the window closes. That
      player receives the forfeit point and no stale live room blocks future
      play.
- [ ] In another pairing neither player checks in. It becomes double forfeit,
      neither receives a point, and the tournament can still advance.
- [ ] Refresh Tournament, Game, and Replay routes during the flow. The account,
      tournament deep link, check-in state, live game, and completed result all
      recover without duplicate rows or redirect loops.

### 14.4 Spectator, correction, and completion states

- [ ] Player C opens **Spectate** for a live pairing. Board, names, moves, and
      clocks update once; Player C cannot move or use player-only controls.
- [ ] Creator opens **Correct result**. Saving without a reason is blocked.
      The warning clearly says the correction changes tournament standings and
      Buchholz, not replay, profile record, or global rating.
- [ ] Save one correction with a reason. Standings recompute once; refreshing
      preserves it while the saved replay and both players' profile/rating
      records remain unchanged.
- [ ] A non-creator sees no correction control and receives friendly copy if a
      forged creator-only command is attempted.
- [ ] On completion, the directory and deep link show final standings and
      completed pairings. There are no registration/check-in controls, fake
      data, Preview badge, duplicate games, or stuck loading state.
- [ ] Repeat the critical directory/detail/create flow at narrow mobile width:
      cards, tables, dialog, controls, countdown, focus order, and keyboard
      Escape/Tab behavior remain usable without horizontal page overflow.

Do not mark live tournament play complete or begin cloud deployment testing
until every applicable item above is checked or its failure is recorded.
