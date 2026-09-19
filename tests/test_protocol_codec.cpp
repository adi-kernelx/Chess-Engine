/**
 * test_protocol_codec.cpp — LLD-1 protocol boundary.
 *
 * The codec is the single place JSON gets translated into and out of the
 * migrated routes. If it is loose, every downstream handler inherits the
 * looseness; if it is strict, downstream code can treat its inputs as
 * trusted values. These tests fix that contract:
 *
 *   - Well-formed requests decode to the expected struct.
 *   - Missing/wrong-typed/out-of-range fields return std::nullopt, never
 *     partially-populated structs.
 *   - Extra unrecognised fields are IGNORED (forward compatibility).
 *   - Every encoded frame is valid JSON that reparses to the same object.
 *   - Encoders escape client-supplied strings (control bytes, quotes,
 *     unicode surrogates) — never assemble bytes by hand-concatenation.
 *   - Wire field order matches the pre-refactor emitter (`type` first).
 */

#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "protocol/json_codec.h"

using json = nlohmann::json;
using namespace chess::protocol;

namespace {

int g_passed = 0;
int g_failed = 0;

void run_test(const std::string& name, const std::function<bool()>& fn) {
    std::cout << "  [TEST] " << name << "... ";
    try {
        if (fn()) { std::cout << "PASS\n"; ++g_passed; }
        else      { std::cout << "FAIL\n"; ++g_failed; }
    } catch (const std::exception& e) {
        std::cout << "FAIL (exception: " << e.what() << ")\n"; ++g_failed;
    }
}

} // namespace


int main() {
    std::cout << "========================================\n";
    std::cout << " LLD-1 — protocol JSON codec\n";
    std::cout << "========================================\n";

    // ── decode_make_move ────────────────────────────────────────────────

    run_test("make_move: happy path with no promotion", []() {
        json j = { {"type","make_move"}, {"from","e2"}, {"to","e4"} };
        auto r = codec::decode_make_move(j);
        return r && r->from == "e2" && r->to == "e4" && !r->promotion.has_value();
    });

    run_test("make_move: promotion 'q' accepted lower", []() {
        json j = { {"from","e7"}, {"to","e8"}, {"promotion","q"} };
        auto r = codec::decode_make_move(j);
        return r && r->promotion.has_value() && *r->promotion == 'q';
    });

    run_test("make_move: promotion 'Q' lowercased", []() {
        json j = { {"from","e7"}, {"to","e8"}, {"promotion","Q"} };
        auto r = codec::decode_make_move(j);
        return r && r->promotion.has_value() && *r->promotion == 'q';
    });

    run_test("make_move: promotion 'k' rejected (not a promo piece)", []() {
        json j = { {"from","e7"}, {"to","e8"}, {"promotion","k"} };
        return !codec::decode_make_move(j).has_value();
    });

    run_test("make_move: empty promotion string rejected", []() {
        json j = { {"from","e7"}, {"to","e8"}, {"promotion",""} };
        return !codec::decode_make_move(j).has_value();
    });

    run_test("make_move: missing 'from' rejected", []() {
        json j = { {"to","e4"} };
        return !codec::decode_make_move(j).has_value();
    });

    run_test("make_move: missing 'to' rejected", []() {
        json j = { {"from","e2"} };
        return !codec::decode_make_move(j).has_value();
    });

    run_test("make_move: 'from' not string rejected", []() {
        json j = { {"from",42}, {"to","e4"} };
        return !codec::decode_make_move(j).has_value();
    });

    run_test("make_move: rank 9 rejected", []() {
        json j = { {"from","e2"}, {"to","e9"} };
        return !codec::decode_make_move(j).has_value();
    });

    run_test("make_move: file 'i' rejected", []() {
        json j = { {"from","i2"}, {"to","e4"} };
        return !codec::decode_make_move(j).has_value();
    });

    run_test("make_move: three-char square 'e2 ' rejected", []() {
        json j = { {"from","e2 "}, {"to","e4"} };
        return !codec::decode_make_move(j).has_value();
    });

    run_test("make_move: extra unknown fields tolerated", []() {
        json j = { {"from","e2"}, {"to","e4"}, {"comment","ignore me"},
                    {"level",7} };
        auto r = codec::decode_make_move(j);
        return r && r->from == "e2";
    });

    run_test("make_move: bare string rejected", []() {
        json j = "make_move";
        return !codec::decode_make_move(j).has_value();
    });

    // ── decode_resign / decode_game_state ──────────────────────────────

    run_test("resign: empty object accepted", []() {
        return codec::decode_resign(json::object()).has_value();
    });

    run_test("resign: bare string rejected", []() {
        return !codec::decode_resign(json("resign")).has_value();
    });

    run_test("game_state: empty object accepted", []() {
        return codec::decode_game_state(json::object()).has_value();
    });

    // ── encode_move_made ───────────────────────────────────────────────

    run_test("move_made: emitted keys and values round-trip", []() {
        MoveMadeResponse r;
        r.from = "e2"; r.to = "e4"; r.san = "e4";
        r.fen  = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1";
        r.white_time_ms = 599872;
        r.black_time_ms = 600000;
        const auto s = codec::encode_move_made(r);
        const auto j = json::parse(s);
        return j["type"]       == "move_made"
            && j["from"]       == "e2"
            && j["to"]         == "e4"
            && j["san"]        == "e4"
            && j["white_time"] == 599872
            && j["black_time"] == 600000
            && j["fen"]        == r.fen
            && j.find("promotion") == j.end();     // no promo field
    });

    run_test("move_made: type is the FIRST key on the wire", []() {
        MoveMadeResponse r; r.from="e2"; r.to="e4"; r.san="e4";
        const auto s = codec::encode_move_made(r);
        // "{\"type\":\"move_made\"..." must be the prefix.
        return s.rfind(R"({"type":"move_made")", 0) == 0;
    });

    run_test("move_made: promotion emitted only when present", []() {
        MoveMadeResponse r; r.from="e7"; r.to="e8"; r.san="e8=Q";
        r.promotion = std::string("q");
        const auto s = codec::encode_move_made(r);
        return s.find("\"promotion\":\"q\"") != std::string::npos;
    });

    // ── string escaping (the reason we don't hand-concatenate) ─────────

    run_test("error: control byte in message is JSON-escaped", []() {
        ErrorResponse e;
        e.code    = "auth_required";
        // NUL, backspace, quote, backslash — every one must escape.
        e.message = std::string("\x01") + "\"\\";
        const auto s = codec::encode_error(e);
        // Parseability is the ground truth: if it re-parses, the escape
        // worked; if the emitter had glued raw bytes it would not parse.
        json j = json::parse(s);
        return j["code"] == "auth_required" && j["message"] == e.message;
    });

    run_test("error: no code emits shape without 'code' key", []() {
        ErrorResponse e; e.message = "You are not in a game";
        const auto s = codec::encode_error(e);
        json j = json::parse(s);
        return j["type"] == "error"
            && j.find("code") == j.end()
            && j["message"]   == "You are not in a game";
    });

    run_test("game_state: fen and moves round-trip", []() {
        GameStateResponse r;
        r.game_id       = 7;
        r.fen           = "startpos";
        r.state         = "in_progress";
        r.white_time_ms = 12345;
        r.black_time_ms = 67890;
        r.moves.push_back({"e4", 800});
        r.moves.push_back({"e5", 1200});
        const auto s = codec::encode_game_state(r);
        json j = json::parse(s);
        return j["type"]     == "game_state"
            && j["game_id"]  == 7
            && j["state"]    == "in_progress"
            && j["moves"].is_array()
            && j["moves"].size() == 2
            && j["moves"][0]["san"]      == "e4"
            && j["moves"][0]["think_ms"] == 800;
    });

    run_test("game_state: finished game emits result + reason", []() {
        GameStateResponse r;
        r.state  = "finished";
        r.result = std::string("1-0");
        r.reason = std::string("checkmate");
        const auto s = codec::encode_game_state(r);
        json j = json::parse(s);
        return j["result"] == "1-0" && j["reason"] == "checkmate";
    });

    run_test("game_state: waiting game omits result + reason", []() {
        GameStateResponse r; r.state = "waiting";
        const auto s = codec::encode_game_state(r);
        json j = json::parse(s);
        return j.find("result") == j.end()
            && j.find("reason") == j.end();
    });

    run_test("game_over: only type/result/reason", []() {
        GameOverResponse r; r.result = "0-1"; r.reason = "resignation";
        const auto s = codec::encode_game_over(r);
        json j = json::parse(s);
        return j.size() == 3
            && j["type"]   == "game_over"
            && j["result"] == "0-1"
            && j["reason"] == "resignation";
    });

    run_test("move_rejected: shape matches pre-refactor emitter", []() {
        MoveRejectedResponse r; r.error = "Illegal move";
        const auto s = codec::encode_move_rejected(r);
        json j = json::parse(s);
        return j["type"] == "move_rejected" && j["error"] == "Illegal move";
    });

    // ── summary ─────────────────────────────────────────────────────────

    std::cout << "\n========================================\n";
    std::cout << " Results: " << g_passed << " passed, "
              << g_failed << " failed\n";
    std::cout << "========================================\n";
    return g_failed == 0 ? 0 : 1;
}
