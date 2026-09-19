/**
 * application/result.h — typed application-layer outcome.
 *
 * The current handler layer intertwines "did the operation succeed" with
 * "which wire error frame should the client see". That coupling makes it
 * hard to test a use-case without stubbing the socket. `Result<T>`
 * separates the two: use cases return a typed outcome, and one place
 * (the handler adapter) maps it to a wire error frame.
 *
 * `ResultCode` deliberately stays small. Every value maps to a stable
 * client-visible `error.code`; a new code here means a new client
 * behavior, so additions want conscious review.
 */

#pragma once

#include <string>
#include <utility>

namespace chess::application {

enum class ResultCode {
    Ok,
    Unauthorized,       ///< missing/invalid access token or seat identity
    NotFound,           ///< no room, no player, no persisted game
    Unavailable,        ///< storage / dependency temporarily gone
    Conflict,           ///< duplicate operation, state mismatch, seat taken
    InvalidInput,       ///< malformed request or out-of-range field
    NotYourTurn,        ///< action requires the other side to move
    IllegalMove,        ///< move was well-formed but board rejected it
    RateLimited,        ///< per-caller quota exceeded
};

/// Non-void result carrying a value on success and a reason on failure.
///
/// Kept as a struct rather than a proper std::variant/std::expected so
/// callers can inspect and construct without ceremony. `value` is only
/// meaningful when `code == Ok`; `reason` is only meaningful otherwise.
template <typename T>
struct Result {
    ResultCode  code = ResultCode::Ok;
    std::string reason;
    T           value{};

    static Result ok(T v) {
        Result r;
        r.code  = ResultCode::Ok;
        r.value = std::move(v);
        return r;
    }
    static Result err(ResultCode c, std::string why) {
        Result r;
        r.code   = c;
        r.reason = std::move(why);
        return r;
    }

    bool is_ok() const { return code == ResultCode::Ok; }
};

/// Void-valued specialisation for operations that succeed with no payload.
template <>
struct Result<void> {
    ResultCode  code = ResultCode::Ok;
    std::string reason;

    static Result ok() { Result r; r.code = ResultCode::Ok; return r; }
    static Result err(ResultCode c, std::string why) {
        Result r; r.code = c; r.reason = std::move(why); return r;
    }
    bool is_ok() const { return code == ResultCode::Ok; }
};

} // namespace chess::application
