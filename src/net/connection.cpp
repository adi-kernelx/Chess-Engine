#include "connection.h"
#include "../core/logger.h"
#include <sys/socket.h>
#include <errno.h>
#include <cstring>
#include <algorithm>

namespace chess {
namespace net {

Connection::Connection(int fd, const std::string& ip) 
    : fd_(fd), ip_(ip) {
    core::Logger::debug("net", "Connection", "Created connection for " + ip_ + " (fd: " + std::to_string(fd_) + ")");
}

Connection::~Connection() {
    if (fd_ >= 0) {
        close(fd_);
        core::Logger::debug("net", "Connection", "Closed connection for " + ip_ + " (fd: " + std::to_string(fd_) + ")");
    }
}

int Connection::read_from_socket() {
    uint8_t temp_buf[4096];
    
    int bytes_read = recv(fd_, temp_buf, sizeof(temp_buf), 0);
    
    if (bytes_read > 0) {
        read_buffer_.insert(read_buffer_.end(), temp_buf, temp_buf + bytes_read);
    }
    
    return bytes_read;
}

int Connection::write_to_socket() {
    std::lock_guard<std::recursive_mutex> lock(write_mutex_);
    if (write_buffer_.empty()) return 0;

    // A peer closing during hard refresh is a normal transport failure, not
    // permission to terminate the entire process with SIGPIPE.
    int bytes_written = send(fd_, write_buffer_.data(), write_buffer_.size(), MSG_NOSIGNAL);
    
    if (bytes_written > 0) {
        // Remove written bytes from buffer
        write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + bytes_written);
    }
    
    return bytes_written;
}

void Connection::append_to_write_buffer(const uint8_t* data, size_t length) {
    std::lock_guard<std::recursive_mutex> lock(write_mutex_);
    // LLD-6.4: drop-on-overflow. Once we've overflowed we drop every
    // subsequent write — the transport is expected to close the
    // connection soon after checking the flag, and continuing to
    // buffer would let a slow client eat unbounded RAM.
    if (write_buffer_overflowed_) return;
    if (write_buffer_.size() + length > MAX_WRITE_BUFFER_BYTES) {
        write_buffer_overflowed_ = true;
        core::Logger::warn("net", "Connection",
            "Write buffer overflow (>" +
            std::to_string(MAX_WRITE_BUFFER_BYTES) + " bytes) for " +
            ip_ + " (fd " + std::to_string(fd_) + ") — dropping writes");
        return;
    }
    write_buffer_.insert(write_buffer_.end(), data, data + length);
}

void Connection::append_to_write_buffer(const std::string& data) {
    append_to_write_buffer(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

void Connection::consume_read_buffer(size_t bytes) {
    if (bytes >= read_buffer_.size()) {
        read_buffer_.clear();
    } else {
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + bytes);
    }
}

void Connection::set_upgraded(bool val) {
    upgraded_ = val;
    awaiting_pong_ = false;
    last_heartbeat_at_ = HeartbeatClock::now();
    ping_sent_at_ = last_heartbeat_at_;
}

bool Connection::heartbeat_due(HeartbeatTimePoint now,
                               HeartbeatClock::duration interval) const {
    return upgraded_ && !awaiting_pong_ && now - last_heartbeat_at_ >= interval;
}

bool Connection::heartbeat_expired(HeartbeatTimePoint now,
                                   HeartbeatClock::duration timeout) const {
    return upgraded_ && awaiting_pong_ && now - ping_sent_at_ >= timeout;
}

void Connection::mark_ping_sent(HeartbeatTimePoint now) {
    awaiting_pong_ = true;
    ping_sent_at_ = now;
}

void Connection::mark_pong_received(HeartbeatTimePoint now) {
    mark_activity_received(now);
}

void Connection::mark_activity_received(HeartbeatTimePoint now) {
    awaiting_pong_ = false;
    last_heartbeat_at_ = now;
}

} // namespace net
} // namespace chess
