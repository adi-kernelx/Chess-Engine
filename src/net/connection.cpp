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
    if (write_buffer_.empty()) return 0;

    int bytes_written = send(fd_, write_buffer_.data(), write_buffer_.size(), 0);
    
    if (bytes_written > 0) {
        // Remove written bytes from buffer
        write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + bytes_written);
    }
    
    return bytes_written;
}

void Connection::append_to_write_buffer(const uint8_t* data, size_t length) {
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

} // namespace net
} // namespace chess
