#include "net/socket_message_sink.h"

#include "net/websocket.h"

namespace chess {
namespace net {

SocketMessageSink::SocketMessageSink(ConnectionHandle handle,
                                     ConnectionLookup  lookup)
    : handle_(handle), lookup_(std::move(lookup)) {}

SocketMessageSink::SocketMessageSink(Connection& conn)
    : handle_(conn.handle()), direct_conn_(&conn) {}

bool SocketMessageSink::send(std::string frame) {
    // Direct-connection path (LLD-2.1): we already own the target
    // connection. No lookup, no generation check — the caller is
    // answering the request that arrived on this exact socket. The
    // Most handlers return immediately and the worker's normal drain is
    // enough. AI search is intentionally synchronous, however, and may keep
    // this handler active for several seconds. Drain now so an accepted human
    // move reaches the browser before the engine starts thinking. The normal
    // post-handler drain remains a harmless fallback for EAGAIN/partial writes.
    if (direct_conn_ != nullptr) {
        WebSocket::write_frame(*direct_conn_, WsOpcode::TEXT, frame);
        while (direct_conn_->has_data_to_write()) {
            const int written = direct_conn_->write_to_socket();
            if (written <= 0) break;
        }
        return true;
    }

    // Foreign-fd path: resolve, generation-check, write, drain inline.
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
