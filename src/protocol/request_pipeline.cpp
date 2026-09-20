/**
 * protocol/request_pipeline.cpp — see header.
 *
 * The pipeline is intentionally tiny: three stages inline, no virtual
 * dispatch, no policy plug-in table. Adding SealOpen (LLD-5.2) and
 * RateLimit (LLD-5.3) means two more inline blocks below Parse and
 * above Dispatch, not a new abstraction.
 */

#include "protocol/request_pipeline.h"

#include <ctime>
#include <utility>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace chess::protocol {

RequestPipeline::RequestPipeline(
        const chess::application::auth::IdentityExtractor* identity,
        chess::net::ConnectionLookup                       lookup)
    : identity_(identity), lookup_(std::move(lookup)) {}

void RequestPipeline::register_route(RoutePolicy policy, TypedRouteFn fn) {
    Entry e{std::move(policy), std::move(fn)};
    entries_[e.policy.type] = std::move(e);
}

void RequestPipeline::install_on_router(chess::net::MessageRouter& router) {
    for (auto& kv : entries_) {
        // Bind the router entry per-type. The router hands us the
        // Connection& and raw message; run_stages does the rest.
        router.register_handler(kv.first,
            [this, type = kv.first](chess::net::Connection& conn,
                                    const std::string&      message) {
                auto it = entries_.find(type);
                if (it == entries_.end()) return;
                run_stages(it->second, conn, message);
            });
    }
}

void RequestPipeline::dispatch_for_test(const RoutePolicy&                 policy,
                                        const TypedRouteFn&                fn,
                                        chess::net::Connection&            conn,
                                        const std::string&                 message) {
    Entry e{policy, fn};
    run_stages(e, conn, message);
}

// ── stages ──────────────────────────────────────────────────────────

void RequestPipeline::run_stages(const Entry&                     entry,
                                 chess::net::Connection&          conn,
                                 const std::string&               message) {
    // The caller sink is a direct-ctor SocketMessageSink over the
    // request's own Connection — same pattern the family handlers used
    // pre-LLD-5.
    chess::net::SocketMessageSink caller_sink(conn);

    // Stage 1: ParseJson (always on)
    json msg;
    try {
        msg = json::parse(message);
    } catch (const json::exception& e) {
        send_error(caller_sink, std::string("Invalid JSON: ") + e.what());
        return;
    }

    // Build the RequestContext up front; Auth stage will fill in
    // `identity` on success.
    chess::application::RequestContext ctx;
    ctx.caller           = conn.handle();
    ctx.received_at_unix = static_cast<int64_t>(std::time(nullptr));

    // Stage 2: Auth (when policy.auth == Required)
    if (entry.policy.auth == AuthRequirement::Required) {
        if (!identity_) {
            send_auth_error(caller_sink,
                "Authentication is not configured on this server");
            return;
        }
        auto id_res = identity_->extract(msg);
        if (!id_res.is_ok()) {
            send_auth_error(caller_sink, id_res.reason);
            return;
        }
        ctx.identity = id_res.value;
    }

    // Stage 3: Dispatch. The typed route receives an already-parsed
    // message, an authenticated ctx (when required), and the sink.
    entry.fn(ctx, msg, caller_sink);
}

void RequestPipeline::send_error(chess::application::MessageSink& sink,
                                 const std::string& message) const {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    sink.send(err.dump());
}

void RequestPipeline::send_auth_error(chess::application::MessageSink& sink,
                                      const std::string& message) const {
    json err;
    err["type"]    = "error";
    err["code"]    = chess::application::auth::kAuthRequiredCode;
    err["message"] = message;
    sink.send(err.dump());
}

} // namespace chess::protocol
