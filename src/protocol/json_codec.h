/**
 * protocol/json_codec.h — one place that speaks JSON on this layer.
 *
 * Every handler in this repo used to call `msg.value("field", default)`
 * inline, catching `json::exception` around a big try/catch. Each
 * handler picked its own defaults, its own bounds, its own escaping.
 * This file centralises decoding for LLD-1's three migrated routes and
 * encoding for the responses they produce.
 *
 * All string values in emitted JSON are escaped by `nlohmann::json::dump()`;
 * we never assemble output bytes by hand-concatenating client-supplied
 * strings. Field order is `type` first — the router expects it, and
 * `match_notify.cpp` already relies on the same convention.
 *
 * The decode functions take a parsed `nlohmann::json` rather than a raw
 * string. Parsing failures are caught in the adapter and turned into an
 * `invalid_json` error; the codec proper is concerned only with the
 * structural check.
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "protocol/request.h"
#include "protocol/response.h"

namespace chess::protocol::codec {

// ── Decoding ────────────────────────────────────────────────────────────
//
// Each decode function returns std::nullopt if the top-level JSON is not
// what the route expects — missing required field, wrong type, or a
// value outside its allowed range. Extra fields are IGNORED (forward
// compatibility). `type` is not checked here; the router already
// dispatches by type.

/// `make_move`: requires `from` and `to`, each a two-character UCI
/// square in [a-h][1-8]. Optional `promotion` string first character in
/// {'q','r','b','n'} (case-insensitive → lowercased). Anything else in
/// the promotion field is a rejection.
std::optional<MakeMoveRequest>
decode_make_move(const nlohmann::json& j);

std::optional<ResignRequest>
decode_resign(const nlohmann::json& j);

std::optional<GameStateRequest>
decode_game_state(const nlohmann::json& j);

// ── Encoding ────────────────────────────────────────────────────────────
//
// Each encode function produces a canonical JSON string with `type` first
// and other keys in the order the current handler emitted them, so the
// wire bytes match the pre-refactor output field-for-field.

std::string encode_move_made(const MoveMadeResponse& r);
std::string encode_move_rejected(const MoveRejectedResponse& r);
std::string encode_game_state(const GameStateResponse& r);
std::string encode_game_over(const GameOverResponse& r);

/// If `error.code` is empty, emits `{ "type": "error", "message": "..." }`
/// (preserving the pre-refactor `make_error()` shape). Otherwise emits
/// `{ "type": "error", "code": "...", "message": "..." }`.
std::string encode_error(const ErrorResponse& r);

} // namespace chess::protocol::codec
