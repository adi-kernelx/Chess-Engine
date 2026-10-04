#pragma once

#include "connection.h"
#include "websocket.h"
#include "../concurrent/thread_pool.h"
#include <string>
#include <memory>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <future>
#include <atomic>
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
    // Predicate is polled on the event-loop thread, never in a signal handler.
    void run(std::function<bool()> stop_requested = {});

    // Access the message router to register handlers from outside
    MessageRouter& get_router() { return router_; }

    /// Look up a connection by fd. Returns nullptr if not found.
    /// Caller must be aware this holds the connections mutex briefly.
    Connection* get_connection(int fd);
    /// Atomically look up, enqueue, and flush a foreign frame. Safe for
    /// background maintenance; keeps shared ownership through the send and
    /// takes only the write-buffer lock, never another socket's handler lock.
    void send_text(int fd, const std::string& frame);

    /// Set a callback that fires when a connection disconnects.
    /// Used by the game layer to handle player disconnections.
    using DisconnectCallback = std::function<void(int fd)>;
    void set_disconnect_callback(DisconnectCallback cb) { disconnect_cb_ = std::move(cb); }

    /// Periodic callback offloaded from the socket event loop. Only one pass
    /// may be in flight; run() drains it before returning on shutdown.
    using MaintenanceCallback = std::function<void()>;
    void set_maintenance_callback(MaintenanceCallback cb) { maintenance_cb_ = std::move(cb); }

private:
    bool setup_socket();
    bool set_non_blocking(int fd);
    void handle_new_connection();
    void handle_client_data(const std::shared_ptr<Connection>& connection);
    void close_connection(const std::shared_ptr<Connection>& connection);
    std::shared_ptr<Connection> acquire_connection(int fd);
    void run_connection_maintenance();

    uint16_t port_;
    int server_fd_;
    int epoll_fd_;
    std::atomic<bool> running_;

    concurrent::ThreadPool& pool_;
    MessageRouter router_;
    std::recursive_mutex connections_mutex_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    /// LLD-1: monotonic counter incremented every time we accept() and
    /// stamped onto the new Connection's generation. Combined with fd,
    /// this gives ConnectionHandle a value stable across the whole
    /// process lifetime that fd reuse cannot forge.
    uint64_t next_generation_ = 1;
    DisconnectCallback disconnect_cb_;
    MaintenanceCallback maintenance_cb_;
    std::future<void> maintenance_task_;
    static const int MAX_EVENTS = 64;
    // Browser control-frame replies should be immediate, but background tabs,
    // proxies, and a busy worker pool can delay delivery. A 30+30 window avoids
    // false disconnects while still bounding silent network failure to 60 s.
    static constexpr std::chrono::seconds HEARTBEAT_INTERVAL{30};
    static constexpr std::chrono::seconds HEARTBEAT_TIMEOUT{30};
};

} // namespace net
} // namespace chess

