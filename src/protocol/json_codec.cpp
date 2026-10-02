#include "protocol/json_codec.h"

#include <cctype>
#include <string>

namespace chess::protocol::codec {

using nlohmann::json;

// ── Small local helpers ─────────────────────────────────────────────────

namespace {

/// Extract a required string field. Returns nullopt when the key is
/// missing, is not a JSON string, or (for a chess coordinate) does not
/// pass a two-char [a-h][1-8] shape check. The shape check is done in
/// the codec rather than deferred to `parse_square` so the handler can
/// treat the value as trusted.
std::optional<std::string> get_square_field(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    const std::string s = it->get<std::string>();
    if (s.size() != 2) return std::nullopt;
    const char file = s[0], rank = s[1];
    if (file < 'a' || file > 'h') return std::nullopt;
    if (rank < '1' || rank > '8') return std::nullopt;
    return s;
}

/// Emit `"key":<json-encoded-value>` into `out`, prefixed with `,` when
/// `first` is false. The value is always escaped through `json::dump()`,
/// so client-supplied strings can never break the wire framing.
void append_field(std::string& out, const char* key, const json& value,
                  bool first) {
    if (!first) out.push_back(',');
    out += json(key).dump();
    out.push_back(':');
    out += value.dump();
}

} // namespace

// ── Decoding ────────────────────────────────────────────────────────────

std::optional<MakeMoveRequest> decode_make_move(const json& j) {
    if (!j.is_object()) return std::nullopt;

    auto from = get_square_field(j, "from");
    auto to   = get_square_field(j, "to");
    if (!from || !to) return std::nullopt;

    MakeMoveRequest r;
    r.from = std::move(*from);
    r.to   = std::move(*to);

    // Optional promotion. Missing key = no promotion. Present-but-invalid
    // is a rejection — a client that intended "no promotion" would omit
    // the field entirely.
    auto pit = j.find("promotion");
    if (pit != j.end()) {
        if (!pit->is_string()) return std::nullopt;
        const std::string ps = pit->get<std::string>();
        if (ps.empty()) return std::nullopt;
        const char lower = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ps[0])));
        if (lower != 'q' && lower != 'r' && lower != 'b' && lower != 'n') {
            return std::nullopt;
        }
        r.promotion = lower;
    }

    return r;
}

std::optional<ResignRequest> decode_resign(const json& j) {
    // Payload has no required fields, so any object is acceptable. Reject
    // non-objects so a caller sending `"resign"` as a bare string is not
    // silently accepted.
    if (!j.is_object()) return std::nullopt;
    return ResignRequest{};
}

std::optional<GameStateRequest> decode_game_state(const json& j) {
    if (!j.is_object()) return std::nullopt;
    return GameStateRequest{};
}

// ── Encoding ────────────────────────────────────────────────────────────

std::string encode_move_made(const MoveMadeResponse& r) {
    std::string out;
    out.reserve(160);
    out.push_back('{');
    append_field(out, "type",       json("move_made"),   true);
    append_field(out, "from",       json(r.from),        false);
    append_field(out, "to",         json(r.to),          false);
    append_field(out, "san",        json(r.san),         false);
    append_field(out, "white_time", json(r.white_time_ms), false);
    append_field(out, "black_time", json(r.black_time_ms), false);
    append_field(out, "fen",        json(r.fen),         false);
    append_field(out, "legal_moves", json(r.legal_moves), false);
    if (r.promotion.has_value()) {
        append_field(out, "promotion", json(*r.promotion), false);
    }
    out.push_back('}');
    return out;
}

std::string encode_move_rejected(const MoveRejectedResponse& r) {
    std::string out;
    out.reserve(64);
    out.push_back('{');
    append_field(out, "type",  json("move_rejected"), true);
    append_field(out, "error", json(r.error),         false);
    out.push_back('}');
    return out;
}

std::string encode_game_over(const GameOverResponse& r) {
    std::string out;
    out.reserve(64);
    out.push_back('{');
    append_field(out, "type",   json("game_over"), true);
    append_field(out, "result", json(r.result),    false);
    append_field(out, "reason", json(r.reason),    false);
    out.push_back('}');
    return out;
}

std::string encode_game_state(const GameStateResponse& r) {
    std::string out;
    out.reserve(256);
    out.push_back('{');
    append_field(out, "type",       json("game_state"),      true);
    append_field(out, "game_id",    json(r.game_id),         false);
    append_field(out, "white_username", json(r.white_username), false);
    append_field(out, "black_username", json(r.black_username), false);
    append_field(out, "fen",        json(r.fen),             false);
    append_field(out, "white_time", json(r.white_time_ms),   false);
    append_field(out, "black_time", json(r.black_time_ms),   false);
    append_field(out, "state",      json(r.state),           false);

    // Build the moves array with an explicit key order per entry so the
    // wire matches what `handle_game_state` used to emit.
    json moves = json::array();
    for (const auto& m : r.moves) {
        json entry;
        entry["san"]      = m.san;
        entry["think_ms"] = m.think_time_ms;
        moves.push_back(std::move(entry));
    }
    append_field(out, "moves", moves, false);
    append_field(out, "legal_moves", json(r.legal_moves), false);
    if (r.draw_offer_from.has_value()) {
        append_field(out, "draw_offer_from", json(*r.draw_offer_from), false);
    }

    if (r.result.has_value())  append_field(out, "result", json(*r.result), false);
    if (r.reason.has_value())  append_field(out, "reason", json(*r.reason), false);
    out.push_back('}');
    return out;
}

std::string encode_error(const ErrorResponse& r) {
    std::string out;
    out.reserve(64);
    out.push_back('{');
    append_field(out, "type", json("error"), true);
    if (!r.code.empty()) {
        append_field(out, "code", json(r.code), false);
    }
    append_field(out, "message", json(r.message), false);
    out.push_back('}');
    return out;
}

} // namespace chess::protocol::codec
