#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <unistd.h>

namespace chess {
namespace net {

class Connection {
public:
    explicit Connection(int fd, const std::string& ip);
    ~Connection();

    // Disable copy
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    int get_fd() const { return fd_; }
    const std::string& get_ip() const { return ip_; }

    // Reads data from socket into read_buffer_. Returns bytes read, or 0 on disconnect, -1 on error (EAGAIN handled)
    int read_from_socket();
    
    // Writes data from write_buffer_ to socket. Returns bytes written, or -1 on error
    int write_to_socket();

    // Append data to the write buffer
    void append_to_write_buffer(const uint8_t* data, size_t length);
    void append_to_write_buffer(const std::string& data);

    // Get access to read buffer to process data
    const std::vector<uint8_t>& get_read_buffer() const { return read_buffer_; }
    
    // Consume bytes from read buffer after processing
    void consume_read_buffer(size_t bytes);

    bool has_data_to_write() const { return !write_buffer_.empty(); }

private:
    int fd_;
    std::string ip_;
    
    std::vector<uint8_t> read_buffer_;
    std::vector<uint8_t> write_buffer_;
};

} // namespace net
} // namespace chess
