#include "core/logger.h"
#include "net/tcp_server.h"
#include "concurrent/thread_pool.h"
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
    chess::core::Logger::info("main", "startup", "Server starting...");
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create thread pool with hardware_concurrency() workers
    chess::concurrent::ThreadPool pool;
    g_pool = &pool;

    chess::net::TcpServer server(9000, pool);
    g_server = &server;

    // Register a default handler that echoes messages back (for testing).
    // In later phases, we'll replace this with game-specific handlers.
    server.get_router().set_default_handler(
        [](chess::net::Connection& conn, const std::string& message) {
            chess::core::Logger::info("main", "echo", "Echoing: " + message);
            chess::net::WebSocket::write_frame(conn, chess::net::WsOpcode::TEXT, message);
        }
    );

    if (server.start()) {
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
