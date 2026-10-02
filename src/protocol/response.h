/**
 * protocol/response.h — typed response values for LLD-1 migrated routes.
 *
 * Wire-shape preservation matters. Every field name here matches what
 * `src/game/game_handler.cpp` currently emits, including the naming
 * quirks the code already has (`white_time` carrying **milliseconds**,
 * not seconds — a rename would be an incompatible protocol change).
 *
 * Encoders live in `protocol/json_codec.h` and emit `type` first for the
 * router's benefit, exactly as `src/game/match_notify.cpp` already does.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chess::protocol {

/// `type: move_made` — broadcast after a legal move.
struct MoveMadeResponse {
    std::string from;
    std::string to;
    std::string san;
    std::string fen;               ///< board position after the move
    int white_time_ms = 0;         ///< wire field name: `white_time`
    int black_time_ms = 0;         ///< wire field name: `black_time`
    /// Legal UCI moves for the new side to move. Produced from the same
    /// authoritative post-move board so the client can highlight instantly.
    std::vector<std::string> legal_moves;
    /// The original promotion string the client sent, when the move was
    /// a promotion. Emitted only when present, matching current behavior.
    std::optional<std::string> promotion;
};

/// `type: move_rejected` — server-authoritative rejection of a client's
/// `make_move`. Carries the reason as free-form text; a future phase will
/// swap this for a code + message pair.
struct MoveRejectedResponse {
    std::string error;
};

/// `type: game_over` — sent after any terminal transition. Same wire
/// shape whether triggered by checkmate, resignation, timeout, or draw.
struct GameOverResponse {
    std::string result;             ///< "1-0" / "0-1" / "1/2-1/2"
    std::string reason;             ///< "checkmate" / "resignation" / …
};

/// One entry inside `GameStateResponse::moves`.
struct MoveHistoryEntry {
    std::string san;
    int         think_time_ms = 0;   ///< wire field name: `think_ms`
};

/// `type: game_state` — snapshot for a seat's on-demand refresh.
///
/// Fields match the current inline JSON exactly:
///   { type, game_id, fen, white_time, black_time, state,
///     moves: [{san, think_ms}, ...],
///     result?, reason? }
///
/// `result` and `reason` are emitted only when `state == "finished"`.
struct GameStateResponse {
    int64_t                       game_id = 0;
    std::string                   white_username;
    std::string                   black_username;
    std::string                   fen;
    int                           white_time_ms = 0;
    int                           black_time_ms = 0;
    std::string                   state;          ///< "waiting"/"in_progress"/"finished"
    std::vector<MoveHistoryEntry> moves;
    std::vector<std::string>      legal_moves;
    std::optional<std::string>    draw_offer_from; ///< "white" / "black"
    std::optional<std::string>    result;
    std::optional<std::string>    reason;
};

/// `type: error` — anything else went wrong. `code` is short and stable
/// (e.g. `auth_required`, `not_in_room`); `message` is human-readable.
/// The codec accepts an empty code and emits only `message`, preserving
/// the current shape of `make_error()` in `game_handler.cpp` for routes
/// that have not been migrated to structured error codes yet.
struct ErrorResponse {
    std::string code;
    std::string message;
};

} // namespace chess::protocol
