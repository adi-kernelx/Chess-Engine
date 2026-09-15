/**
 * match_notify.h — the two JSON messages a match-found event emits.
 *
 * WHY THIS EXISTS AS ITS OWN FILE
 *
 * The original code path (`main.cpp`'s match callback) built these messages
 * by string concatenation with no escaping:
 *
 *   "\"type\":\"match_found\",...\"opponent\":\"" + name + "\",..."
 *
 * A username with a `"` inside would close the string mid-JSON and let the
 * rest of the value spill into arbitrary fields — trivially, an attacker
 * whose username was `evil","cheat":true,"filler":"x` would inject a
 * `"cheat": true` field on the opponent's frame. Today's §7.6 whitelist
 * happens to forbid `"` and `\` — the exploit is dormant, not fixed. And
 * "we validate usernames elsewhere" is exactly the "known-safe-so-far"
 * reasoning that leaves a footgun for every future name-like field
 * (custom titles, chat lines, tournament names, ...).
 *
 * The right fix is at the encoder: `nlohmann::json::dump()` is guaranteed
 * to escape any string it emits. Two lines of test guarantee it stays that
 * way (test_json_escaping).
 */

#pragma once

#include "game/matchmaker.h"

#include <string>

namespace chess {
namespace game {

/// Which colour a message is addressed to.
enum class MatchSide { White, Black };

/**
 * Build the JSON string the match_found handler sends to one player.
 *
 * Every field that reads a caller-supplied value goes through nlohmann's
 * escaper, so an untrusted `white_name`/`black_name`/`opponent` cannot
 * break framing. The `type` field is emitted first because
 * `websocket.cpp:325+` finds it by string scan, not by JSON parse.
 */
std::string build_match_found(MatchSide side, const MatchResult& match);

} // namespace game
} // namespace chess
