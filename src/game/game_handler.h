/**
 * game_handler.h — WebSocket API handler.
 *
 * As of LLD-2.1 this class is transitioning to a REGISTRATION FACADE.
 * The gameplay + matchmaking family (create_game / join_game / make_move /
 * resign / game_state / quick_play / cancel_queue / list_games / play_ai)
 * has been extracted into `application::GameplayService` +
 * `game::handlers::GameplayHandler`. GameHandler still owns the router
 * hookup for the three families that have not yet been extracted
 * (query/spectator/replay, analysis, tournaments) — they migrate in
 * LLD-2.2, 2.3, and 2.4 respectively.
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
 *     { "type": "game_state" }
 *     { "type": "list_live_games" }                                     // Phase 9.1
 *     { "type": "spectate",        "access_token": "…", "game_id": 1 }  // Phase 9.1
 *     { "type": "stop_spectating", "game_id": 1 }                       // Phase 9.1
 *
 *   Auth-required routes (create_game/join_game/quick_play/play_ai/spectate/
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

#include "application/auth/identity_extractor.h"
#include "application/gameplay_service.h"
#include "auth/token.h"
#include "game/ai_player.h"
#include "game/handlers/gameplay_handler.h"
#include "game/matchmaker.h"
#include "game/room_manager.h"
#include "net/connection.h"
#include "net/websocket.h"
#include "storage/database.h"

namespace chess {
namespace game {

class GameHandler {
public:
    GameHandler(RoomManager& room_mgr, Matchmaker& matchmaker);

    /// Register every message handler on the router. Must be called
    /// AFTER `set_connection_lookup`, `set_database`, and `set_signer`
    /// — see main.cpp for the required ordering. The gameplay family is
    /// delegated to an internally-owned `GameplayHandler`; other
    /// families are still handled by methods on this class pending
    /// LLD-2.2 / 2.3 / 2.4 migrations.
    void register_handlers(net::MessageRouter& router);

    /// Called by the transport when a socket closes. Forwards to the
    /// gameplay service (room+queue cleanup); the unmigrated families
    /// currently have no disconnect hook.
    void on_player_disconnect(int connection_fd);

    /// Setters used by main during composition. All optional — a null
    /// database or signer produces `auth_required` on auth-required
    /// routes rather than silently downgrading. See IdentityExtractor.
    void set_connection_lookup(std::function<net::Connection*(int fd)> lookup) {
        connection_lookup_ = std::move(lookup);
    }
    void set_database(storage::Database* db) { db_ = db; }
    void set_signer(auth::TokenSigner* s)    { signer_ = s; }

private:
    // ── Un-migrated family handlers (LLD-2.2 / 2.3 / 2.4 targets) ──

    // Query / spectator / replay family (LLD-2.2 target)
    void handle_get_profile(net::Connection& conn, const std::string& message);
    void handle_get_leaderboard(net::Connection& conn, const std::string& message);
    void handle_spectate(net::Connection& conn, const std::string& message);
    void handle_stop_spectating(net::Connection& conn, const std::string& message);
    void handle_list_live_games(net::Connection& conn, const std::string& message);
    void handle_get_game(net::Connection& conn, const std::string& message);
    void handle_get_history(net::Connection& conn, const std::string& message);

    // Analysis family (LLD-2.3 target)
    void handle_analyze_position(net::Connection& conn, const std::string& message);
    void handle_analyze_game(net::Connection& conn, const std::string& message);

    // Tournament family (LLD-2.4 target)
    void handle_create_tournament(net::Connection& conn, const std::string& message);
    void handle_join_tournament(net::Connection& conn, const std::string& message);
    void handle_start_tournament(net::Connection& conn, const std::string& message);
    void handle_tournament_state(net::Connection& conn, const std::string& message);
    void handle_list_tournaments(net::Connection& conn, const std::string& message);
    void handle_report_tournament_result(net::Connection& conn, const std::string& message);

    // ── Helpers still used by the un-migrated families ──

    void send_json(net::Connection& conn, const std::string& json);
    void send_json_to_fd(int fd, const std::string& json);
    void broadcast_to_spectators(GameRoom& room, const std::string& json_str);
    static std::string make_error(const std::string& message);
    static std::string status_to_reason(GameStatus status);
    void send_error_code(net::Connection& conn,
                         const std::string& code,
                         const std::string& message);

    /// Legacy identity-extraction path used by the un-migrated families.
    /// A thin wrapper around `application::auth::IdentityExtractor::extract`
    /// that preserves the old out-parameter shape so we don't rewrite
    /// every un-migrated handler in this slice — LLD-2.2/.3/.4 will
    /// switch them to the shared helper directly and delete this shim.
    bool extract_identity(net::Connection& conn,
                          const nlohmann::json& msg,
                          int64_t& out_db_player_id,
                          std::string& out_username,
                          int& out_elo);

    // ── Data ──

    RoomManager&  room_mgr_;
    Matchmaker&   matchmaker_;
    AIPlayer      ai_player_;

    storage::Database*    db_     = nullptr;
    auth::TokenSigner*    signer_ = nullptr;

    std::function<net::Connection*(int fd)> connection_lookup_;

    // ── LLD-2.1 objects, constructed on register_handlers ──

    std::unique_ptr<application::auth::IdentityExtractor> identity_;
    std::unique_ptr<application::GameplayService>         gameplay_service_;
    std::unique_ptr<handlers::GameplayHandler>            gameplay_handler_;
};

} // namespace game
} // namespace chess
