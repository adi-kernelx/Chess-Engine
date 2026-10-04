#include "tcp_server.h"
#include "../core/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <vector>

namespace chess {
namespace net {

namespace {

bool contains_sensitive_json(const std::string& message) {
    return message.find("\"password\"")      != std::string::npos ||
           message.find("\"access_token\"")  != std::string::npos ||
           message.find("\"refresh_token\"") != std::string::npos ||
           message.find("\"supabase_jwt\"")   != std::string::npos ||
           message.find("\"sealed\"")         != std::string::npos;
}

} // namespace

TcpServer::TcpServer(uint16_t port, concurrent::ThreadPool& pool) 
    : port_(port), server_fd_(-1), epoll_fd_(-1), running_(false), pool_(pool) {}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) != -1;
}

bool TcpServer::setup_socket() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        core::Logger::error("net", "TcpServer", "Failed to create socket");
        return false;
    }

    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        core::Logger::error("net", "TcpServer", "Failed to set SO_REUSEADDR");
        return false;
    }

    if (!set_non_blocking(server_fd_)) {
        core::Logger::error("net", "TcpServer", "Failed to set non-blocking on server socket");
        return false;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        core::Logger::error("net", "TcpServer", "Bind failed on port " + std::to_string(port_) + ": " + strerror(errno));
        return false;
    }

    if (listen(server_fd_, SOMAXCONN) < 0) {
        core::Logger::error("net", "TcpServer", "Listen failed: " + std::string(strerror(errno)));
        return false;
    }

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        core::Logger::error("net", "TcpServer", "epoll_create1 failed");
        return false;
    }

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET; // Edge-triggered
    event.data.fd = server_fd_;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &event) < 0) {
        core::Logger::error("net", "TcpServer", "epoll_ctl failed for server_fd");
        return false;
    }

    return true;
}

bool TcpServer::start() {
    if (!setup_socket()) return false;
    running_ = true;
    core::Logger::info("net", "TcpServer", "Server listening on port " + std::to_string(port_));
    return true;
}

void TcpServer::stop() {
    running_ = false;
    {
        std::lock_guard<std::recursive_mutex> lock(connections_mutex_);
        connections_.clear();
    }
    
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
    core::Logger::info("net", "TcpServer", "Server stopped");
}

void TcpServer::handle_new_connection() {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                core::Logger::error("net", "TcpServer", "Accept error: " + std::string(strerror(errno)));
                break;
            }
        }

        if (!set_non_blocking(client_fd)) {
            close(client_fd);
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);

        {
            std::lock_guard<std::recursive_mutex> lock(connections_mutex_);
            auto conn = std::make_shared<Connection>(client_fd, std::string(ip_str));
            // LLD-1: stamp a fresh, never-reused generation on this
            // connection. A stale ConnectionHandle for the same fd but
            // an earlier generation now compares unequal to the current
            // one, so a foreign send scheduled before the previous
            // socket closed cannot land on this new client.
            conn->set_generation(next_generation_++);
            connections_[client_fd] = std::move(conn);
        }

        struct epoll_event event;
        event.events = EPOLLIN | EPOLLOUT | EPOLLET;
        event.data.fd = client_fd;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) < 0) {
            core::Logger::error("net", "TcpServer", "epoll_ctl failed for client_fd");
            close_connection(acquire_connection(client_fd));
        }
    }
}

void TcpServer::handle_client_data(const std::shared_ptr<Connection>& connection) {
    if (!connection) return;
    std::lock_guard<std::recursive_mutex> lock(connection->processing_mutex);
    if (connection->retired) return;
    Connection* conn = connection.get();
    
    // Read all available data from the socket
    while (true) {
        int bytes = conn->read_from_socket();
        
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                close_connection(connection);
                return;
            }
        } else if (bytes == 0) {
            close_connection(connection);
            return;
        }
    }

    // ─── Phase 1: Connection hasn't upgraded yet → try WebSocket handshake ───
    if (!conn->is_upgraded()) {
        if (!WebSocket::try_handshake(*conn)) {
            // Not a valid upgrade request — could be partial data,
            // or a plain HTTP request. Either way, wait for more data.
            return;
        }
        // Handshake succeeded, 101 response is in the write buffer.
        // Fall through to flush it.
    }

    // ─── Phase 2: Connection is upgraded → parse WebSocket frames ───
    if (conn->is_upgraded()) {
        WsFrame frame;
        // Phase 7.9 hardening: read_next_frame checks the announced payload
        // length against MAX_FRAME_PAYLOAD BEFORE any bytes are copied into
        // frame.payload. An oversized frame drops the connection instead of
        // waiting for gigabytes of bytes to arrive.
        for (;;) {
            const ReadFrameResult r = WebSocket::read_next_frame(*conn, frame);
            if (r == ReadFrameResult::NeedMore) break;
            if (r == ReadFrameResult::TooLarge) {
                core::Logger::warn("net", "WebSocket",
                    "Oversized frame from " + conn->get_ip() + " — closing");
                close_connection(connection);
                return;
            }
            // A valid complete frame of any opcode proves liveness. This also
            // prevents an actively playing browser from being disconnected if
            // its explicit pong is delayed or coalesced by the network stack.
            conn->mark_activity_received(Connection::HeartbeatClock::now());
            switch (frame.opcode) {
                case WsOpcode::TEXT: {
                    // Convert payload to string and route to handler
                    std::string message(frame.payload.begin(), frame.payload.end());
                    // Never place credentials, bearer tokens, or encrypted
                    // auth envelopes in logs. Even local debug transcripts are
                    // commonly pasted into bug reports.
                    core::Logger::debug(
                        "net", "WebSocket",
                        contains_sensitive_json(message)
                            ? "Received sensitive JSON frame (payload redacted)"
                            : "Received: " + message);
                    router_.route(*conn, message);
                    break;
                }
                case WsOpcode::BINARY:
                    // We'll handle binary protocol in Phase 4
                    core::Logger::debug("net", "WebSocket", "Received binary frame (" 
                                       + std::to_string(frame.payload.size()) + " bytes)");
                    break;
                case WsOpcode::PING:
                    WebSocket::send_pong(*conn, frame.payload);
                    break;
                case WsOpcode::CLOSE:
                    core::Logger::info("net", "WebSocket", "Client " + conn->get_ip() + " sent close frame");
                    WebSocket::send_close(*conn);
                    // Flush the close frame, then disconnect
                    while (conn->has_data_to_write()) {
                        int written = conn->write_to_socket();
                        if (written <= 0) break;
                    }
                    close_connection(connection);
                    return;
                case WsOpcode::PONG:
                    conn->mark_pong_received(Connection::HeartbeatClock::now());
                    break;
                default:
                    core::Logger::warn("net", "WebSocket", "Unknown opcode: " 
                                       + std::to_string(static_cast<int>(frame.opcode)));
                    break;
            }
        }
    }

    // Flush write buffer to socket
    while (conn->has_data_to_write()) {
        int written = conn->write_to_socket();
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                close_connection(connection);
                return;
            }
        }
    }

    // LLD-6.4: cap enforcement. If any of the writes above pushed the
    // buffer past `Connection::MAX_WRITE_BUFFER_BYTES`, the append was
    // dropped and the sticky overflow flag is set. Close now — a slow
    // client that lets its socket back-pressure this hard is either
    // dead or malicious, and continuing to serve it wastes RAM.
    if (conn->write_buffer_overflowed()) {
        core::Logger::warn("net", "TcpServer",
            "Closing " + conn->get_ip() + " — write buffer overflow");
        close_connection(connection);
    }
}

void TcpServer::close_connection(const std::shared_ptr<Connection>& connection) {
    if (!connection) return;
    std::lock_guard<std::recursive_mutex> processing(connection->processing_mutex);
    if (connection->retired) return;
    connection->retired = true;
    const int client_fd = connection->get_fd();
    {
        std::lock_guard<std::recursive_mutex> registry(connections_mutex_);
        const auto it = connections_.find(client_fd);
        if (it == connections_.end() || it->second != connection) return;
        connections_.erase(it);
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    }
    if (disconnect_cb_) {
        disconnect_cb_(client_fd);
    }
}

std::shared_ptr<Connection> TcpServer::acquire_connection(int fd) {
    std::lock_guard<std::recursive_mutex> lock(connections_mutex_);
    const auto it = connections_.find(fd);
    return it == connections_.end() ? nullptr : it->second;
}

Connection* TcpServer::get_connection(int fd) {
    std::lock_guard<std::recursive_mutex> lock(connections_mutex_);
    auto it = connections_.find(fd);
    if (it != connections_.end()) return it->second.get();
    return nullptr;
}

void TcpServer::run_connection_maintenance() {
    std::vector<std::shared_ptr<Connection>> snapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(connections_mutex_);
        for (const auto& entry : connections_) snapshot.push_back(entry.second);
    }
    const auto now = Connection::HeartbeatClock::now();
    std::vector<std::shared_ptr<Connection>> stale_connections;

    for (auto& owned : snapshot) {
        // A busy application's SQL must not stall accept/epoll/other sockets.
        std::unique_lock<std::recursive_mutex> processing(owned->processing_mutex, std::try_to_lock);
        if (!processing.owns_lock() || owned->retired) continue;
        Connection& conn = *owned;
        if (!conn.is_upgraded()) continue;

        if (conn.heartbeat_expired(now, HEARTBEAT_TIMEOUT)) {
            stale_connections.push_back(owned);
            continue;
        }

        if (!conn.heartbeat_due(now, HEARTBEAT_INTERVAL)) continue;

        WebSocket::write_frame(conn, WsOpcode::PING, "chess-heartbeat");
        conn.mark_ping_sent(now);

        while (conn.has_data_to_write()) {
            const int written = conn.write_to_socket();
            if (written > 0) continue;
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            stale_connections.push_back(owned);
            break;
        }

        if (conn.write_buffer_overflowed()) stale_connections.push_back(owned);
    }

    for (const auto& connection : stale_connections) {
        core::Logger::warn("net", "TcpServer",
            "Closing unresponsive WebSocket connection (heartbeat timeout)");
        close_connection(connection);
    }
}

void TcpServer::send_text(int fd, const std::string& frame) {
    const auto owned = acquire_connection(fd);
    if (!owned) return;
    auto& conn = *owned;
    WebSocket::write_frame(conn, WsOpcode::TEXT, frame);
    while (conn.has_data_to_write()) {
        if (conn.write_to_socket() <= 0) break;
    }
}

void TcpServer::run(std::function<bool()> stop_requested) {
    struct epoll_event events[MAX_EVENTS];

    while (running_ && (!stop_requested || !stop_requested())) {
        // Wake periodically even when no socket has traffic so game deadlines
        // are enforced server-side.
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, 250);
        
        if (num_events < 0) {
            if (errno == EINTR) continue;
            core::Logger::error("net", "TcpServer", "epoll_wait error");
            break;
        }

        for (int i = 0; i < num_events; ++i) {
            if (events[i].data.fd == server_fd_) {
                // Accept new connections on main thread (fast, no blocking)
                handle_new_connection();
            } else {
                int client_fd = events[i].data.fd;
                uint32_t ev = events[i].events;
                const auto connection = acquire_connection(client_fd);
                if (!connection) continue;

                if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    pool_.submit([this, connection] { close_connection(connection); });
                } else if (ev & EPOLLIN) {
                    // Offload data processing to the thread pool.
                    // The epoll loop stays free to handle other events.
                    pool_.submit([this, connection]() {
                        handle_client_data(connection);
                    });
                } else if (ev & EPOLLOUT) {
                    pool_.submit([this, connection]() {
                        handle_client_data(connection);
                    });
                }
            }
        }

        if (running_) run_connection_maintenance();
        if (running_ && maintenance_cb_) {
            if (maintenance_task_.valid()
                && maintenance_task_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                maintenance_task_.get();
            }
            if (!maintenance_task_.valid()) {
                maintenance_task_ = std::async(std::launch::async, [this] {
                    try { maintenance_cb_(); }
                    catch (const std::exception& e) {
                        core::Logger::error("net", "TcpServer", "Maintenance failed: " + std::string(e.what()));
                    }
                    catch (...) {
                        core::Logger::error("net", "TcpServer", "Maintenance failed with an unknown exception");
                    }
                });
            }
        }
    }
    running_ = false;
    if (maintenance_task_.valid()) maintenance_task_.get();
}

} // namespace net
} // namespace chess
