#include "game/match_notify.h"

#include <nlohmann/json.hpp>

namespace chess {
namespace game {

using json = nlohmann::json;

std::string build_match_found(MatchSide side, const MatchResult& match) {
    const bool is_white = (side == MatchSide::White);

    json j;
    j["type"]       = "match_found";
    j["game_id"]    = match.game_id;
    j["color"]      = is_white ? "white" : "black";
    j["opponent"]   = is_white ? match.black_name : match.white_name;
    j["white_time"] = match.time_control.base_time_ms;
    j["black_time"] = match.time_control.base_time_ms;

    // dump() escapes every string field it emits. A username of `"a"` becomes
    // `"\"a\""` on the wire — the JSON is well-formed and the receiving JS
    // parser recovers the original bytes.
    //
    // The wire protocol requires `type` to be the first key, so a dumped
    // JSON object with alphabetically-ordered keys would not do. We build the
    // string in the required order by hand — but every VALUE is emitted by
    // nlohmann's escaper, so no user-supplied bytes reach the wire raw.
    std::string out;
    out.reserve(96);
    out.push_back('{');
    auto add = [&](const char* key, const json& v, bool first) {
        if (!first) out.push_back(',');
        out += json(key).dump();
        out.push_back(':');
        out += v.dump();
    };
    add("type",       j["type"],       /*first=*/true);
    add("game_id",    j["game_id"],    false);
    add("color",      j["color"],      false);
    add("opponent",   j["opponent"],   false);
    add("white_time", j["white_time"], false);
    add("black_time", j["black_time"], false);
    out.push_back('}');
    return out;
}

} // namespace game
} // namespace chess
