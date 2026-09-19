#include "net/socket_message_sink.h"

#include "net/websocket.h"

namespace chess {
namespace net {

SocketMessageSink::SocketMessageSink(ConnectionHandle handle,
                                     ConnectionLookup  lookup)
    : handle_(handle), lookup_(std::move(lookup)) {}

bool SocketMessageSink::send(std::string frame) {
    if (!handle_.valid() || !lookup_) return false;

    Connection* conn = lookup_(handle_.fd);
    if (conn == nullptr) return false;

    // LLD-1: guard against fd reuse. If the connection currently on this
    // fd has a different generation than the sink was built for, the
    // original socket has closed and this fd has been handed to someone
    // else — write nothing and report gone.
    if (conn->handle() != handle_) return false;

    WebSocket::write_frame(*conn, WsOpcode::TEXT, frame);
    // Drain inline exactly as GameHandler::send_json_to_fd does today —
    // EPOLLET only re-fires once when the socket becomes writable, so
    // handlers that write to a foreign fd cannot rely on the event loop
    // to eventually flush for them.
    while (conn->has_data_to_write()) {
        int written = conn->write_to_socket();
        if (written <= 0) break;   // EAGAIN or error; epoll will retry
    }
    return true;
}

} // namespace net
} // namespace chess
