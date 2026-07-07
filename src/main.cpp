#include "core/logger.h"
#include "net/tcp_server.h"
#include <csignal>
#include <atomic>

std::atomic<bool> g_stop_server{false};
chess::net::TcpServer* g_server = nullptr;

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

    chess::net::TcpServer server(9000);
    g_server = &server;

    if (server.start()) {
        server.run();
    } else {
        chess::core::Logger::error("main", "startup", "Failed to start server");
        return 1;
    }
    
    chess::core::Logger::info("main", "shutdown", "Server exited gracefully");
    return 0;
}
