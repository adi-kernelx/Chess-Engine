#pragma once

#include "connection.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <sys/epoll.h>

namespace chess {
namespace net {

class TcpServer {
public:
    TcpServer(uint16_t port);
    ~TcpServer();

    // Disable copy
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start();
    void stop();
    
    // Main event loop
    void run();

private:
    bool setup_socket();
    bool set_non_blocking(int fd);
    void handle_new_connection();
    void handle_client_data(int client_fd);
    void close_connection(int client_fd);

    uint16_t port_;
    int server_fd_;
    int epoll_fd_;
    bool running_;

    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    static const int MAX_EVENTS = 64;
};

} // namespace net
} // namespace chess
