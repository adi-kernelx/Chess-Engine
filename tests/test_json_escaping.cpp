/**
 * test_json_escaping.cpp — Phase 7.9.
 *
 * The old `main.cpp` match_found builder concatenated player names into a
 * JSON template with no escaping:
 *
 *     "…\"opponent\":\"" + name + "\",…"
 *
 * A `"` inside `name` closes the string mid-message and lets the rest of
 * the value spill into arbitrary fields. Today's §7.6 username whitelist
 * forbids `"` and `\`, so the exploit is dormant — but nothing in the
 * builder says so, and any future feature that reuses the pattern with a
 * less restrictive input (a custom title, a tournament name, a chat line)
 * breaks silently.
 *
 * The fix is at the encoder: `build_match_found` routes every string field
 * through nlohmann's `dump()`, which is guaranteed to emit valid JSON. This
 * test uses a name that WOULD have shattered the old builder and confirms
 * the new one survives.
 *
 * The test intentionally violates the §7.6 whitelist in its input, because
 * the guarantee we care about is "the encoder is safe regardless of the
 * input" — a whitelist is not the last line of defence, it is a very early
 * one.
 */

#include <cstring>
#include <functional>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "game/match_notify.h"
#include "game/matchmaker.h"

using json = nlohmann::json;
using namespace chess::game;

// ============================================================
// Harness
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

static void run_test(const std::string& name, std::function<bool()> fn) {
    std::cout << "  [TEST] " << name << "... ";
    if (fn()) { std::cout << "PASS" << std::endl; g_passed++; }
    else      { std::cout << "FAIL" << std::endl; g_failed++; }
}

/// Make a MatchResult with test-friendly names. TimeControl is initialised
/// to a plausible blitz value; the specific numbers do not matter.
static MatchResult make_match(const std::string& white, const std::string& black) {
    MatchResult m{};
    m.white_fd    = 100;
    m.black_fd    = 101;
    m.white_id    = 1;
    m.black_id    = 2;
    m.white_name  = white;
    m.black_name  = black;
    m.game_id     = 42;
    m.time_control = TimeControl{300000, 5000};
    return m;
}

// ============================================================
// Tests
// ============================================================

static void test_json_escaping() {
    std::cout << "\n--- match_found JSON escaping ---" << std::endl;

    run_test("Well-behaved names round-trip", [] {
        auto m = make_match("Adi", "Bob");
        const std::string s = build_match_found(MatchSide::White, m);
        json j = json::parse(s, nullptr, false);
        return !j.is_discarded() &&
               j["type"] == "match_found" &&
               j["game_id"] == 42 &&
               j["color"] == "white" &&
               j["opponent"] == "Bob" &&
               j["white_time"] == 300000 &&
               j["black_time"] == 300000;
    });

    run_test("`type` is the FIRST key in the emitted string", [] {
        // websocket.cpp finds `type` by string scan (see MessageRouter::route
        // note in websocket.h), not by JSON parse. If nlohmann emitted keys
        // in alphabetical order the routing would break silently.
        auto m = make_match("Adi", "Bob");
        const std::string s = build_match_found(MatchSide::White, m);
        return s.rfind("{\"type\":\"match_found\"", 0) == 0;
    });

    run_test("Double-quote in a name does NOT break the frame", [] {
        // The classical injection. Under the old string-concat builder this
        // produced malformed JSON that added an "opponent_ends_early" field.
        auto m = make_match("adi", R"(evil","cheat":true,"filler":"x)");
        const std::string s = build_match_found(MatchSide::White, m);
        json j = json::parse(s, nullptr, false);
        if (j.is_discarded()) return false;
        // The forged fields must NOT have leaked in — the whole malicious
        // string sits inside `opponent`.
        return j["opponent"] == R"(evil","cheat":true,"filler":"x)" &&
               j.find("cheat") == j.end() &&
               j.find("filler") == j.end();
    });

    run_test("Backslash in a name is preserved as one backslash", [] {
        auto m = make_match("adi", R"(back\slash)");
        const std::string s = build_match_found(MatchSide::White, m);
        json j = json::parse(s, nullptr, false);
        return !j.is_discarded() && j["opponent"] == R"(back\slash)";
    });

    run_test("Newline in a name is preserved as one newline byte", [] {
        // Log-injection style: an unescaped newline would let an attacker
        // inject a fake JSON message into the same frame. dump() escapes it
        // as `\n` inside the JSON string.
        auto m = make_match("adi", "line1\nline2");
        const std::string s = build_match_found(MatchSide::White, m);
        // The wire bytes contain `\n` (escaped), not a literal newline.
        if (s.find("line1\nline2") != std::string::npos) return false;
        if (s.find("line1\\nline2") == std::string::npos) return false;
        json j = json::parse(s, nullptr, false);
        return !j.is_discarded() && j["opponent"] == "line1\nline2";
    });

    run_test("Emoji / multi-byte UTF-8 survives round-trip", [] {
        auto m = make_match("adi", "player \xF0\x9F\x91\x91");   // U+1F451 crown
        const std::string s = build_match_found(MatchSide::White, m);
        json j = json::parse(s, nullptr, false);
        return !j.is_discarded() &&
               j["opponent"] == std::string("player \xF0\x9F\x91\x91");
    });

    run_test("Empty name serialises cleanly", [] {
        auto m = make_match("", "");
        const std::string s = build_match_found(MatchSide::White, m);
        json j = json::parse(s, nullptr, false);
        return !j.is_discarded() && j["opponent"] == "";
    });

    run_test("Black-side message reverses `opponent` correctly", [] {
        auto m = make_match("Adi", "Bob");
        const std::string b = build_match_found(MatchSide::Black, m);
        json j = json::parse(b, nullptr, false);
        return !j.is_discarded() &&
               j["color"] == "black" &&
               j["opponent"] == "Adi";
    });

    run_test("Every key that must be present, is present", [] {
        // A future refactor that renames a field would break the frontend
        // silently. Pin the exact key set.
        auto m = make_match("A", "B");
        const std::string s = build_match_found(MatchSide::White, m);
        json j = json::parse(s, nullptr, false);
        const std::vector<std::string> expected = {
            "type", "game_id", "color", "opponent", "white_time", "black_time"
        };
        if (j.size() != expected.size()) return false;
        for (const auto& k : expected) if (j.find(k) == j.end()) return false;
        return true;
    });

    run_test("Control byte in a name is JSON-escaped (\\u00XX form)", [] {
        // Every ASCII control byte (0..0x1F) must escape — a literal 0x08
        // would confuse a JSON parser reading the frame.
        auto m = make_match("adi", std::string("hey") + char(0x08) + "there");
        const std::string s = build_match_found(MatchSide::White, m);
        // The wire form must NOT contain the raw byte.
        if (s.find(char(0x08)) != std::string::npos) return false;
        // And the parsed value must recover the byte.
        json j = json::parse(s, nullptr, false);
        return !j.is_discarded() &&
               j["opponent"].get<std::string>() ==
                   std::string("hey") + char(0x08) + "there";
    });
}

// ============================================================
// main
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " match_found JSON escaping - Phase 7.9" << std::endl;
    std::cout << "========================================" << std::endl;

    test_json_escaping();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Results: " << g_passed << " passed, " << g_failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;
    return (g_failed > 0) ? 1 : 0;
}
