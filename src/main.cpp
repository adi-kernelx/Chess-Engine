#include "core/logger.h"
#include "net/tcp_server.h"
#include "concurrent/thread_pool.h"
#include "game/game_handler.h"
#include "game/room_manager.h"
#include "game/matchmaker.h"
#include <csignal>
#include <atomic>

chess::net::TcpServer* g_server = nullptr;
chess::concurrent::ThreadPool* g_pool = nullptr;

void signal_handler(int signum) {
    chess::core::Logger::info("main", "signal", "Received signal " + std::to_string(signum) + ", shutting down...");
    if (g_server) {
        g_server->stop();
    }
}

int main() {
    chess::core::Logger::init(chess::core::LogLevel::DEBUG);
    chess::core::Logger::info("main", "startup", "=== Multiplayer Chess Platform ===");
    chess::core::Logger::info("main", "startup", "Server starting...");
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create thread pool with hardware_concurrency() workers
    chess::concurrent::ThreadPool pool;
    g_pool = &pool;

    chess::net::TcpServer server(9000, pool);
    g_server = &server;

    // ── Game Layer Setup ──
    chess::game::RoomManager room_mgr;
    chess::game::Matchmaker matchmaker(room_mgr);
    chess::game::GameHandler game_handler(room_mgr, matchmaker);

    // Give the GameHandler the ability to look up connections by fd
    game_handler.set_connection_lookup([&server](int fd) -> chess::net::Connection* {
        return server.get_connection(fd);
    });

    // Set the match callback — notify both players when matched
    matchmaker.set_match_callback([&server](const chess::game::MatchResult& match) {
        chess::core::Logger::info("game", "Matchmaker",
            "Match found! " + match.white_name + " vs " + match.black_name +
            " → Game " + std::to_string(match.game_id));

        // Build match_found messages for both players
        std::string white_msg = "{\"type\":\"match_found\",\"game_id\":" +
            std::to_string(match.game_id) + ",\"color\":\"white\",\"opponent\":\"" +
            match.black_name + "\",\"white_time\":" +
            std::to_string(match.time_control.base_time_ms) + ",\"black_time\":" +
            std::to_string(match.time_control.base_time_ms) + "}";

        std::string black_msg = "{\"type\":\"match_found\",\"game_id\":" +
            std::to_string(match.game_id) + ",\"color\":\"black\",\"opponent\":\"" +
            match.white_name + "\",\"white_time\":" +
            std::to_string(match.time_control.base_time_ms) + ",\"black_time\":" +
            std::to_string(match.time_control.base_time_ms) + "}";

        auto* white_conn = server.get_connection(match.white_fd);
        auto* black_conn = server.get_connection(match.black_fd);

        if (white_conn) {
            chess::net::WebSocket::write_frame(*white_conn, chess::net::WsOpcode::TEXT, white_msg);
            while (white_conn->has_data_to_write()) {
                if (white_conn->write_to_socket() <= 0) break;
            }
        }
        if (black_conn) {
            chess::net::WebSocket::write_frame(*black_conn, chess::net::WsOpcode::TEXT, black_msg);
            while (black_conn->has_data_to_write()) {
                if (black_conn->write_to_socket() <= 0) break;
            }
        }
    });

    // Register all game message handlers on the WebSocket router
    game_handler.register_handlers(server.get_router());

    // Handle player disconnections — cleanup queue + room state
    server.set_disconnect_callback([&game_handler](int fd) {
        game_handler.on_player_disconnect(fd);
    });

    // Set default handler for unrecognized message types
    server.get_router().set_default_handler(
        [](chess::net::Connection& conn, const std::string& message) {
            chess::core::Logger::warn("main", "unknown", "Unrecognized message: " + message);
            chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT,
                "{\"type\":\"error\",\"message\":\"Unknown message type\"}");
        }
    );

    if (server.start()) {
        chess::core::Logger::info("main", "startup", "Server ready. Waiting for connections on ws://localhost:9000");
        server.run();
    } else {
        chess::core::Logger::error("main", "startup", "Failed to start server");
        return 1;
    }

    // Shut down the pool after the event loop exits.
    // This waits for all in-flight tasks to complete.
    pool.shutdown();
    
    chess::core::Logger::info("main", "shutdown", "Server exited gracefully");
    return 0;
}
