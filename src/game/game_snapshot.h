/**
 * game/game_snapshot.h — an immutable view of a completed game (LLD-4.1).
 *
 * WHY THIS EXISTS
 *
 * When a game ends today, three things need the terminal state — the
 * persist path, the participant-notification path, and the spectator-
 * broadcast path. All three currently reach back into `GameRoom` to
 * read whatever they need. That creates two problems:
 *
 *   1. Each path calls its own accessors under the room mutex,
 *      which means they see three subtly different snapshots if the
 *      room mutates between calls (e.g. a spectator joins). No user
 *      has hit this yet — the terminal transition is quick — but the
 *      invariant is a landmine.
 *
 *   2. The persist path takes a `GameRoom*` and knows too much about
 *      the room's shape (`get_move_history` returning `MoveRecord`,
 *      `get_time_control` returning `TimeControl`, etc.). That
 *      coupling stops anything below the room from being tested with
 *      a fake room.
 *
 * `GameSnapshot` fixes both. It is a value type built once, under the
 * room mutex, at the moment of the terminal transition, and handed to
 * every consumer as a `const&`. Consumers never re-read the room; the
 * snapshot IS the terminal state.
 *
 * WHAT IT CARRIES
 *
 * Every field the pre-LLD-4 `GameplayService::persist_game` reached
 * into `GameRoom` to fetch, plus a few name/id fields the notification
 * paths use. Move history is included (small — median game is ~40
 * plies) because both persistence and replay want it. Spectator fds
 * are NOT here — the spectator fan-out sees the SAME snapshot for
 * every subscriber but selects recipients from the room's current
 * spectator set, which continues to be read live.
 *
 * SIZE + LIFETIME
 *
 * ~200 bytes plus the move history and player names — small enough to
 * pass by value if we wanted, but every listener takes it by const&
 * to keep the callback signature stable. Snapshots have no lifetime
 * dependency on `GameRoom`; a listener can outlive the room and still
 * hold a valid snapshot.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"       // GameStatus, GameId, PlayerId, Color
#include "game/game_room.h"   // MoveRecord, TimeControl

namespace chess {
namespace game {

/// Immutable per-color view of a seat at the moment the game ended.
struct PlayerSnapshot {
    int64_t     db_player_id = 0;   ///< 0 = unauthenticated
    PlayerId    player_id    = 0;   ///< Local atomic id
    std::string username;
    int         elo          = 1200;  ///< snapshot at game start (not post-game)
    int         remaining_ms = 0;
};

struct GameSnapshot {
    // ── Identity ──────────────────────────────────────────────
    GameId       room_id     = 0;
    /// Stable idempotency key derived at snapshot-build time. Used by
    /// LLD-4.2's completion service to make persist retriable without
    /// creating duplicate rows. Populated as UUID v4 (36 chars).
    std::string  completion_uuid;
    bool         is_ai_game  = false;

    /// LLD-6.4: monotonically-increasing revision of the source room
    /// at the moment this snapshot was taken. Every mutating GameRoom
    /// operation bumps the room's revision; a consumer that queues a
    /// long-running job with a snapshot in hand can re-read the room's
    /// current revision before applying the result and discard if it
    /// has advanced. No consumer uses it today — the current
    /// serialized path never produces a stale snapshot — but async
    /// move-selection landing in a later slice needs this field to
    /// implement "late results after resign/disconnect/state change
    /// must be discarded" (plan §6.4).
    uint64_t     revision    = 0;

    // ── Outcome ───────────────────────────────────────────────
    GameStatus   status      = GameStatus::ONGOING;  ///< the terminal status
    std::string  result;                              ///< "1-0" / "0-1" / "1/2-1/2"
    std::string  termination_reason;                  ///< status_to_reason(status)

    // ── Seats ─────────────────────────────────────────────────
    PlayerSnapshot white;
    PlayerSnapshot black;

    // ── Timing ────────────────────────────────────────────────
    TimeControl  time_control;
    std::string  started_at_iso;
    std::string  ended_at_iso;

    // ── Move history ──────────────────────────────────────────
    std::vector<MoveRecord> history;
    int          move_count  = 0;  ///< history.size(), duplicated for lookup speed
};

} // namespace game
} // namespace chess
