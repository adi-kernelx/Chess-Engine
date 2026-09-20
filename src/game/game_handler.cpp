/**
 * game_handler.cpp — WebSocket JSON API registration facade.
 *
 * As of LLD-2.4 this file is a PURE REGISTRATION FACADE. Every route
 * has moved into a `HandlerClass` + `Service` pair under
 * `src/game/handlers/` and `src/application/`:
 *
 *   gameplay + matchmaking (LLD-2.1)   → gameplay_handler + gameplay_service
 *   query / spectator / replay (2.2)   → query_handler    + game_query_service
 *   analysis (2.3)                     → analysis_handler + analysis_service
 *   tournaments (2.4)                  → tournament_handler + tournament_service
 *
 * What remains here:
 *   - `register_handlers`: constructs the four (service, handler) pairs
 *     and asks each to install its routes on the router.
 *   - `on_player_disconnect`: forwards to the gameplay handler
 *     (which owns the room/queue/spectator sweep).
 *   - `send_json_to_fd` + `broadcast_to_spectators`: the fan-out
 *     callables bound into `GameplayService` at construction.
 */

#include "game/game_handler.h"

#include <cassert>
#include <memory>

namespace chess {
namespace game {

// ============================================================
// Construction & Registration
// ============================================================

GameHandler::GameHandler(RoomManager& room_mgr, Matchmaker& matchmaker)
    : room_mgr_(room_mgr), matchmaker_(matchmaker) {}

void GameHandler::register_handlers(net::MessageRouter& router) {
    // ── LLD-2.1: build and register the gameplay family. ──
    //
    // Both db_ and signer_ MUST already have been set (or explicitly
    // left null when auth is not configured). See main.cpp for ordering.
    identity_ = std::make_unique<application::auth::IdentityExtractor>(
        db_, signer_);

    auto foreign_sender = [this](int fd, const std::string& frame) {
        this->send_json_to_fd(fd, frame);
    };
    auto spectator_broadcaster = [this](GameRoom& room, const std::string& frame) {
        this->broadcast_to_spectators(room, frame);
    };

    // LLD-3.3 invariant: `game_store_` and `player_queries_` are non-null
    // by the time we get here. `main.cpp` wires PostgresGameStore /
    // PostgresPlayerQueries when the DB is available, or NullGameStore /
    // NullPlayerQueries when it is not (capability-disabled mode).
    assert(game_store_     != nullptr);
    assert(player_queries_ != nullptr);

    gameplay_service_ = std::make_unique<application::GameplayService>(
        room_mgr_, matchmaker_, ai_player_,
        std::move(foreign_sender),
        std::move(spectator_broadcaster));

    gameplay_handler_ = std::make_unique<handlers::GameplayHandler>(
        *gameplay_service_, identity_.get(), connection_lookup_);

    gameplay_handler_->register_handlers(router);

    // ── LLD-4.2: register the completion service as RoomManager's ──
    //   default listener. Every room `create_room` / `create_ai_room`
    //   builds from this point on will automatically fire its
    //   `GameCompleted` event into `GameCompletionService`, which owns
    //   the (idempotent) persist path. AI games and unauthenticated
    //   seats short-circuit inside the service — attaching to every
    //   room is a no-op for those cases.
    completion_service_ = std::make_shared<application::GameCompletionService>(
        *game_store_);
    room_mgr_.set_default_listener(completion_service_);

    // ── LLD-2.2: build and register the query / spectator / replay family. ──

    query_service_ = std::make_unique<application::GameQueryService>(
        room_mgr_, *player_queries_);
    query_handler_ = std::make_unique<handlers::QueryHandler>(
        *query_service_, identity_.get(), connection_lookup_);
    query_handler_->register_handlers(router);

    // ── LLD-2.3: build and register the analysis family. ──

    analysis_service_ = std::make_unique<application::AnalysisService>(
        *player_queries_, *game_store_);
    analysis_handler_ = std::make_unique<handlers::AnalysisHandler>(
        *analysis_service_, connection_lookup_);
    analysis_handler_->register_handlers(router);

    // ── LLD-2.4: build and register the tournaments family. ──

    tournament_service_ = std::make_unique<application::TournamentService>(db_);
    tournament_handler_ = std::make_unique<handlers::TournamentHandler>(
        *tournament_service_, identity_.get(), connection_lookup_);
    tournament_handler_->register_handlers(router);
}

void GameHandler::on_player_disconnect(int connection_fd) {
    // Forward to the gameplay service (room + matchmaker + spectator sweep).
    // The un-migrated families currently have no disconnect hook.
    if (gameplay_handler_) {
        gameplay_handler_->on_player_disconnect(connection_fd);
    }
}

// ============================================================
// Fan-out helpers used by GameplayService's injected callables
// ============================================================

void GameHandler::send_json_to_fd(int fd, const std::string& json_str) {
    if (!connection_lookup_) return;
    net::Connection* conn = connection_lookup_(fd);
    if (conn) {
        net::WebSocket::write_frame(*conn, net::WsOpcode::TEXT, json_str);
        // Flush immediately — this connection's handler isn't running,
        // so no one else will flush its write buffer for us.
        while (conn->has_data_to_write()) {
            int written = conn->write_to_socket();
            if (written <= 0) break;  // EAGAIN or error — epoll will retry later
        }
    }
}

void GameHandler::broadcast_to_spectators(GameRoom& room,
                                          const std::string& json_str) {
    // Snapshot first, iterate outside — room mutex must never be held across
    // send_json_to_fd (each send does a syscall + inline flush, which would
    // serialize every other move on this room behind the slowest watcher).
    const auto fds = room.spectator_fds();
    for (int fd : fds) {
        send_json_to_fd(fd, json_str);
    }
}

} // namespace game
} // namespace chess
