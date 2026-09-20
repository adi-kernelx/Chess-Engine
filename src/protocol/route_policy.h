/**
 * protocol/route_policy.h — declarative per-route policy (LLD-5.1).
 *
 * A `RoutePolicy` is the value a handler family registers alongside
 * each route function. It replaces the ad-hoc block that lived at the
 * top of every handler method:
 *
 *     json::parse(...)                          <-- ParseJson stage
 *     if (!identity_) send_auth_error(...)      <-- Auth stage
 *     auto id = identity_->extract(msg);        <-- Auth stage
 *     if (!id.is_ok()) send_auth_error(...)     <-- Auth stage
 *
 * `RequestPipeline` reads the policy on incoming dispatch and runs the
 * matching stages before the typed route function ever executes.
 *
 * LLD-5.1 supports two flags: parse (always on) and auth. LLD-5.2 adds
 * `seal`; LLD-5.3 adds `rate_limit`. Every field defaults to the
 * least-privileged / no-check value so a route that omits its policy
 * still runs identically to a legacy public route.
 */

#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "application/ports/message_sink.h"
#include "application/request_context.h"

namespace chess::protocol {

enum class AuthRequirement { None, Required };
enum class SealRequirement { None, Required };  // populated but unused in 5.1

struct RoutePolicy {
    std::string     type;
    AuthRequirement auth = AuthRequirement::None;
    SealRequirement seal = SealRequirement::None;
};

/// A route function receives the parsed JSON, a fully-populated
/// RequestContext (with `identity` set iff policy.auth == Required and
/// extraction succeeded), and a caller-facing MessageSink for the
/// service's own responses. The pipeline owns the sink's lifetime for
/// the duration of one dispatch.
using TypedRouteFn = std::function<void(
    chess::application::RequestContext&      ctx,
    const nlohmann::json&                    msg,
    chess::application::MessageSink&         sink)>;

} // namespace chess::protocol
