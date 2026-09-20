/**
 * protocol/request_pipeline.h — one auditable path from a raw WebSocket
 * frame to a typed route function.
 *
 * BEFORE
 *   Every family handler duplicated the same four steps at the top of
 *   every route method: try/catch json::parse, check `identity_` for
 *   null, call `identity_->extract`, emit `auth_required` on failure.
 *   Seal opening was a separate pre-dispatch closure installed on the
 *   router in main.cpp. Rate limits lived inline in AuthHandler.
 *
 * NOW (LLD-5.1 / 5.2 / 5.3)
 *   Each route declares a `RoutePolicy` at registration time. The
 *   pipeline owns the ordered stages that policy triggers. A typed
 *   route function only runs after every applicable stage has passed;
 *   any failure short-circuits by sending a stable, wire-parity error
 *   frame through the caller's MessageSink (or, for seal-required-but-
 *   unsealed, drops silently — matching the pre-refactor sealed
 *   registry semantics).
 *
 * STAGE ORDER
 *   0. SealOpen   — when a SealedRegistry is wired: consult it with
 *                   the raw message. NotSealed → continue; Opened →
 *                   replace the raw message with the plaintext and
 *                   continue; Rejected → drop silently (no frame).
 *   1. ParseJson  — on typed routes. Failure sends
 *                   `{type:"error", message:"Invalid JSON: ..."}`.
 *   2. Auth       — on when policy.auth == Required. Failure sends
 *                   `{type:"error", code:"auth_required", message:...}`
 *                   with the exact phrasings the pre-refactor
 *                   `send_auth_error` helpers used.
 *   3. Dispatch   — invokes the registered route fn.
 *
 * TWO REGISTRATION SHAPES
 *   - `register_route(policy, TypedRouteFn)` — the pipeline runs
 *     stages 0-2 and passes the parsed JSON + populated context.
 *     Every game-family route uses this.
 *   - `register_raw_route(policy, RawRouteFn)` — the pipeline runs
 *     stage 0 (SealOpen) then hands the (possibly rewritten) raw
 *     string to the route fn. ParseJson and Auth are the route's
 *     responsibility. This is how AuthHandler routes on the pipeline
 *     — the auth family has unique parse-error semantics
 *     (`auth_error{code:invalid_request}`) and per-surface rate
 *     limits that don't fit a single generic Auth stage.
 *
 * INTEGRATION
 *   `install_on_router(router)` installs one router entry per
 *   registered route (typed or raw). IdentityExtractor and
 *   SealedRegistry may each be null when the corresponding capability
 *   is not wired; the pipeline fails closed in either case (auth →
 *   auth_required; seal → NotSealed pass-through, which for an
 *   unregistered type is a no-op).
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "application/auth/identity_extractor.h"
#include "net/connection.h"
#include "net/socket_message_sink.h"
#include "net/websocket.h"
#include "protocol/route_policy.h"

namespace chess::protocol {

/// A raw route function receives the (possibly seal-opened) message
/// string plus a caller sink. Auth-family handlers use this — they
/// carry their own JSON parsing, error frames, and rate limits.
using RawRouteFn = std::function<void(chess::net::Connection&      conn,
                                      const std::string&           message,
                                      chess::application::MessageSink& sink)>;

/// Outcome of a SealOpen stage callback. The pipeline never speaks
/// `crypto/` types directly — the composition root wires a callback
/// (typically over `crypto::SealedRegistry::inspect`) whose semantics
/// map cleanly to the three cases below.
enum class SealOutcome {
    Continue,   ///< message is not sealed / registry has no opinion — dispatch as-is
    Rewritten,  ///< sealed and opened; caller wrote plaintext into `out`
    Rejected    ///< required-but-unsealed or malformed seal; drop silently
};

/// The SealOpen stage. `out_rewritten` is only written on `Rewritten`.
using SealOpenFn = std::function<SealOutcome(const std::string& type,
                                             const std::string& message,
                                             std::string&       out_rewritten)>;

class RequestPipeline {
public:
    /// `identity` may be null when the server was launched without
    /// auth wiring. `seal_open` may be a default-constructed function
    /// when the operator did not deploy an identity key; the stage
    /// then becomes a no-op. `lookup` resolves fds to Connection* for
    /// foreign-fd sinks.
    RequestPipeline(const chess::application::auth::IdentityExtractor* identity,
                    SealOpenFn                                         seal_open,
                    chess::net::ConnectionLookup                       lookup);

    /// Register a typed route. Pipeline runs SealOpen + ParseJson +
    /// (optionally) Auth before invoking `fn`.
    void register_route(RoutePolicy policy, TypedRouteFn fn);

    /// Register a raw route. Pipeline runs SealOpen only, then hands
    /// the (possibly rewritten) message to `fn`. Used by AuthHandler.
    void register_raw_route(RoutePolicy policy, RawRouteFn fn);

    /// Bind every registered route on the given router.
    void install_on_router(chess::net::MessageRouter& router);

    /// Test hook — bypasses the router but runs the same stage code.
    void dispatch_for_test(const RoutePolicy&                 policy,
                           const TypedRouteFn&                fn,
                           chess::net::Connection&            conn,
                           const std::string&                 message);
    void dispatch_raw_for_test(const RoutePolicy&             policy,
                               const RawRouteFn&              fn,
                               chess::net::Connection&        conn,
                               const std::string&             message);

private:
    struct TypedEntry { RoutePolicy policy; TypedRouteFn fn; };
    struct RawEntry   { RoutePolicy policy; RawRouteFn   fn; };

    /// Run stages for a typed route.
    void run_typed(const TypedEntry&                 entry,
                   chess::net::Connection&           conn,
                   const std::string&                message);

    /// Run stages for a raw route.
    void run_raw(const RawEntry&                     entry,
                 chess::net::Connection&             conn,
                 const std::string&                  message);

    /// SealOpen stage. Delegates to the injected callback.
    SealOutcome seal_stage(const std::string& type,
                           const std::string& message,
                           std::string&       out_rewritten);

    void send_error(chess::application::MessageSink& sink,
                    const std::string&               message) const;
    void send_auth_error(chess::application::MessageSink& sink,
                         const std::string&               message) const;

    const chess::application::auth::IdentityExtractor* identity_   = nullptr;
    SealOpenFn                                         seal_open_;
    chess::net::ConnectionLookup                       lookup_;
    std::unordered_map<std::string, TypedEntry>        typed_entries_;
    std::unordered_map<std::string, RawEntry>          raw_entries_;
};

} // namespace chess::protocol
