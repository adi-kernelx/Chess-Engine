/**
 * protocol/request_pipeline.cpp — see header.
 *
 * SealOpen is stage 0 for both typed and raw routes. Typed routes
 * then run ParseJson + Auth. Raw routes go straight to their fn with
 * the (possibly rewritten) raw string.
 */

#include "protocol/request_pipeline.h"

#include <ctime>
#include <chrono>
#include <utility>

#include <nlohmann/json.hpp>
#include "core/logger.h"

using nlohmann::json;

namespace chess::protocol {

namespace {
class RequestTiming {
public:
    explicit RequestTiming(const std::string& type) : type_(type) {}
    ~RequestTiming() {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count();
        if (ms >= 500) chess::core::Logger::warn("protocol", "RequestPipeline",
            "slow request type=" + type_ + " elapsed_ms=" + std::to_string(ms));
    }
private:
    std::string type_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};
// Optional request correlation for typed calls. Unsolicited room events still
// use their normal sender and are never attributed to a pending request.
class CorrelatedSink final : public chess::application::MessageSink {
public:
    CorrelatedSink(chess::application::MessageSink& target, const json& request)
        : target_(target) {
        if (request.contains("request_id") && request["request_id"].is_string()
            && request["request_id"].get_ref<const std::string&>().size() <= 128)
            request_id_ = request["request_id"];
    }
    bool send(std::string frame) override {
        if (request_id_.empty()) return target_.send(std::move(frame));
        auto response = json::parse(frame, nullptr, false);
        if (!response.is_object()) return target_.send(std::move(frame));
        response["request_id"] = request_id_;
        return target_.send(response.dump());
    }
private:
    chess::application::MessageSink& target_;
    std::string request_id_;
};
}

RequestPipeline::RequestPipeline(
        const chess::application::auth::IdentityExtractor* identity,
        SealOpenFn                                         seal_open,
        chess::net::ConnectionLookup                       lookup)
    : identity_(identity),
      seal_open_(std::move(seal_open)),
      lookup_(std::move(lookup)) {}

void RequestPipeline::register_route(RoutePolicy policy, TypedRouteFn fn) {
    TypedEntry e{std::move(policy), std::move(fn)};
    typed_entries_[e.policy.type] = std::move(e);
}

void RequestPipeline::register_raw_route(RoutePolicy policy, RawRouteFn fn) {
    RawEntry e{std::move(policy), std::move(fn)};
    raw_entries_[e.policy.type] = std::move(e);
}

void RequestPipeline::install_on_router(chess::net::MessageRouter& router) {
    for (auto& kv : typed_entries_) {
        router.register_handler(kv.first,
            [this, type = kv.first](chess::net::Connection& conn,
                                    const std::string&      message) {
                auto it = typed_entries_.find(type);
                if (it == typed_entries_.end()) return;
                run_typed(it->second, conn, message);
            });
    }
    for (auto& kv : raw_entries_) {
        router.register_handler(kv.first,
            [this, type = kv.first](chess::net::Connection& conn,
                                    const std::string&      message) {
                auto it = raw_entries_.find(type);
                if (it == raw_entries_.end()) return;
                run_raw(it->second, conn, message);
            });
    }
}

void RequestPipeline::dispatch_for_test(const RoutePolicy&           policy,
                                        const TypedRouteFn&          fn,
                                        chess::net::Connection&      conn,
                                        const std::string&           message) {
    TypedEntry e{policy, fn};
    run_typed(e, conn, message);
}

void RequestPipeline::dispatch_raw_for_test(const RoutePolicy&      policy,
                                            const RawRouteFn&       fn,
                                            chess::net::Connection& conn,
                                            const std::string&      message) {
    RawEntry e{policy, fn};
    run_raw(e, conn, message);
}

// ── stages ──────────────────────────────────────────────────────────

SealOutcome RequestPipeline::seal_stage(const std::string& type,
                                        const std::string& message,
                                        std::string&       out_rewritten) {
    if (!seal_open_) return SealOutcome::Continue;
    return seal_open_(type, message, out_rewritten);
}

void RequestPipeline::run_typed(const TypedEntry&        entry,
                                chess::net::Connection&  conn,
                                const std::string&       message) {
    RequestTiming timing(entry.policy.type);
    chess::net::SocketMessageSink caller_sink(conn);

    // Stage 0: SealOpen. Rejected → silent drop.
    std::string rewritten;
    auto seal = seal_stage(entry.policy.type, message, rewritten);
    if (seal == SealOutcome::Rejected) return;
    const std::string& effective =
        (seal == SealOutcome::Rewritten) ? rewritten : message;

    // Stage 1: ParseJson.
    json msg;
    try {
        msg = json::parse(effective);
    } catch (const json::exception& e) {
        send_error(caller_sink, std::string("Invalid JSON: ") + e.what());
        return;
    }
    CorrelatedSink response_sink(caller_sink, msg);

    chess::application::RequestContext ctx;
    ctx.caller           = conn.handle();
    ctx.received_at_unix = static_cast<int64_t>(std::time(nullptr));

    // Stage 2: Auth.
    if (entry.policy.auth == AuthRequirement::Required) {
        if (!identity_) {
            send_auth_error(response_sink,
                "Authentication is not configured on this server");
            return;
        }
        auto id_res = identity_->extract(msg);
        if (!id_res.is_ok()) {
            if (id_res.code == chess::application::ResultCode::Unavailable)
                send_error(response_sink, id_res.reason);
            else send_auth_error(response_sink, id_res.reason);
            return;
        }
        ctx.identity = id_res.value;
    }

    // Stage 3: Dispatch.
    entry.fn(ctx, msg, response_sink);
}

void RequestPipeline::run_raw(const RawEntry&         entry,
                              chess::net::Connection& conn,
                              const std::string&      message) {
    RequestTiming timing(entry.policy.type);
    chess::net::SocketMessageSink caller_sink(conn);

    // Stage 0: SealOpen. Same behaviour as the typed path.
    std::string rewritten;
    auto seal = seal_stage(entry.policy.type, message, rewritten);
    if (seal == SealOutcome::Rejected) return;
    const std::string& effective =
        (seal == SealOutcome::Rewritten) ? rewritten : message;

    // Stage 3: Dispatch — raw routes handle their own parsing, auth,
    // rate limits, and error frames.
    entry.fn(conn, effective, caller_sink);
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
