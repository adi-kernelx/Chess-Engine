/**
 * application/tournament_service.cpp — see header for design.
 *
 * Every method here is a direct move from the corresponding
 * `GameHandler::handle_*_tournament*` method. Wire bytes preserved
 * bit-for-bit: same error phrasings, same bounds, same
 * `TournamentManager` calls, same creator-check ordering.
 *
 * The pattern of every mutating route: null-db check → id-shape check →
 * manager call. The `report_tournament_result` route adds one extra
 * step — a lookup that resolves the pairing to its tournament so the
 * creator check can run before the write.
 */

#include "application/tournament_service.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>

#include "tournament/tournament_manager.h"
#include "tournament/tournament_repo.h"

using nlohmann::json;

namespace chess::application {

namespace {

std::string make_error_frame(const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    return err.dump();
}

// Turn a StoredTournament row into the wire JSON shape.
json tournament_to_json(const chess::tournament::StoredTournament& t) {
    json j;
    j["id"]             = t.id;
    j["name"]           = t.name;
    j["format"]         = t.format;
    j["rounds"]         = t.rounds;
    j["current_round"]  = t.current_round;
    j["time_base"]      = t.time_control_initial_ms   / 1000;
    j["time_inc"]       = t.time_control_increment_ms / 1000;
    j["status"]         = t.status;
    j["created_by"]     = t.created_by;
    j["created_at"]     = t.created_at;
    j["started_at"]     = t.started_at;
    j["completed_at"]   = t.completed_at;
    return j;
}

json standing_to_json(const chess::tournament::StandingRow& r) {
    json j;
    j["player_id"]     = r.player_id;
    j["elo"]           = r.initial_elo;
    j["score"]         = r.score;
    j["buchholz"]      = r.buchholz;
    j["whites_played"] = r.whites_played;
    j["received_bye"]  = r.received_bye;
    j["withdrawn"]     = r.withdrawn;
    return j;
}

json pairing_to_json(const chess::tournament::StoredPairing& p) {
    json j;
    j["id"]              = p.id;
    j["round"]           = p.round;
    j["white_player_id"] = p.white_player_id;
    if (p.black_player_id.has_value()) j["black_player_id"] = *p.black_player_id;
    else                               j["black_player_id"] = nullptr;
    if (p.game_id.has_value())         j["game_id"]         = *p.game_id;
    else                               j["game_id"]         = nullptr;
    j["result"]          = p.result;
    return j;
}

} // namespace

TournamentService::TournamentService(chess::storage::Database* db)
    : db_(db) {}

// ── create_tournament ───────────────────────────────────────────────

void TournamentService::create_tournament(const RequestContext& /*ctx*/,
                                          int64_t               actor_db_player_id,
                                          const std::string&    name,
                                          int                   rounds,
                                          int                   time_base_sec,
                                          int                   time_inc_sec,
                                          MessageSink&          caller_sink) {
    if (!db_) {
        caller_sink.send(make_error_frame("Tournaments require a database"));
        return;
    }
    if (name.empty()) {
        caller_sink.send(make_error_frame("Missing 'name'"));
        return;
    }
    // Bound the name — keeps DB rows sane; anything longer is either
    // a mistake or an attempt to blow up the standings JSON payload.
    if (name.size() > 128) {
        caller_sink.send(make_error_frame("Tournament name too long"));
        return;
    }
    if (rounds <= 0 || rounds > 30) {
        caller_sink.send(make_error_frame("Rounds must be between 1 and 30"));
        return;
    }
    if (time_base_sec <= 0 || time_inc_sec < 0) {
        caller_sink.send(make_error_frame("Invalid time control"));
        return;
    }

    auto cr = chess::tournament::create_tournament(
        *db_, name, rounds,
        /*tc_initial=*/time_base_sec * 1000,
        /*tc_increment=*/time_inc_sec * 1000,
        actor_db_player_id);
    if (!cr.ok) {
        caller_sink.send(make_error_frame("create_tournament failed: " + cr.error));
        return;
    }

    json response;
    response["type"]          = "tournament_created";
    response["tournament_id"] = cr.id;
    caller_sink.send(response.dump());
}

// ── join_tournament ─────────────────────────────────────────────────

void TournamentService::join_tournament(const RequestContext& /*ctx*/,
                                        int64_t               actor_db_player_id,
                                        int                   actor_elo,
                                        bool                  has_tournament_id,
                                        int64_t               tournament_id,
                                        MessageSink&          caller_sink) {
    if (!db_) {
        caller_sink.send(make_error_frame("Tournaments require a database"));
        return;
    }
    if (!has_tournament_id) {
        caller_sink.send(make_error_frame("Missing or invalid tournament_id"));
        return;
    }

    chess::tournament::TournamentManager tm(*db_);
    auto r = tm.join(tournament_id, actor_db_player_id, actor_elo);
    if (!r.ok) {
        caller_sink.send(make_error_frame(r.error));
        return;
    }

    json response;
    response["type"]          = "tournament_joined";
    response["tournament_id"] = tournament_id;
    caller_sink.send(response.dump());
}

// ── start_tournament ────────────────────────────────────────────────

void TournamentService::start_tournament(const RequestContext& /*ctx*/,
                                         int64_t               actor_db_player_id,
                                         bool                  has_tournament_id,
                                         int64_t               tournament_id,
                                         MessageSink&          caller_sink) {
    if (!db_) {
        caller_sink.send(make_error_frame("Tournaments require a database"));
        return;
    }
    if (!has_tournament_id) {
        caller_sink.send(make_error_frame("Missing or invalid tournament_id"));
        return;
    }

    chess::tournament::TournamentManager tm(*db_);
    auto r = tm.start(tournament_id, actor_db_player_id);
    if (!r.ok) {
        caller_sink.send(make_error_frame(r.error));
        return;
    }

    auto state = tm.get_state(tournament_id);
    int current_round = state ? state->tournament.current_round : 1;

    json response;
    response["type"]          = "tournament_started";
    response["tournament_id"] = tournament_id;
    response["round"]         = current_round;
    caller_sink.send(response.dump());
}

// ── tournament_state ────────────────────────────────────────────────

void TournamentService::tournament_state(const RequestContext& /*ctx*/,
                                         bool                  has_tournament_id,
                                         int64_t               tournament_id,
                                         MessageSink&          caller_sink) {
    if (!db_) {
        caller_sink.send(make_error_frame("Tournaments require a database"));
        return;
    }
    if (!has_tournament_id) {
        caller_sink.send(make_error_frame("Missing or invalid tournament_id"));
        return;
    }

    chess::tournament::TournamentManager tm(*db_);
    auto st = tm.get_state(tournament_id);
    if (!st) {
        caller_sink.send(make_error_frame("tournament_not_found"));
        return;
    }

    json response;
    response["type"]       = "tournament_state";
    response["tournament"] = tournament_to_json(st->tournament);

    json standings = json::array();
    for (const auto& s : st->standings) standings.push_back(standing_to_json(s));
    response["standings"]  = standings;

    json pairings = json::array();
    for (const auto& p : st->all_pairings) pairings.push_back(pairing_to_json(p));
    response["pairings"]   = pairings;

    caller_sink.send(response.dump());
}

// ── list_tournaments ────────────────────────────────────────────────

void TournamentService::list_tournaments(const RequestContext& /*ctx*/,
                                         const std::string&    status,
                                         int                   limit,
                                         MessageSink&          caller_sink) {
    if (!db_) {
        caller_sink.send(make_error_frame("Tournaments require a database"));
        return;
    }
    limit = std::clamp(limit, 1, 100);

    auto rows = chess::tournament::list_tournaments(*db_, status, limit);

    json response;
    response["type"] = "tournament_list";
    json arr = json::array();
    for (const auto& t : rows) arr.push_back(tournament_to_json(t));
    response["tournaments"] = arr;
    caller_sink.send(response.dump());
}

// ── report_tournament_result ────────────────────────────────────────

void TournamentService::report_tournament_result(const RequestContext& /*ctx*/,
                                                 int64_t               actor_db_player_id,
                                                 bool                  has_pairing_id,
                                                 int64_t               pairing_id,
                                                 const std::string&    result,
                                                 MessageSink&          caller_sink) {
    // Creator-authored hook. In production an end-of-game callback
    // inside persist_game will call TournamentManager::report_result
    // directly; this WebSocket-facing form exists so the frontend (and
    // tests) can drive the state machine without wiring a real
    // GameRoom per pairing. See the LLD-2 log entry.
    if (!db_) {
        caller_sink.send(make_error_frame("Tournaments require a database"));
        return;
    }
    if (!has_pairing_id) {
        caller_sink.send(make_error_frame("Missing or invalid pairing_id"));
        return;
    }

    // Fetch the pairing to check the tournament's creator matches.
    // Two queries — one here to find the tournament_id, one in
    // report_result to write. Not the tightest path, but this is an
    // out-of-band admin action; per-call cost is fine.
    auto rq = db_->exec(
        "SELECT tournament_id FROM tournament_pairings WHERE id = $1",
        {chess::storage::Param::int64(pairing_id)});
    if (!rq.ok || rq.rows.empty()) {
        caller_sink.send(make_error_frame("pairing_not_found"));
        return;
    }
    const int64_t tid = std::stoll(rq.rows[0].at(0));

    auto tournament = chess::tournament::find_tournament(*db_, tid);
    if (!tournament) {
        caller_sink.send(make_error_frame("tournament_not_found"));
        return;
    }
    if (tournament->created_by != actor_db_player_id) {
        caller_sink.send(make_error_frame("not_creator"));
        return;
    }

    chess::tournament::TournamentManager tm(*db_);
    auto r = tm.report_result(pairing_id, result);
    if (!r.ok) {
        caller_sink.send(make_error_frame(r.error));
        return;
    }

    json response;
    response["type"]       = "tournament_result_recorded";
    response["pairing_id"] = pairing_id;
    response["result"]     = result;
    caller_sink.send(response.dump());
}

} // namespace chess::application
