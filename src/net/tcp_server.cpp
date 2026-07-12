#include "tcp_server.h"
#include "../core/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

namespace chess {
namespace net {

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
        std::lock_guard<std::mutex> lock(connections_mutex_);
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
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[client_fd] = std::make_unique<Connection>(client_fd, std::string(ip_str));
        }

        struct epoll_event event;
        event.events = EPOLLIN | EPOLLOUT | EPOLLET;
        event.data.fd = client_fd;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) < 0) {
            core::Logger::error("net", "TcpServer", "epoll_ctl failed for client_fd");
            close_connection(client_fd);
        }
    }
}

void TcpServer::handle_client_data(int client_fd) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;
    
    Connection* conn = it->second.get();
    
    // Read all available data from the socket
    while (true) {
        int bytes = conn->read_from_socket();
        
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                close_connection(client_fd);
                return;
            }
        } else if (bytes == 0) {
            close_connection(client_fd);
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
        while (WebSocket::try_read_frame(*conn, frame)) {
            switch (frame.opcode) {
                case WsOpcode::TEXT: {
                    // Convert payload to string and route to handler
                    std::string message(frame.payload.begin(), frame.payload.end());
                    core::Logger::debug("net", "WebSocket", "Received: " + message);
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
                    close_connection(client_fd);
                    return;
                case WsOpcode::PONG:
                    // Pong received — client is alive, nothing to do
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
                close_connection(client_fd);
                return;
            }
        }
    }
}

void TcpServer::close_connection(int client_fd) {
    // Note: caller must already hold connections_mutex_
    connections_.erase(client_fd);
}

void TcpServer::run() {
    struct epoll_event events[MAX_EVENTS];

    while (running_) {
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        
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

                if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    close_connection(client_fd);
                } else if (ev & EPOLLIN) {
                    // Offload data processing to the thread pool.
                    // The epoll loop stays free to handle other events.
                    pool_.submit([this, client_fd]() {
                        handle_client_data(client_fd);
                    });
                } else if (ev & EPOLLOUT) {
                    pool_.submit([this, client_fd]() {
                        handle_client_data(client_fd);
                    });
                }
            }
        }
    }
}

} // namespace net
} // namespace chess
