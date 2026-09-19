/**
 * application/game_query_service.h — the "what to do" for the query,
 * spectator, and replay family (LLD-2.2).
 *
 * SEVEN ROUTES land here — every one that used to live on `GameHandler`
 * in the read/browse column:
 *
 *      get_profile / get_leaderboard / get_history      (DB browse)
 *      get_game                                         (replay reconstruction)
 *      list_live_games / spectate / stop_spectating     (live spectator)
 *
 * WHAT THIS CLASS OWNS
 *   - Read-only DB queries: player profiles, leaderboards, per-player
 *     history, single-game replay payloads.
 *   - Room-manager coordination for spectator add/remove and live-game
 *     listings.
 *   - Board reconstruction for the replay payload — the UCI move string
 *     is replayed through `Board::make_move` against the legal-move list
 *     to recover fully-flagged Moves (castle / en-passant / promotion).
 *
 * WHAT IT DOES NOT OWN
 *   - `nlohmann::json::parse()` on the incoming frame. The `QueryHandler`
 *     adapter parses; the service takes structured inputs.
 *   - Auth. Only `spectate` needs a proven identity (see LLD-1: mandatory
 *     auth for spectating). The handler runs `IdentityExtractor::extract`
 *     up-front and passes the snapshot in; every other route here is a
 *     public read.
 *   - Fan-out. Spectator broadcasts on live games are the gameplay
 *     service's job — this class only manages the subscription set.
 *
 * WHAT IT PRAGMATICALLY STILL DOES
 *   - Emits JSON frames inline via `nlohmann::json`. Every one of the
 *     seven responses is a legacy shape (predates `protocol::codec`);
 *     inventing typed DTOs for each is LLD-3+ scope. Emissions still
 *     flow through the shared `MessageSink` boundary, so the transport
 *     is fake-substitutable in tests.
 *
 * THREADING
 *   Each service method runs on the worker thread that decoded the
 *   frame. `RoomManager` and `Database` synchronise themselves; nothing
 *   in this class introduces new shared state.
 */

#pragma once

#include <cstdint>
#include <string>

#include "application/ports/message_sink.h"
#include "application/ports/player_queries.h"
#include "application/request_context.h"
#include "application/result.h"
#include "game/room_manager.h"

namespace chess::application {

class GameQueryService {
public:
    /// Both references must outlive the service. In capability-disabled
    /// mode the composition root injects `NullPlayerQueries`; DB-backed
    /// routes then hit the port, get a `Disconnected` outcome, and
    /// emit the pre-refactor "unavailable" wire error string.
    GameQueryService(chess::game::RoomManager&                 rooms,
                     chess::application::ports::PlayerQueries& queries);

    // ── DB browse (no auth) ────────────────────────────────────────

    void get_profile    (const RequestContext& ctx,
                         const std::string&    username,
                         int                   offset,
                         MessageSink&          caller_sink);

    void get_leaderboard(const RequestContext& ctx,
                         int                   limit,
                         int                   offset,
                         MessageSink&          caller_sink);

    void get_history    (const RequestContext& ctx,
                         const std::string&    username,
                         int                   limit,
                         MessageSink&          caller_sink);

    // ── Replay (no auth) ───────────────────────────────────────────

    void get_game       (const RequestContext& ctx,
                         int64_t               game_id,
                         MessageSink&          caller_sink);

    // ── Live spectator ─────────────────────────────────────────────

    void list_live_games(const RequestContext& ctx,
                         MessageSink&          caller_sink);

    /// Auth required — handler proved identity via IdentityExtractor
    /// before calling. `spectator_username` is the resolved handle used
    /// for the log line, matching the pre-refactor "<user> is spectating
    /// game N" format exactly.
    void spectate       (const RequestContext& ctx,
                         const std::string&    spectator_username,
                         int64_t               game_id,
                         MessageSink&          caller_sink);

    void stop_spectating(const RequestContext& ctx,
                         int64_t               game_id,
                         MessageSink&          caller_sink);

private:
    chess::game::RoomManager&                 rooms_;
    chess::application::ports::PlayerQueries& queries_;
};

} // namespace chess::application
