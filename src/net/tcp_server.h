#pragma once

#include "connection.h"
#include "websocket.h"
#include "../concurrent/thread_pool.h"
#include <string>
#include <memory>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <sys/epoll.h>

namespace chess {
namespace net {

class TcpServer {
public:
    TcpServer(uint16_t port, concurrent::ThreadPool& pool);
    ~TcpServer();

    // Disable copy
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start();
    void stop();
    
    // Main event loop
    void run();

    // Access the message router to register handlers from outside
    MessageRouter& get_router() { return router_; }

    /// Look up a connection by fd. Returns nullptr if not found.
    /// Caller must be aware this holds the connections mutex briefly.
    Connection* get_connection(int fd);

    /// Set a callback that fires when a connection disconnects.
    /// Used by the game layer to handle player disconnections.
    using DisconnectCallback = std::function<void(int fd)>;
    void set_disconnect_callback(DisconnectCallback cb) { disconnect_cb_ = std::move(cb); }

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

    concurrent::ThreadPool& pool_;
    MessageRouter router_;
    std::recursive_mutex connections_mutex_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    DisconnectCallback disconnect_cb_;
    static const int MAX_EVENTS = 64;
};

} // namespace net
} // namespace chess

