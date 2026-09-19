/**
 * protocol/request.h — typed request values for LLD-1 migrated routes.
 *
 * Handlers today take `const std::string& message` and hand-decode fields
 * inline with `msg.value("from", "")` defaults. Every handler repeats the
 * same field validation and every one gets it slightly differently. This
 * file introduces value types the codec produces and the handler consumes,
 * so that "did the client send a valid `make_move` request" is answered
 * exactly once, at the boundary, in code that has no dependency on
 * GameRoom / Database / Connection.
 *
 * Scope note: LLD-1 migrates three routes — `make_move`, `resign`,
 * `game_state`. Every other route keeps its current inline decode until
 * a later phase. This is deliberate: the fewer routes we touch in one
 * step, the easier the wire-byte-diff review, and the plan doc explicitly
 * says to characterise other routes before moving them.
 *
 * Deliberately not a std::variant<...> yet. A tagged sum makes sense once
 * every route flows through one decode-then-dispatch pipeline (LLD-5); at
 * three routes with distinct handlers it would add ceremony without
 * removing any branch.
 */

#pragma once

#include <optional>
#include <string>

namespace chess::protocol {

/// Client submitted `make_move`. Coordinate strings are validated to be
/// two-character UCI squares (`[a-h][1-8]`) — the codec rejects anything
/// else, so the handler can treat `from`/`to` as trustworthy.
struct MakeMoveRequest {
    std::string from;                    ///< e.g. "e2"
    std::string to;                      ///< e.g. "e4"
    /// Promotion piece character if the client asked for one, lowercased
    /// to one of {'q', 'r', 'b', 'n'}. std::nullopt means "no promotion
    /// field on the wire" — different from "promotion field present but
    /// unrecognised", which the codec rejects.
    std::optional<char> promotion;
};

/// Client submitted `resign`. Payload is intentionally empty — the seat
/// the request came from is the seat that resigns; the client cannot
/// resign on someone else's behalf.
struct ResignRequest {};

/// Client submitted `game_state`. Same as `resign` — the caller's seat
/// tells us which game to describe.
struct GameStateRequest {};

} // namespace chess::protocol
