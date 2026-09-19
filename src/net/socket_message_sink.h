/**
 * net/socket_message_sink.h — MessageSink adapter for a real WebSocket.
 *
 * Bridges the application-layer boundary (`MessageSink::send(frame)`) to
 * the existing WebSocket + drain-inline pattern. Wraps rather than
 * replaces: it looks up the current `Connection*` for the stored fd,
 * checks the generation matches (fd reuse would give a stale sink to
 * a new client), writes the WebSocket frame, then drains the write
 * buffer just as `GameHandler::send_json_to_fd` does today.
 *
 * The lookup callable is expected to synchronise against the TcpServer's
 * connections_mutex_ internally (see `TcpServer::get_connection`).
 * SocketMessageSink itself takes no locks — that would nest a mutex
 * around the lookup one, which we deliberately avoid.
 *
 * `send()` returns false when the recipient is gone (either the fd has
 * closed or has been reassigned to a different client). Callers must
 * NOT retry — a "gone" recipient will never reappear.
 */

#pragma once

#include <functional>
#include <string>

#include "application/ports/message_sink.h"
#include "net/connection.h"
#include "net/connection_handle.h"

namespace chess {
namespace net {

/// Function returning the live `Connection*` for a given fd, or nullptr
/// if the fd is not currently tracked. `TcpServer::get_connection` is
/// the production implementation; tests can substitute an in-memory map.
using ConnectionLookup = std::function<Connection*(int fd)>;

class SocketMessageSink final : public application::MessageSink {
public:
    /// Foreign-fd path: the caller has only an fd (opponent seat,
    /// spectator, notification target). `lookup` resolves the current
    /// `Connection*` at send time, and the generation guard prevents
    /// delivery to a reused fd. Returns false when the recipient is
    /// gone.
    SocketMessageSink(ConnectionHandle handle, ConnectionLookup lookup);

    /// Direct path (LLD-2.1): the caller already holds a `Connection&`
    /// — typically the handler's own request-side connection. This
    /// mirrors what the pre-refactor `GameHandler::send_json(conn, …)`
    /// did: write straight to the known socket, no lookup, no
    /// generation check (the callee is talking to the sender of the
    /// request it is answering, so the handle is trivially current).
    /// The lifetime of `conn` must exceed the sink's; construct the
    /// sink per-request on the worker thread that owns the connection
    /// callback stack. Never keep this sink in application state.
    explicit SocketMessageSink(Connection& conn);

    /// See MessageSink::send. Returns false without writing if the fd
    /// has been reassigned (generation mismatch) or is no longer live.
    bool send(std::string frame) override;

    /// The handle this sink targets. Useful for tests and log lines.
    ConnectionHandle handle() const { return handle_; }

private:
    ConnectionHandle  handle_;
    ConnectionLookup  lookup_;                 ///< set on foreign-fd ctor
    Connection*       direct_conn_ = nullptr;  ///< set on direct ctor
};

} // namespace net
} // namespace chess
