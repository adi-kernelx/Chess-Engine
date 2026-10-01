/**
 * game_handler.h — WebSocket API registration facade.
 *
 * As of LLD-2.4 this class is a PURE REGISTRATION FACADE. Every route
 * has moved into its own service + handler pair:
 *
 *   gameplay + matchmaking (LLD-2.1)   → GameplayService  + GameplayHandler
 *   query / spectator / replay (2.2)   → GameQueryService + QueryHandler
 *   analysis (2.3)                     → AnalysisService  + AnalysisHandler
 *   tournaments (2.4)                  → TournamentService + TournamentHandler
 *
 * `register_handlers` builds those four (service, handler) pairs and
 * asks each to install its routes on the router.
 * `on_player_disconnect` forwards to the gameplay handler (which owns
 * the room + matchmaker + spectator sweep).
 * `send_json_to_fd` and `broadcast_to_spectators` are the fan-out
 * callables that GameplayService receives at construction —
 * ForeignSender and SpectatorBroadcaster respectively.
 *
 * WebSocket JSON API (unchanged from the pre-refactor contract):
 *
 *   Client → Server:
 *     { "type": "create_game",     "access_token": "…", "time_base": 600, "time_inc": 5 }
 *     { "type": "join_game",       "access_token": "…", "game_id": 1 }
 *     { "type": "make_move",       "from": "e2", "to": "e4", "promotion": "q" }
 *     { "type": "resign" }
 *     { "type": "quick_play",      "access_token": "…", "time_base": 600, "time_inc": 5 }
 *     { "type": "cancel_queue" }
 *     { "type": "play_ai",         "access_token": "…", "difficulty": "medium", "time_base": 600, "time_inc": 5 }
 *     { "type": "list_games" }
 *     { "type": "game_state",      "access_token": "…" }
 *     { "type": "get_active_game", "access_token": "…" }
 *     { "type": "list_live_games" }                                     // Phase 9.1
 *     { "type": "spectate",        "access_token": "…", "game_id": 1 }  // Phase 9.1
 *     { "type": "stop_spectating", "game_id": 1 }                       // Phase 9.1
 *
 *   Auth-required routes (create_game/join_game/quick_play/play_ai/game_state/
 *   get_active_game/spectate/
 *   analyze_game/create_tournament/join_tournament/start_tournament/
 *   report_tournament_result) reply with:
 *     { "type": "error", "code": "auth_required", "message": "…" }
 *   on any auth failure (no wiring, missing token, bad signature, revoked,
 *   or player deleted).
 *
 *   Every response frame from these families is unchanged bit-for-bit;
 *   see BENCHMARKS.md / test suite for wire fixtures.
 *
 *   Phase 9.2 client → server (additional):
 *     { "type": "get_game",          "game_id": 1 }
 *     { "type": "get_history",       "username": "Alice", "limit": 20 }
 *     { "type": "analyze_position",  "fen": "…", "depth": 8 }
 *
 *   Phase 9.3 client → server (additional):
 *     { "type": "analyze_game",      "game_id": 1 }
 *
 *   Phase 9.4 client → server (additional):
 *     { "type": "create_tournament" | "join_tournament" | "start_tournament"
 *              | "tournament_state" | "list_tournaments"
 *              | "report_tournament_result", … }
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "application/analysis_service.h"
#include "application/game_completion_service.h"
#include "application/game_query_service.h"
#include "application/gameplay_service.h"
#include "application/ports/game_store.h"
#include "application/ports/player_queries.h"
#include "application/tournament_service.h"
#include "application/tournament_runtime_service.h"
#include "game/ai_player.h"
#include "game/handlers/analysis_handler.h"
#include "game/handlers/gameplay_handler.h"
#include "game/handlers/query_handler.h"
#include "game/handlers/tournament_handler.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"
#include "net/connection.h"
#include "net/websocket.h"
#include "protocol/request_pipeline.h"
#include "storage/database.h"

namespace chess {
namespace game {

class GameHandler {
public:
    GameHandler(RoomManager& room_mgr, Matchmaker& matchmaker);

    /// Register every game-family message handler on the shared
    /// pipeline. Must be called AFTER `set_connection_lookup`,
    /// `set_game_store`, and `set_player_queries`. The pipeline is
    /// owned by the composition root (main.cpp / tests) — LLD-5.3
    /// hoisted it there so AuthHandler and GameHandler can share.
    void register_handlers(protocol::RequestPipeline& pipeline);

    /// Called by the transport when a socket closes. Forwards to the
    /// gameplay service (room+queue cleanup); the unmigrated families
    /// currently have no disconnect hook.
    void on_player_disconnect(int connection_fd);

    /// Called by the transport maintenance tick.
    void expire_disconnected_games();

    /// Setters used by main during composition.
    void set_connection_lookup(std::function<net::Connection*(int fd)> lookup) {
        connection_lookup_ = std::move(lookup);
    }
    void set_database(storage::Database* db) { db_ = db; }
    /// Set the GameStore port (LLD-3.2). Must be non-null before
    /// `register_handlers` — in capability-disabled mode pass a
    /// `NullGameStore`; the port's `capable()` method distinguishes.
    void set_game_store(application::ports::GameStore* store) { game_store_ = store; }
    /// Set the PlayerQueries port (LLD-3.3). Must be non-null before
    /// `register_handlers` — same capability-disabled composition
    /// rule as `set_game_store`.
    void set_player_queries(application::ports::PlayerQueries* q) { player_queries_ = q; }

private:
    // ── Un-migrated family handlers (LLD-2.2 / 2.3 / 2.4 targets) ──

    // ── Helpers used by the injected fan-out callables ──

    /// WebSocket write to a foreign fd (opponent seat, spectator,
    /// notification target). Resolves the current Connection via
    /// `connection_lookup_`, writes the frame, then drains the write
    /// buffer inline — EPOLLET only notifies once when the socket
    /// becomes writable, so a foreign-fd send without an inline flush
    /// would sit in RAM until the recipient wrote back. Both
    /// GameplayService's ForeignSender and its SpectatorBroadcaster
    /// funnel through this.
    void send_json_to_fd(int fd, const std::string& json);
    void broadcast_to_spectators(GameRoom& room, const std::string& json_str);

    // ── Data ──

    RoomManager&  room_mgr_;
    Matchmaker&   matchmaker_;
    AIPlayer      ai_player_;

    storage::Database*                 db_             = nullptr;
    application::ports::GameStore*     game_store_     = nullptr;
    application::ports::PlayerQueries* player_queries_ = nullptr;

    std::function<net::Connection*(int fd)> connection_lookup_;

    // ── LLD-2.1 objects, constructed on register_handlers ──

    std::unique_ptr<application::GameplayService>         gameplay_service_;
    std::unique_ptr<handlers::GameplayHandler>            gameplay_handler_;

    // ── LLD-2.2 objects, constructed on register_handlers ──

    std::unique_ptr<application::GameQueryService>        query_service_;
    std::unique_ptr<handlers::QueryHandler>               query_handler_;

    // ── LLD-2.3 objects, constructed on register_handlers ──

    std::unique_ptr<application::AnalysisService>         analysis_service_;
    std::unique_ptr<handlers::AnalysisHandler>            analysis_handler_;

    // ── LLD-2.4 objects, constructed on register_handlers ──

    std::unique_ptr<application::TournamentService>       tournament_service_;
    std::unique_ptr<application::TournamentRuntimeService> tournament_runtime_service_;
    std::unique_ptr<handlers::TournamentHandler>          tournament_handler_;

    // ── LLD-4.2 objects, constructed on register_handlers ──
    //
    // Held as a shared_ptr because `RoomManager::set_default_listener`
    // stores a `GameEventListenerPtr` (== `shared_ptr<GameEventListener>`)
    // and every room the manager creates thereafter copies that pointer
    // into its own listener list.
    std::shared_ptr<application::GameCompletionService>   completion_service_;
};

} // namespace game
} // namespace chess
