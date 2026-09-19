/**
 * application/request_context.h — everything a use case needs to answer
 * "who is calling, on which connection, at what time" without touching
 * the transport layer.
 *
 * A handler adapter builds this once per incoming request and hands it
 * to the typed use-case function. The context carries a stable
 * ConnectionHandle (fd + generation), so a use case can name the caller
 * for delivery through a MessageSink without ever seeing raw Connection
 * pointers or WebSocket state.
 *
 * `identity` is std::nullopt until the auth stage populates it. Public
 * routes (list_games, get_profile, …) run to completion with it unset.
 * A route registered as auth-required rejects the request before it
 * reaches the use case if identity is missing.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "net/connection_handle.h"

namespace chess::application {

/// Identity established by verifying the caller's access token against
/// the players table. Copies primitives out so downstream code does not
/// hold onto a `players` row that could disappear.
struct AuthenticatedIdentity {
    int64_t     player_id = 0;
    std::string username;
    int         elo_rating = 1200;
};

struct RequestContext {
    net::ConnectionHandle                caller;
    std::optional<AuthenticatedIdentity> identity;
    /// Short opaque id useful for correlating log lines from the same
    /// request across the router, use case, and delivery adapter.
    std::string                          trace_id;
    /// Wall-clock arrival time in unix seconds; injected rather than read
    /// from time() so tests can pin it.
    int64_t                              received_at_unix = 0;
};

} // namespace chess::application
