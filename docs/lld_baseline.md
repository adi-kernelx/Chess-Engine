# LLD-0 Baseline

Snapshot of the codebase before any LLD-phase code changes land. Every
subsequent LLD-N phase is measured against what is written here — not
against aspirational plan-doc text or CLAUDE.md claims that predate this
audit.

## Git state at baseline

- Branch: `main`
- Head: **`3a1c22b`** — `docs: LLD refactoring proposal`
- Ancestor: `771fa4c` — `docs: comprehensive documentation — architecture, benchmarks`
- Both are local; not pushed.

Working tree at the end of LLD-0 contains:
- `tests/support/database_fixture.h` (new, safe-DB gate for future destructive tests — see §Test DB safeguard).
- `docs/lld_baseline.md` (this file).
- `CMakeLists.txt` — four `target_compile_definitions(... CHESS_SOURCE_DIR ...)` lines added (see §Fixes below).
- `tests/test_database.cpp`, `test_password.cpp`, `test_token.cpp`, `test_oauth.cpp` — schema path uses the compile define; drop list broadened to cover Phase-8/9 tables (see §Fixes below).
Nothing under `src/` (production code) is modified in LLD-0.

## Toolchain and environment

| Component | Value |
|---|---|
| Host       | Windows 11 (WSL2 Ubuntu 26.04 LTS) |
| CPU        | AMD Ryzen 5 5600H, 6c/12t |
| RAM        | 8 GB (WSL allocation) |
| Kernel     | 6.6.87 microsoft-standard-WSL2 |
| Compiler   | GCC 15.2 |
| CMake      | 4.2.3 |
| OpenSSL    | 3.5.5 (default provider carries ML-KEM, ML-DSA, Argon2id) |
| Postgres   | 18 local cluster, database `chess_bench` |
| Build flags | `-Wall -Wextra -Wpedantic -Werror -std=c++17`, `-DCMAKE_BUILD_TYPE=Release` |

## Route + policy inventory

Every WebSocket `type` the server currently handles, its auth rule, its
seal rule, and where in the source the response is produced. Auth here
means **`extract_identity()` is called before any state change** — a
missing/tampered/expired token returns `{"type":"error","code":"auth_required"}`.
Seal means the payload must arrive inside a Phase-7.4 sealed envelope
(configured at startup by `sealed_registry.require_sealed(...)`).

### Auth-domain routes (`src/auth/auth_handler.cpp`)

| Route          | Auth | Seal | Rate limit                 | Optional? | Notes |
|----------------|:----:|:----:|----------------------------|:---------:|-------|
| `seal_request` |  –   |  –   | `seal_request_per_ip` (20/min) | requires SERVER_IDENTITY_KEY_PATH | Returns fresh one-time ML-KEM key + ML-DSA signature. |
| `register`     |  –   |  ✓   | `register_per_ip` (3/hour) | –         | Sealed. Issues access + refresh on success. |
| `login`        |  –   |  ✓   | `login_per_ip` (5/min) + per-account | – | Sealed. Password verify then session issue. |
| `refresh`      |  –   |  –   | –                          | –         | Rotates refresh, mints new access. |
| `logout`       |  –   |  –   | –                          | –         | Revokes current refresh family. |
| `logout_all`   |  –   |  –   | –                          | –         | Bumps `players.token_epoch`. Invalidates every unexpired access token. |
| `google_auth`  |  –   |  ✓   | `google_auth_per_ip` (10/min) | requires SUPABASE_JWT_SECRET | Sealed. Verifies Supabase JWT; issues session. |
| `link_google`  | auth |  –   | –                          | requires SUPABASE_JWT_SECRET | Attaches Google identity to authenticated password account. |
| `unlink_google`| auth |  –   | –                          | requires SUPABASE_JWT_SECRET | Only if password login is still viable. |

### Game-domain routes (`src/game/game_handler.cpp`)

| Route                      | Auth | Notes on identity/permissions |
|----------------------------|:----:|-------------------------------|
| `create_game`              | ✓   | Auth via `extract_identity`. Username/ELO from `players` row. |
| `join_game`                | ✓   | Auth via `extract_identity`. Refuses if room full or wrong state. |
| `make_move`                | seat | No token check — the fd must already be seated in a room. Seat established at create/join/match time. |
| `resign`                   | seat | Same seat-implied rule. |
| `quick_play`               | ✓   | Auth via `extract_identity`. Matchmaker keys by ELO. |
| `cancel_queue`             | fd   | Removes caller's fd from the queue if present. |
| `list_games`               | –   | Public. Returns `WAITING` rooms. |
| `game_state`               | fd   | Reads current room by fd (must be seated). |
| `play_ai`                  | ✓   | Auth required even though AI games are not persisted — keeps the wire contract uniform. |
| `get_profile`              | –   | Public. Reads `players` row by id. |
| `get_leaderboard`          | –   | Public. Top-N by ELO. |
| `spectate`                 | ✓   | Auth via `extract_identity`. Adds fd to room's spectator list. |
| `stop_spectating`          | fd   | Removes fd; idempotent. |
| `list_live_games`          | –   | Public. Returns `IN_PROGRESS` rooms with spectator count. |
| `get_game`                 | –   | Public. Full persisted game replay by game_id. |
| `get_history`              | –   | Public. Persisted history by username. |
| `analyze_position`         | –   | Public. Engine analysis of a FEN with bounded depth ≤15, time ≤3s. |
| `analyze_game`             | –   | Public. Statistical anti-cheat report on a persisted game. |
| `create_tournament`        | ✓   | Auth required. Creator captured for later `report_tournament_result` auth. |
| `join_tournament`          | ✓   | Auth required. Rejects duplicates. |
| `start_tournament`         | ✓   | Auth required. Creator-only in current implementation. |
| `tournament_state`         | –   | Public. Standings + pairings + tournament row. |
| `list_tournaments`         | –   | Public. Optional `status` filter. |
| `report_tournament_result` | ✓   | Auth required. Creator-only. **Current tournament model does not auto-create live rooms** — results are creator-reported. |

### Server-initiated frames

Emitted by handlers, not in response to a specific client message from the recipient:

| Frame            | Sent to                                | Origin |
|------------------|----------------------------------------|--------|
| `match_found`    | both seats of a newly-matched game     | `src/game/match_notify.cpp::build_match_found` |
| `move_made`      | both seats + all spectators of a room  | `handle_make_move` after `Board::make_move` commits. Carries FEN + updated clocks. |
| `game_over`      | both seats + all spectators            | terminal-transition path in `handle_make_move` / `handle_resign` / timeout handler |
| `spectate_start` | joining spectator                      | `handle_spectate` — full snapshot at join time |
| `spectate_end`   | spectator ejected because the room ended | (implied when a spectator's room hits FINISHED) |

### Disconnect behavior

`GameHandler::on_player_disconnect(fd)`:
1. Removes the fd from the matchmaker queue.
2. Finds the fd's room (if any) and calls `room->on_disconnect(fd)` — pauses clock, waits for reconnect up to 60 s (Phase 4.1).
3. Sweeps the fd out of every room's spectator list (`room_mgr_.remove_spectator_everywhere(fd)`) — prevents an OS-recycled fd from silently receiving broadcasts from someone else's game.

### Persistence gating (`GameHandler::persist_game`)

- Skipped if `room->is_ai_game()` — AI games are not persisted (see the comment on `handle_play_ai`).
- Skipped if the room has fewer than 2 human db_player_ids.
- Otherwise calls `storage::save_completed_game` — one SQL transaction covering `games` insert + `move_times` inserts + ELO + stats updates for both players.
- **No idempotency key on the write.** Two calls with the same room can double-count stats. This is a known gap and is the driving concern for LLD-4's completion service.
- On DB failure the game state remains FINISHED, but nothing is persisted and nothing is retried in this process.

## Test-DB safeguard

`tests/support/database_fixture.h` (new in LLD-0) contains
`require_test_database()`. It refuses to allow destructive test setup
unless all of the following hold:

- `$DATABASE_URL` is set and points to `127.0.0.1` / `localhost` / `::1`.
- The database name is one of `chess_bench`, `chess_test`, `postgres_test`.
- The URL string contains neither `supabase` nor `pooler`.

The `.env` in the repo root points at Supabase and is `.gitignore`-listed;
anyone who accidentally `source .env`s before running a destructive test
will be stopped by the gate. There is intentionally no override flag —
the fix is to set `$DATABASE_URL` correctly, not to bypass the check.

Existing tests continue to use their own inline `DROP TABLE` sequences in
LLD-0; retrofitting them to route through this gate happens in LLD-3
alongside the persistence-port work. **Nothing in `src/` depends on this
header** — it is a test-only utility.

## Test-suite results (fresh Release build)

### Repro

```bash
cd build_release
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
# From the repo ROOT (so schema-file paths resolve for any test still using them):
DATABASE_URL='postgresql://chessbench:chessbench@127.0.0.1:5432/chess_bench' \
JWT_SIGNING_KEY='<32-byte base64 key>' \
bash /path/to/scratchpad/run_all_tests.sh
```

### Result table

Two categories: **CLEAN** = every assertion passed; **FLAKE** = documented
pre-existing failure below.

| Binary                     | Status | Passing | Note |
|----------------------------|--------|--------:|------|
| `test_board`               | CLEAN  |  36/36  |      |
| `test_move_gen`            | CLEAN  |  15/15  | Perft to depth 4 startpos, depth 3 Kiwipete. **README claim of depth 5 is aspirational — not tested.** |
| `test_notation`            | CLEAN  |  29/29  |      |
| `test_game_room`           | CLEAN  |  32/32  |      |
| `test_matchmaker`          | CLEAN  |  19/19  |      |
| `test_engine`              | FLAKE  |  23/24  | See §Known failure 1. |
| `test_transposition_table` | CLEAN  |   7/7   |      |
| `test_crypto_env`          | CLEAN  |  11/11  |      |
| `test_hash`                | CLEAN  |  42/42  |      |
| `test_aes`                 | CLEAN  |  28/28  |      |
| `test_pqc`                 | CLEAN  |  40/40  |      |
| `test_sealed_envelope`     | CLEAN  |  44/44  |      |
| `test_database`            | CLEAN  |  14/14  | Was previously failing (crash) — see §Fixes below. |
| `test_schema_phase8`       | CLEAN  |   9/9   |      |
| `test_password`            | CLEAN  |  24/24  | Was previously 21/3 — see §Fixes below. |
| `test_token`               | CLEAN  |  32/32  | Was previously crashing — see §Fixes below. |
| `test_oauth`               | CLEAN  |  30/30  | Was previously 22/8 — see §Fixes below. |
| `test_rate_limiter`        | CLEAN  |  13/13  |      |
| `test_json_escaping`       | CLEAN  |  10/10  |      |
| `test_ai_integration`      | CLEAN  |   9/9   |      |
| `test_elo`                 | CLEAN  |  12/12  |      |
| `test_repositories`        | FLAKE  |  26/28  | See §Known failure 2. |
| `test_game_persistence`    | CLEAN  |  14/14  |      |
| `test_auth_required`       | CLEAN  |   8/8   |      |
| `test_spectator`           | CLEAN  |   9/9   |      |
| `test_replay`              | CLEAN  |   9/9   |      |
| `test_anti_cheat`          | CLEAN  |  22/22  |      |
| `test_tournament`          | CLEAN  |  27/27  |      |

**Totals:** 26 test binaries clean; 2 with a documented flake. **No unaccounted regression.**

### Fixes landed in LLD-0

Two classes of setup bugs were surfaced by the baseline run and fixed in
this phase — both were long-standing, both were masked by the fact that
earlier work ran the tests from the repo root rather than from `build_release/`.

1. **Missing `CHESS_SOURCE_DIR` on four DB-backed test targets.** `test_database`,
   `test_password`, `test_token`, `test_oauth` read `src/storage/schema_phase7.sql`
   at a *relative* path with no compile-time prefix. Running them from
   `build_release/` gave `read_file()` an empty string, and the subsequent
   `db.run_script("")` reported "schema load failed" (with an empty error
   message) followed by the first assertion crashing on missing tables.
   Fix: add `target_compile_definitions(<test> PRIVATE CHESS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")`
   to each of the four CMake targets, and change each test's
   `DEFAULT_SCHEMA` constant to prefix the path with the macro when defined.
   This matches how `test_schema_phase8`, `test_auth_required`, and every
   other more recent DB-backed test target does it.
2. **Stale `DROP TABLE` list in `test_password`, `test_token`, `test_oauth` `reset()`.**
   The three tests were written in Phase 7 before Phase 8 added `games` /
   `move_times` and Phase 9 added `cheat_reports` / tournament tables. The
   `reset()` function only dropped `sessions`, `players`, and the trigger
   function. Because `players` is referenced by every table Phase 8 and
   Phase 9 added, `DROP TABLE IF EXISTS players` failed silently on the
   FK dependency, leaving the schema half-loaded. Subsequent
   `CREATE TABLE IF NOT EXISTS` NOTICEs kept the run alive until a query
   returned zero rows and `.at(0)` crashed. Fix: broaden the drop list to
   match the more complete sequence used by `test_database` / `test_auth_required`.

Both fixes are test-support changes only — no `src/` code path is affected.

### Known failure 1 — `test_engine` "Start pos near 0" (line 227)

- **Assertion:** `std::abs(result.score) < 100` after `engine.search(500)` from the starting position.
- **Observed under Release:** score is roughly 157 cp; assertion trips.
- **Root cause:** the search reaches deep enough at 500 ms under `-O2`
  that it truncates a still-active tactic. Without quiescence, the leaf
  evaluation returns the transient material delta instead of a resolved
  score. CLAUDE.md documents this as Plan Deviation #7.
- **Not a regression.** Pre-existed before LLD-0; reproduces at the same commit.
- **Intended fix:** replace with a depth-bounded assertion (deterministic across
  machines) plus a separate time-budget test that only checks return
  correctness, not score. Deferred out of LLD-0 to keep the phase's
  code footprint honest.

### Known failure 2 — `test_repositories` two planner assertions

- **Line 697:** "Leaderboard after mass games uses idx_players_elo".
- **Line 728:** "get_player_games uses game indexes".
- **What they assert:** that an `EXPLAIN` plan for a specific read query
  mentions a specific index name.
- **Root cause:** Postgres's cost-based planner may correctly choose a
  Seq Scan (or Bitmap Scan) over the 50-row test data set. That is not a
  bug in the schema — the indexes exist (`test_schema_phase8` confirms the
  same-family index is picked at 1 000 rows). It is a threshold-of-planner
  choice at small row counts.
- **Not a regression.** Dataset-size-sensitive since introduction.
- **Intended fix:** convert into (a) a `pg_indexes` existence check and (b) a
  large-dataset planner-usage test flagged so it only runs when a large
  test data set is set up. Per the plan doc: *"do not force a plan merely
  to make tests green."*

### Enumerated known failures (before LLD-1 starts)

1. **`test_engine` — "Start pos near 0"**
   - **Test line:** `test_engine.cpp:227` — asserts `abs(score) < 100` after `engine.search(500)` from the starting position.
   - **Symptom under Release:** returns ~157 cp, fails.
   - **Root cause:** the search reaches deep enough at 500 ms under `-O2` that it truncates a still-active tactic. Without quiescence, the search-boundary evaluation returns the transient material imbalance instead of a resolved score. CLAUDE.md documents this as Plan Deviation #7.
   - **Not a regression.** The failure was recorded before LLD-0 started and reproduces at the same commit.
   - **LLD-0 disposition:** leave in place, note here. A depth-bounded replacement + separate time-budget coverage is the intended fix; per the plan, done in a targeted commit after baseline capture, not silently during another phase.

2. **`test_repositories` — "Leaderboard after mass games uses idx_players_elo"** (line 697)
   and **"get_player_games uses game indexes"** (line 728)
   - **What they test:** EXPLAIN plan for a read query must mention the target index by name.
   - **Root cause:** Postgres's cost-based planner may correctly choose a Seq Scan or a Bitmap Scan over the 50-row test dataset. Small datasets are below the planner's threshold for using a btree index; ANALYZE does not change that when the row count is genuinely low.
   - **Not a regression.** These have been "not always green" since the tests were added; they are dataset-size-sensitive rather than deterministic.
   - **LLD-0 disposition:** leave in place, note here. Plan-doc guidance is explicit: *"verify index existence and intended query behavior separately from cost-based planner choice; do not force a plan merely to make tests green."* The correct fix is to test `pg_indexes` presence + a functional correctness check, and drop the EXPLAIN assertion (or move it behind a large-dataset flag).

## Engine + protocol baseline numbers

### Engine search (`bench_engine`, three consecutive warm runs, medians)

| Position    | Budget | Depth reached | Median NPS |
|-------------|-------:|--------------:|-----------:|
| startpos    |    2 s |             6 |    ~155 K  |
| startpos    |    5 s |             6 |    ~148 K  |
| startpos    |   10 s |             7 |    ~150 K  |
| kiwipete    |    2 s |             6 |    ~119 K  |
| kiwipete    |    5 s |             6 |    ~115 K  |
| kiwipete    |   10 s |             7 |    ~120 K  |
| middlegame  |    2 s |             5 |    ~144 K  |
| middlegame  |    5 s |             6 |    ~147 K  |
| middlegame  |   10 s |             6 |    ~149 K  |

**Caveat on absolute numbers.** WSL2 NPS in this repo swings by up to
2.5× depending on the CPU governor state at the time of measurement.
Earlier this week the same binary on the same machine reported
~380–390 K NPS steady-state (see `docs/BENCHMARKS.md`); today's warm run
reports ~120–160 K. Both are honest measurements of the same code; the
Windows host was under different load. **Comparing across LLD phases must
use samples collected within the same session,** not against a
different-day reading.

### Move generation coverage

Perft tests currently green:

| Position          | Depth | Node count target |
|-------------------|------:|------------------:|
| Starting position |     4 | 197 281           |
| Kiwipete          |     3 |  97 862           |
| Position 3        |     4 |  43 238           |
| Position 4        |     3 |   9 467           |

The plan doc and `README.md` mention perft depth 5 (starting position:
4 865 609). **That depth is not in the test suite.** LLD-0 records this
as a documentation-vs-implementation gap; the actual depths above are the
correctness ground truth.

### Protocol latency baseline

The load-test numbers in `docs/BENCHMARKS.md` (Phase 10.1) remain the
reference. They were recorded from a fresh session and are not
re-measured here because the WSL2 host variance above would make a
one-shot re-run misleading. When any LLD phase changes routing or
handler code, a same-session before/after via `tools/load_test.py`
is the fair comparison.

## Protocol / security checklist for later phases to preserve

Every LLD phase after this must preserve these behaviors. When later phases
change the code that produces them, they must show a test that continues
to enforce the same behavior.

- [ ] Sealed-only routes reject any unsealed arrival (`register`, `login`, `google_auth` when configured).
- [ ] Missing/expired/tampered access token on the four game-starting routes returns `{"type":"error","code":"auth_required"}`.
- [ ] Rate limiter fires **before** any expensive work (Argon2id, JWT verify, key generation).
- [ ] `on_player_disconnect` performs all three cleanups: matchmaker dequeue, room disconnect, spectator sweep.
- [ ] `save_completed_game` remains a single SQL transaction covering game + move_times + ratings + stats.
- [ ] AI games are excluded from persistence.
- [ ] Sealed-envelope confidentiality: no `password`, no decrypted plaintext, and no bearer/refresh token appears in any log line at any level.
- [ ] Frame-size cap enforced by the WebSocket parser before allocation, not just by handler input validation.
- [ ] Every `move_made` broadcast carries a valid FEN string reflecting the post-move position.
- [ ] `game_over` fires exactly once per terminal transition (no duplicates on retry; no missing broadcast on successful save-failure).
