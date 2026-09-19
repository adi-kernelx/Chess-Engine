/**
 * game_handler.cpp — WebSocket JSON API implementation.
 *
 * As of LLD-2.1 this file is a REGISTRATION FACADE + a home for the three
 * un-migrated families' handlers (query/spectator/replay, analysis,
 * tournaments). The gameplay + matchmaking family has been extracted
 * into `application::GameplayService` + `game::handlers::GameplayHandler`
 * and this class delegates to them.
 *
 * The server is authoritative: every move is validated server-side.
 * Clients cannot directly modify game state — they can only request
 * actions, and the server decides whether to accept or reject them.
 */

#include "game/game_handler.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ctime>
#include <memory>
#include <sstream>

#include <nlohmann/json.hpp>

#include "analysis/anti_cheat.h"
#include "analysis/cheat_report_repo.h"
#include "application/auth/identity_extractor.h"
#include "application/gameplay_service.h"
#include "auth/session.h"
#include "chess/board.h"
#include "chess/engine.h"
#include "chess/move.h"
#include "chess/move_gen.h"
#include "chess/notation.h"
#include "core/logger.h"
#include "game/handlers/gameplay_handler.h"
#include "net/socket_message_sink.h"
#include "protocol/json_codec.h"
#include "protocol/request.h"
#include "protocol/response.h"
#include "storage/game_repo.h"
#include "storage/player_repo.h"
#include "tournament/tournament_manager.h"
#include "tournament/tournament_repo.h"

using json = nlohmann::json;

namespace chess {
namespace game {

// ============================================================
// Construction & Registration
// ============================================================

GameHandler::GameHandler(RoomManager& room_mgr, Matchmaker& matchmaker)
    : room_mgr_(room_mgr), matchmaker_(matchmaker) {}

void GameHandler::register_handlers(net::MessageRouter& router) {
    // ── LLD-2.1: build and register the gameplay family. ──
    //
    // Both db_ and signer_ MUST already have been set (or explicitly
    // left null when auth is not configured). See main.cpp for ordering.
    identity_ = std::make_unique<application::auth::IdentityExtractor>(
        db_, signer_);

    auto foreign_sender = [this](int fd, const std::string& frame) {
        this->send_json_to_fd(fd, frame);
    };
    auto spectator_broadcaster = [this](GameRoom& room, const std::string& frame) {
        this->broadcast_to_spectators(room, frame);
    };

    gameplay_service_ = std::make_unique<application::GameplayService>(
        room_mgr_, matchmaker_, ai_player_,
        std::move(foreign_sender),
        std::move(spectator_broadcaster),
        db_);

    gameplay_handler_ = std::make_unique<handlers::GameplayHandler>(
        *gameplay_service_, identity_.get(), connection_lookup_);

    gameplay_handler_->register_handlers(router);

    // ── LLD-2.2: build and register the query / spectator / replay family. ──

    query_service_ = std::make_unique<application::GameQueryService>(
        room_mgr_, db_);
    query_handler_ = std::make_unique<handlers::QueryHandler>(
        *query_service_, identity_.get(), connection_lookup_);
    query_handler_->register_handlers(router);

    // ── LLD-2.3: build and register the analysis family. ──

    analysis_service_ = std::make_unique<application::AnalysisService>(db_);
    analysis_handler_ = std::make_unique<handlers::AnalysisHandler>(
        *analysis_service_, connection_lookup_);
    analysis_handler_->register_handlers(router);

    // ── Un-migrated families: still owned by this class. ──

    // Phase 9.4 — tournaments
    router.register_handler("create_tournament",
        [this](net::Connection& c, const std::string& m) { handle_create_tournament(c, m); });
    router.register_handler("join_tournament",
        [this](net::Connection& c, const std::string& m) { handle_join_tournament(c, m); });
    router.register_handler("start_tournament",
        [this](net::Connection& c, const std::string& m) { handle_start_tournament(c, m); });
    router.register_handler("tournament_state",
        [this](net::Connection& c, const std::string& m) { handle_tournament_state(c, m); });
    router.register_handler("list_tournaments",
        [this](net::Connection& c, const std::string& m) { handle_list_tournaments(c, m); });
    router.register_handler("report_tournament_result",
        [this](net::Connection& c, const std::string& m) { handle_report_tournament_result(c, m); });
}

void GameHandler::on_player_disconnect(int connection_fd) {
    // Forward to the gameplay service (room + matchmaker + spectator sweep).
    // The un-migrated families currently have no disconnect hook.
    if (gameplay_handler_) {
        gameplay_handler_->on_player_disconnect(connection_fd);
    }
}

// ============================================================
// Shared helpers still used by the un-migrated families
// ============================================================

void GameHandler::send_json(net::Connection& conn, const std::string& json_str) {
    net::WebSocket::write_frame(conn, net::WsOpcode::TEXT, json_str);
}

void GameHandler::send_json_to_fd(int fd, const std::string& json_str) {
    if (!connection_lookup_) return;
    net::Connection* conn = connection_lookup_(fd);
    if (conn) {
        net::WebSocket::write_frame(*conn, net::WsOpcode::TEXT, json_str);
        // Flush immediately — this connection's handler isn't running,
        // so no one else will flush its write buffer for us.
        while (conn->has_data_to_write()) {
            int written = conn->write_to_socket();
            if (written <= 0) break;  // EAGAIN or error — epoll will retry later
        }
    }
}

void GameHandler::broadcast_to_spectators(GameRoom& room,
                                          const std::string& json_str) {
    // Snapshot first, iterate outside — room mutex must never be held across
    // send_json_to_fd (each send does a syscall + inline flush, which would
    // serialize every other move on this room behind the slowest watcher).
    const auto fds = room.spectator_fds();
    for (int fd : fds) {
        send_json_to_fd(fd, json_str);
    }
}

std::string GameHandler::make_error(const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    return err.dump();
}

void GameHandler::send_error_code(net::Connection& conn,
                                  const std::string& code,
                                  const std::string& message) {
    json err;
    err["type"]    = "error";
    err["code"]    = code;
    err["message"] = message;
    send_json(conn, err.dump());
}

bool GameHandler::extract_identity(net::Connection& conn,
                                   const nlohmann::json& msg,
                                   int64_t& out_db_player_id,
                                   std::string& out_username,
                                   int& out_elo) {
    // Thin adapter over the shared helper — preserves the un-migrated
    // families' out-parameter shape until LLD-2.2 / 2.3 / 2.4 switch
    // them to call the extractor directly.
    if (!identity_) {
        send_error_code(conn, application::auth::kAuthRequiredCode,
            "Authentication is not configured on this server");
        return false;
    }
    auto res = identity_->extract(msg);
    if (!res.is_ok()) {
        send_error_code(conn, application::auth::kAuthRequiredCode, res.reason);
        return false;
    }
    out_db_player_id = res.value.player_id;
    out_username     = res.value.username;
    out_elo          = res.value.elo_rating;
    return true;
}

std::string GameHandler::status_to_reason(GameStatus status) {
    switch (status) {
        case GameStatus::CHECKMATE:                  return "checkmate";
        case GameStatus::STALEMATE:                  return "stalemate";
        case GameStatus::DRAW_FIFTY_MOVE:            return "fifty_move_rule";
        case GameStatus::DRAW_INSUFFICIENT_MATERIAL: return "insufficient_material";
        case GameStatus::DRAW_THREEFOLD_REPETITION:  return "threefold_repetition";
        case GameStatus::DRAW_AGREEMENT:             return "draw_agreement";
        case GameStatus::RESIGNATION:                return "resignation";
        case GameStatus::TIMEOUT:                    return "timeout";
        default:                                     return "unknown";
    }
}

// ============================================================
// Phase 9.4 — tournaments
// ============================================================
//
// All handlers share the same pattern: parse → require auth (for the
// mutating four) → dispatch to a fresh TournamentManager built over the
// server's Database. The manager is stateless in-process, so a per-call
// instance carries no cost.

namespace {

// Turn a StoredTournament row into the wire JSON shape.
nlohmann::json tournament_to_json(const tournament::StoredTournament& t) {
    nlohmann::json j;
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

nlohmann::json standing_to_json(const tournament::StandingRow& r) {
    nlohmann::json j;
    j["player_id"]     = r.player_id;
    j["elo"]           = r.initial_elo;
    j["score"]         = r.score;
    j["buchholz"]      = r.buchholz;
    j["whites_played"] = r.whites_played;
    j["received_bye"]  = r.received_bye;
    j["withdrawn"]     = r.withdrawn;
    return j;
}

nlohmann::json pairing_to_json(const tournament::StoredPairing& p) {
    nlohmann::json j;
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

void GameHandler::handle_create_tournament(net::Connection& conn,
                                           const std::string& message) {
    try {
        auto msg = json::parse(message);

        int64_t db_player_id = 0;
        std::string username;
        int elo = 1200;
        if (!extract_identity(conn, msg, db_player_id, username, elo)) return;

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }

        const std::string name = msg.value("name", std::string{});
        if (name.empty()) {
            send_json(conn, make_error("Missing 'name'"));
            return;
        }
        // Bound the name — keeps DB rows sane; anything longer is either
        // a mistake or an attempt to blow up the standings JSON payload.
        if (name.size() > 128) {
            send_json(conn, make_error("Tournament name too long"));
            return;
        }
        const int rounds   = msg.value("rounds",    4);
        const int time_base = msg.value("time_base", 300);
        const int time_inc  = msg.value("time_inc",  3);
        if (rounds <= 0 || rounds > 30) {
            send_json(conn, make_error("Rounds must be between 1 and 30"));
            return;
        }
        if (time_base <= 0 || time_inc < 0) {
            send_json(conn, make_error("Invalid time control"));
            return;
        }

        auto cr = tournament::create_tournament(
            *db_, name, rounds,
            /*tc_initial=*/time_base * 1000,
            /*tc_increment=*/time_inc * 1000,
            db_player_id);
        if (!cr.ok) {
            send_json(conn, make_error("create_tournament failed: " + cr.error));
            return;
        }

        json response;
        response["type"]          = "tournament_created";
        response["tournament_id"] = cr.id;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_join_tournament(net::Connection& conn,
                                         const std::string& message) {
    try {
        auto msg = json::parse(message);

        int64_t db_player_id = 0;
        std::string username;
        int elo = 1200;
        if (!extract_identity(conn, msg, db_player_id, username, elo)) return;

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }

        if (!msg.contains("tournament_id") || !msg["tournament_id"].is_number_integer()) {
            send_json(conn, make_error("Missing or invalid tournament_id"));
            return;
        }
        const int64_t tid = msg["tournament_id"].get<int64_t>();

        tournament::TournamentManager tm(*db_);
        auto r = tm.join(tid, db_player_id, elo);
        if (!r.ok) {
            send_json(conn, make_error(r.error));
            return;
        }

        json response;
        response["type"]          = "tournament_joined";
        response["tournament_id"] = tid;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_start_tournament(net::Connection& conn,
                                          const std::string& message) {
    try {
        auto msg = json::parse(message);

        int64_t db_player_id = 0;
        std::string username;
        int elo = 1200;
        if (!extract_identity(conn, msg, db_player_id, username, elo)) return;

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }

        if (!msg.contains("tournament_id") || !msg["tournament_id"].is_number_integer()) {
            send_json(conn, make_error("Missing or invalid tournament_id"));
            return;
        }
        const int64_t tid = msg["tournament_id"].get<int64_t>();

        tournament::TournamentManager tm(*db_);
        auto r = tm.start(tid, db_player_id);
        if (!r.ok) {
            send_json(conn, make_error(r.error));
            return;
        }

        auto state = tm.get_state(tid);
        int current_round = state ? state->tournament.current_round : 1;

        json response;
        response["type"]          = "tournament_started";
        response["tournament_id"] = tid;
        response["round"]         = current_round;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_tournament_state(net::Connection& conn,
                                          const std::string& message) {
    try {
        auto msg = json::parse(message);

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }
        if (!msg.contains("tournament_id") || !msg["tournament_id"].is_number_integer()) {
            send_json(conn, make_error("Missing or invalid tournament_id"));
            return;
        }
        const int64_t tid = msg["tournament_id"].get<int64_t>();

        tournament::TournamentManager tm(*db_);
        auto st = tm.get_state(tid);
        if (!st) {
            send_json(conn, make_error("tournament_not_found"));
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

        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_list_tournaments(net::Connection& conn,
                                          const std::string& message) {
    try {
        auto msg = json::parse(message);

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }
        std::string status = msg.value("status", std::string{});
        int limit = msg.value("limit", 25);
        limit = std::clamp(limit, 1, 100);

        auto rows = tournament::list_tournaments(*db_, status, limit);

        json response;
        response["type"] = "tournament_list";
        json arr = json::array();
        for (const auto& t : rows) arr.push_back(tournament_to_json(t));
        response["tournaments"] = arr;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

void GameHandler::handle_report_tournament_result(net::Connection& conn,
                                                  const std::string& message) {
    // This is a creator-authored hook. In production, an end-of-game
    // callback inside persist_game will call TournamentManager::report_result
    // directly; the WebSocket-facing form here exists so the frontend (and
    // tests) can drive the state machine without wiring a real GameRoom
    // per pairing. See log narrative on the deferred auto-creation.
    try {
        auto msg = json::parse(message);

        int64_t db_player_id = 0;
        std::string username;
        int elo = 1200;
        if (!extract_identity(conn, msg, db_player_id, username, elo)) return;

        if (!db_) {
            send_json(conn, make_error("Tournaments require a database"));
            return;
        }
        if (!msg.contains("pairing_id") || !msg["pairing_id"].is_number_integer()) {
            send_json(conn, make_error("Missing or invalid pairing_id"));
            return;
        }
        const int64_t pairing_id = msg["pairing_id"].get<int64_t>();
        const std::string result = msg.value("result", std::string{});

        // Fetch the pairing to check the tournament's creator matches.
        // Two queries — one here to find the tournament_id, one in
        // report_result to write. Not the tightest path, but this is
        // an out-of-band admin action; per-call cost is fine.
        auto rq = db_->exec(
            "SELECT tournament_id FROM tournament_pairings WHERE id = $1",
            {storage::Param::int64(pairing_id)});
        if (!rq.ok || rq.rows.empty()) {
            send_json(conn, make_error("pairing_not_found"));
            return;
        }
        const int64_t tid = std::stoll(rq.rows[0].at(0));

        auto tournament = tournament::find_tournament(*db_, tid);
        if (!tournament) {
            send_json(conn, make_error("tournament_not_found"));
            return;
        }
        if (tournament->created_by != db_player_id) {
            send_json(conn, make_error("not_creator"));
            return;
        }

        tournament::TournamentManager tm(*db_);
        auto r = tm.report_result(pairing_id, result);
        if (!r.ok) {
            send_json(conn, make_error(r.error));
            return;
        }

        json response;
        response["type"]       = "tournament_result_recorded";
        response["pairing_id"] = pairing_id;
        response["result"]     = result;
        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

} // namespace game
} // namespace chess
