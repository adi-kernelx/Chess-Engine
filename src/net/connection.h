#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <chrono>
#include <mutex>
#include <unistd.h>

#include "net/connection_handle.h"

namespace chess {
namespace net {

class Connection {
public:
    using HeartbeatClock = std::chrono::steady_clock;
    using HeartbeatTimePoint = HeartbeatClock::time_point;

    explicit Connection(int fd, const std::string& ip);
    ~Connection();

    // Disable copy
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Serialize inbound handlers per socket, not across the entire server.
    std::recursive_mutex processing_mutex;
    bool retired = false; // guarded by processing_mutex

    int get_fd() const { return fd_; }
    const std::string& get_ip() const { return ip_; }

    /// LLD-1: monotonically increasing generation set by TcpServer at accept
    /// time. Paired with `fd` in a `ConnectionHandle`, this lets a foreign
    /// sender detect that the fd it queued for has since closed and been
    /// reassigned to a different client. Default 0 for tests that construct
    /// a Connection directly.
    uint64_t generation() const { return generation_; }
    void set_generation(uint64_t g) { generation_ = g; }

    /// Stable identifier: fd + generation.
    ConnectionHandle handle() const { return {fd_, generation_}; }

    // Reads data from socket into read_buffer_. Returns bytes read, or 0 on disconnect, -1 on error (EAGAIN handled)
    int read_from_socket();
    
    // Writes data from write_buffer_ to socket. Returns bytes written, or -1 on error
    int write_to_socket();

    // Append data to the write buffer. LLD-6.4: if appending would push
    // the buffer past `MAX_WRITE_BUFFER_BYTES`, the write is DROPPED
    // and `write_buffer_overflowed()` starts returning true. The
    // transport must check that flag and close the connection —
    // continuing to append would balloon RAM behind a stalled client.
    void append_to_write_buffer(const uint8_t* data, size_t length);
    void append_to_write_buffer(const std::string& data);

    // Get access to read buffer to process data
    const std::vector<uint8_t>& get_read_buffer() const { return read_buffer_; }

    // Consume bytes from read buffer after processing
    void consume_read_buffer(size_t bytes);

    bool has_data_to_write() const { std::lock_guard<std::recursive_mutex> lock(write_mutex_); return !write_buffer_.empty(); }
    size_t write_buffer_bytes() const { std::lock_guard<std::recursive_mutex> lock(write_mutex_); return write_buffer_.size(); }

    /// LLD-6.4: cap the pending-write buffer at 4 MB. Every response
    /// frame this server emits is well under 64 KB (`WebSocket::
    /// MAX_FRAME_PAYLOAD`) — a client that has 64+ frames queued and
    /// unread is either malicious or dead, and continuing to buffer
    /// gives an attacker unbounded RAM per connection.
    static constexpr size_t MAX_WRITE_BUFFER_BYTES = 4 * 1024 * 1024;

    /// Set by `append_to_write_buffer` when the cap would be exceeded.
    /// Sticky: once true, stays true until the connection is destroyed.
    /// Callers must close the connection promptly on true.
    bool write_buffer_overflowed() const { std::lock_guard<std::recursive_mutex> lock(write_mutex_); return write_buffer_overflowed_; }

    // WebSocket state
    bool is_upgraded() const { return upgraded_; }
    void set_upgraded(bool val);

    // Server-driven WebSocket heartbeat. TcpServer serialises these methods
    // through processing_mutex, so the timestamps do not need their own
    // atomic/mutex overhead.
    bool heartbeat_due(HeartbeatTimePoint now,
                       HeartbeatClock::duration interval) const;
    bool heartbeat_expired(HeartbeatTimePoint now,
                           HeartbeatClock::duration timeout) const;
    void mark_ping_sent(HeartbeatTimePoint now);
    /// Any complete inbound WebSocket traffic proves the peer is alive, even
    /// if a browser/proxy coalesces or delays the explicit pong frame.
    void mark_activity_received(HeartbeatTimePoint now);
    void mark_pong_received(HeartbeatTimePoint now);
    bool awaiting_pong() const { return awaiting_pong_; }

private:
    mutable std::recursive_mutex write_mutex_;
    int fd_;
    std::string ip_;
    uint64_t generation_ = 0;  // LLD-1: bumped by TcpServer on accept()
    bool upgraded_ = false;  // false = raw HTTP, true = WebSocket framing
    bool write_buffer_overflowed_ = false;  // LLD-6.4
    bool awaiting_pong_ = false;
    HeartbeatTimePoint last_heartbeat_at_ = HeartbeatClock::now();
    HeartbeatTimePoint ping_sent_at_ = last_heartbeat_at_;

    std::vector<uint8_t> read_buffer_;
    std::vector<uint8_t> write_buffer_;
};

} // namespace net
} // namespace chess
