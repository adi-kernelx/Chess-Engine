/**
 * protocol/request_pipeline.h — one auditable path from a raw WebSocket
 * frame to a typed route function (LLD-5.1).
 *
 * BEFORE
 *   Each family handler duplicated the same four steps at the top of
 *   every route method: try/catch json::parse, check `identity_` for
 *   null, call `identity_->extract`, emit `auth_required` on failure.
 *   Seal opening was a separate pre-dispatch closure installed on the
 *   router in main.cpp. Rate limits lived inline in AuthHandler.
 *
 * NOW
 *   Each route declares a `RoutePolicy` at registration time. The
 *   pipeline owns the ordered stages that policy triggers. A typed
 *   route function only runs after every applicable stage has passed;
 *   any failure short-circuits by sending a stable, wire-parity error
 *   frame through the caller's MessageSink.
 *
 * STAGE ORDER (LLD-5.1)
 *   1. ParseJson  — always on; produces `nlohmann::json`. Failure sends
 *                   `{type:"error", message:"Invalid JSON: ..."}`.
 *   2. Auth       — on when policy.auth == Required. Consults an
 *                   IdentityExtractor; failure sends
 *                   `{type:"error", code:"auth_required", message:...}`
 *                   using the same phrasings the pre-refactor helpers
 *                   emitted, so every existing test still matches.
 *   3. Dispatch   — invokes the registered TypedRouteFn.
 *
 * FUTURE (populated but not run in 5.1)
 *   - SealOpen / SealRequirement (LLD-5.2) — folds in the seal
 *     handling that main.cpp currently attaches as a pre-dispatch
 *     closure.
 *   - RateLimit (LLD-5.3) — folds in AuthHandler's per-surface
 *     RateLimiter members.
 *
 * INTEGRATION
 *   The pipeline installs a router handler for every declared route
 *   via `install_on_router`. Callers must pass a ConnectionLookup that
 *   returns the current Connection* for an fd (already used by
 *   SocketMessageSink); the pipeline builds the caller sink from it.
 *
 *   IdentityExtractor may be null when the server is running without
 *   auth wiring; the Auth stage then fails closed with the exact same
 *   `auth_required` frame the pre-refactor handlers produced.
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "application/auth/identity_extractor.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "net/websocket.h"
#include "protocol/route_policy.h"

namespace chess::protocol {

class RequestPipeline {
public:
    /// Both pointers may outlive an individual dispatch. `identity` may
    /// be null when the server was launched without auth wiring; the
    /// Auth stage then always fails, emitting the same `auth_required`
    /// frame the pre-refactor handlers used.
    RequestPipeline(const chess::application::auth::IdentityExtractor* identity,
                    chess::net::ConnectionLookup                       lookup);

    /// Register a route. The pipeline stores the (policy, fn) pair; the
    /// route is not attached to the router until `install_on_router`.
    void register_route(RoutePolicy policy, TypedRouteFn fn);

    /// Bind every registered route on the given router. Each router
    /// entry is a small closure that runs the pipeline stages for that
    /// route, then invokes the typed function on success.
    void install_on_router(chess::net::MessageRouter& router);

    /// Non-public — exposed for direct unit testing of stages without a
    /// real router / socket in the loop.
    void dispatch_for_test(const RoutePolicy&                 policy,
                           const TypedRouteFn&                fn,
                           chess::net::Connection&            conn,
                           const std::string&                 message);

private:
    struct Entry {
        RoutePolicy   policy;
        TypedRouteFn  fn;
    };

    /// Run the stages for `entry` against `message`. Returns after
    /// either dispatching `entry.fn` or sending a policy-failure frame
    /// through the caller sink.
    void run_stages(const Entry&                       entry,
                    chess::net::Connection&            conn,
                    const std::string&                 message);

    /// Emit `{ "type": "error", "message": ... }` — the shape used by
    /// pre-refactor `send_error` helpers on every family handler.
    void send_error(chess::application::MessageSink& sink,
                    const std::string&               message) const;

    /// Emit `{ "type": "error", "code": "auth_required", "message": ... }`.
    void send_auth_error(chess::application::MessageSink& sink,
                         const std::string&               message) const;

    const chess::application::auth::IdentityExtractor* identity_ = nullptr;
    chess::net::ConnectionLookup                       lookup_;
    std::unordered_map<std::string, Entry>             entries_;
};

} // namespace chess::protocol
