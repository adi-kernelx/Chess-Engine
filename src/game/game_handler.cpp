/**
 * game_handler.cpp — WebSocket JSON API implementation
 *
 * This is where the network layer meets the game layer. Each handler:
 *   1. Parses the incoming JSON message
 *   2. Performs the corresponding GameRoom operation
 *   3. Builds a JSON response and sends it to the relevant clients
 *
 * The server is authoritative: every move is validated server-side.
 * Clients cannot directly modify game state — they can only request
 * actions, and the server decides whether to accept or reject them.
 */

#include "game/game_handler.h"
#include "core/logger.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace chess {
namespace game {

// ============================================================
// Construction & Registration
// ============================================================

GameHandler::GameHandler(RoomManager& room_mgr, Matchmaker& matchmaker)
    : room_mgr_(room_mgr), matchmaker_(matchmaker) {}

void GameHandler::register_handlers(net::MessageRouter& router) {
    router.register_handler("create_game",
        [this](net::Connection& c, const std::string& m) { handle_create_game(c, m); });
    router.register_handler("join_game",
        [this](net::Connection& c, const std::string& m) { handle_join_game(c, m); });
    router.register_handler("make_move",
        [this](net::Connection& c, const std::string& m) { handle_make_move(c, m); });
    router.register_handler("resign",
        [this](net::Connection& c, const std::string& m) { handle_resign(c, m); });
    router.register_handler("quick_play",
        [this](net::Connection& c, const std::string& m) { handle_quick_play(c, m); });
    router.register_handler("cancel_queue",
        [this](net::Connection& c, const std::string& m) { handle_cancel_queue(c, m); });
    router.register_handler("list_games",
        [this](net::Connection& c, const std::string& m) { handle_list_games(c, m); });
    router.register_handler("game_state",
        [this](net::Connection& c, const std::string& m) { handle_game_state(c, m); });
}

// ============================================================
// create_game — Player creates a new game room
// ============================================================
// Request:  { "type": "create_game", "username": "Alice", "time_base": 600, "time_inc": 5 }
// Response: { "type": "game_created", "game_id": 1, "color": "white" }

void GameHandler::handle_create_game(net::Connection& conn, const std::string& message) {
    try {
        auto msg = json::parse(message);

        std::string username = msg.value("username", "Player");
        int time_base_sec = msg.value("time_base", 600);  // Default: 10 min
        int time_inc_sec  = msg.value("time_inc", 5);      // Default: 5 sec

        // Check if this player is already in a game
        auto existing = room_mgr_.find_room_by_fd(conn.get_fd());
        if (existing && existing->get_state() != RoomState::FINISHED) {
            send_json(conn, make_error("You are already in a game (ID: " +
                      std::to_string(existing->get_id()) + ")"));
            return;
        }

        PlayerId pid = next_player_id_.fetch_add(1);
        TimeControl tc(time_base_sec * 1000, time_inc_sec * 1000);

        auto room = room_mgr_.create_room(pid, username, conn.get_fd(), tc);

        json response;
        response["type"]    = "game_created";
        response["game_id"] = room->get_id();
        response["color"]   = "white";

        core::Logger::info("game", "GameHandler",
            username + " created game " + std::to_string(room->get_id()) +
            " (" + tc.to_string() + ")");

        send_json(conn, response.dump());

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// join_game — Player joins an existing game room
// ============================================================
// Request:  { "type": "join_game", "username": "Bob", "game_id": 1 }
// Response to joiner:  { "type": "game_joined", "game_id": 1, "color": "black", ... }
// Response to creator: { "type": "game_start", "game_id": 1, "opponent": "Bob", ... }

void GameHandler::handle_join_game(net::Connection& conn, const std::string& message) {
    try {
        auto msg = json::parse(message);

        std::string username = msg.value("username", "Player");
        GameId game_id = msg.value("game_id", static_cast<GameId>(0));

        if (game_id == 0) {
            send_json(conn, make_error("Missing or invalid game_id"));
            return;
        }

        // Check if this player is already in a game
        auto existing = room_mgr_.find_room_by_fd(conn.get_fd());
        if (existing && existing->get_state() != RoomState::FINISHED) {
            send_json(conn, make_error("You are already in a game (ID: " +
                      std::to_string(existing->get_id()) + ")"));
            return;
        }

        auto room = room_mgr_.find_room(game_id);
        if (!room) {
            send_json(conn, make_error("Game " + std::to_string(game_id) + " not found"));
            return;
        }

        PlayerId pid = next_player_id_.fetch_add(1);
        if (!room->join(pid, username, conn.get_fd())) {
            send_json(conn, make_error("Cannot join game " + std::to_string(game_id) +
                      " — it may be full or already started"));
            return;
        }

        // Get the time info
        int white_ms, black_ms;
        room->get_remaining_times(white_ms, black_ms);

        // Send confirmation to the joiner (Black)
        json join_response;
        join_response["type"]        = "game_joined";
        join_response["game_id"]     = game_id;
        join_response["color"]       = "black";
        join_response["white_time"]  = white_ms;
        join_response["black_time"]  = black_ms;

        send_json(conn, join_response.dump());

        // Notify the creator (White) that the game has started
        int white_fd = room->get_player_fd(Color::WHITE);
        if (white_fd >= 0 && connection_lookup_) {
            json start_notification;
            start_notification["type"]       = "game_start";
            start_notification["game_id"]    = game_id;
            start_notification["opponent"]   = username;
            start_notification["color"]      = "white";
            start_notification["white_time"] = white_ms;
            start_notification["black_time"] = black_ms;

            send_json_to_fd(white_fd, start_notification.dump());
        }

        core::Logger::info("game", "GameHandler",
            username + " joined game " + std::to_string(game_id));

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// make_move — Player submits a chess move
// ============================================================
// Request:  { "type": "make_move", "from": "e2", "to": "e4", "promotion": "q" }
// Response: { "type": "move_made", "from": "e2", "to": "e4", "san": "e4", ... }
//      or:  { "type": "move_rejected", "error": "Illegal move" }

void GameHandler::handle_make_move(net::Connection& conn, const std::string& message) {
    try {
        auto msg = json::parse(message);

        std::string from_str = msg.value("from", "");
        std::string to_str   = msg.value("to", "");

        if (from_str.empty() || to_str.empty()) {
            send_json(conn, make_error("Missing 'from' or 'to' fields"));
            return;
        }

        Square from_sq = parse_square(from_str);
        Square to_sq   = parse_square(to_str);

        if (from_sq == NO_SQUARE || to_sq == NO_SQUARE) {
            send_json(conn, make_error("Invalid square: '" + from_str + "' or '" + to_str + "'"));
            return;
        }

        // Parse optional promotion piece
        PieceType promo = PieceType::NONE;
        std::string promo_str = msg.value("promotion", "");
        if (!promo_str.empty()) {
            char pc = promo_str[0];
            switch (pc) {
                case 'q': case 'Q': promo = PieceType::QUEEN;  break;
                case 'r': case 'R': promo = PieceType::ROOK;   break;
                case 'b': case 'B': promo = PieceType::BISHOP;  break;
                case 'n': case 'N': promo = PieceType::KNIGHT;  break;
                default: break;
            }
        }

        // Find the player's room
        auto room = room_mgr_.find_room_by_fd(conn.get_fd());
        if (!room) {
            send_json(conn, make_error("You are not in a game"));
            return;
        }

        // Submit the move to the authoritative game room
        auto result = room->submit_move(conn.get_fd(), from_sq, to_sq, promo);

        if (!result.success) {
            json reject;
            reject["type"]  = "move_rejected";
            reject["error"] = result.error;

            // If the error was a timeout, also send game_over
            if (result.game_status == GameStatus::TIMEOUT) {
                json game_over;
                game_over["type"]   = "game_over";
                game_over["result"] = room->get_result_string();
                game_over["reason"] = "timeout";

                // Send to both players
                send_json(conn, reject.dump());
                send_json(conn, game_over.dump());

                int opp_fd = room->get_opponent_fd(conn.get_fd());
                if (opp_fd >= 0 && connection_lookup_) {
                    send_json_to_fd(opp_fd, game_over.dump());
                }
            } else {
                send_json(conn, reject.dump());
            }
            return;
        }

        // Move accepted — build the broadcast message
        json move_msg;
        move_msg["type"]       = "move_made";
        move_msg["from"]       = from_str;
        move_msg["to"]         = to_str;
        move_msg["san"]        = result.san;
        move_msg["white_time"] = result.white_time_ms;
        move_msg["black_time"] = result.black_time_ms;

        if (promo != PieceType::NONE) {
            move_msg["promotion"] = promo_str;
        }

        // Send to the mover
        send_json(conn, move_msg.dump());

        // Send to the opponent
        int opp_fd = room->get_opponent_fd(conn.get_fd());
        if (opp_fd >= 0 && connection_lookup_) {
            send_json_to_fd(opp_fd, move_msg.dump());
        }

        // If the game ended (checkmate, stalemate, draw), send game_over
        if (result.game_status != GameStatus::ONGOING) {
            json game_over;
            game_over["type"]   = "game_over";
            game_over["result"] = room->get_result_string();
            game_over["reason"] = status_to_reason(result.game_status);

            send_json(conn, game_over.dump());
            if (opp_fd >= 0 && connection_lookup_) {
                send_json_to_fd(opp_fd, game_over.dump());
            }

            core::Logger::info("game", "GameHandler",
                "Game " + std::to_string(room->get_id()) + " ended: " +
                room->get_result_string() + " (" + status_to_reason(result.game_status) + ")");
        }

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// resign — Player resigns
// ============================================================

void GameHandler::handle_resign(net::Connection& conn, const std::string& /*message*/) {
    auto room = room_mgr_.find_room_by_fd(conn.get_fd());
    if (!room) {
        send_json(conn, make_error("You are not in a game"));
        return;
    }

    if (!room->resign(conn.get_fd())) {
        send_json(conn, make_error("Cannot resign — game is not in progress"));
        return;
    }

    json game_over;
    game_over["type"]   = "game_over";
    game_over["result"] = room->get_result_string();
    game_over["reason"] = "resignation";

    // Notify both players
    send_json(conn, game_over.dump());
    int opp_fd = room->get_opponent_fd(conn.get_fd());
    if (opp_fd >= 0 && connection_lookup_) {
        send_json_to_fd(opp_fd, game_over.dump());
    }

    core::Logger::info("game", "GameHandler",
        "Game " + std::to_string(room->get_id()) + ": player resigned → " +
        room->get_result_string());
}

// ============================================================
// list_games — List open (joinable) rooms
// ============================================================

void GameHandler::handle_list_games(net::Connection& conn, const std::string& /*message*/) {
    auto open_rooms = room_mgr_.list_open_rooms();

    json response;
    response["type"] = "game_list";
    response["games"] = json::array();

    for (const auto& info : open_rooms) {
        json room_info;
        room_info["game_id"]      = info.id;
        room_info["time_control"] = info.time_control;
        response["games"].push_back(room_info);
    }

    send_json(conn, response.dump());
}

// ============================================================
// game_state — Get current state of the player's game
// ============================================================

void GameHandler::handle_game_state(net::Connection& conn, const std::string& /*message*/) {
    auto room = room_mgr_.find_room_by_fd(conn.get_fd());
    if (!room) {
        send_json(conn, make_error("You are not in a game"));
        return;
    }

    int white_ms, black_ms;
    room->get_remaining_times(white_ms, black_ms);

    auto history = room->get_move_history();

    json response;
    response["type"]       = "game_state";
    response["game_id"]    = room->get_id();
    response["fen"]        = room->get_board().to_fen();
    response["white_time"] = white_ms;
    response["black_time"] = black_ms;

    // State string
    auto state = room->get_state();
    if (state == RoomState::WAITING)     response["state"] = "waiting";
    else if (state == RoomState::IN_PROGRESS) response["state"] = "in_progress";
    else response["state"] = "finished";

    // Move list
    response["moves"] = json::array();
    for (const auto& record : history) {
        json move_entry;
        move_entry["san"]       = record.san;
        move_entry["think_ms"]  = record.think_time_ms;
        response["moves"].push_back(move_entry);
    }

    if (state == RoomState::FINISHED) {
        response["result"] = room->get_result_string();
        response["reason"] = status_to_reason(room->get_game_status());
    }

    send_json(conn, response.dump());
}

// ============================================================
// Helpers
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

std::string GameHandler::make_error(const std::string& message) {
    json err;
    err["type"]    = "error";
    err["message"] = message;
    return err.dump();
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

Square GameHandler::parse_square(const std::string& sq_str) {
    if (sq_str.size() != 2) return NO_SQUARE;

    int file = sq_str[0] - 'a';
    int rank = sq_str[1] - '1';

    if (file < 0 || file > 7 || rank < 0 || rank > 7) return NO_SQUARE;

    return make_square(rank, file);
}

// ============================================================
// quick_play — Player joins the matchmaking queue
// ============================================================
// Request:  { "type": "quick_play", "username": "Alice", "elo": 1200, "time_base": 600, "time_inc": 5 }
// Response: { "type": "queued", "queue_size": 3 }

void GameHandler::handle_quick_play(net::Connection& conn, const std::string& message) {
    try {
        auto msg = json::parse(message);

        std::string username = msg.value("username", "Player");
        int elo = msg.value("elo", 1200);
        int time_base_sec = msg.value("time_base", 600);
        int time_inc_sec  = msg.value("time_inc", 5);

        // Check if already in a game
        auto existing = room_mgr_.find_room_by_fd(conn.get_fd());
        if (existing && existing->get_state() != RoomState::FINISHED) {
            send_json(conn, make_error("You are already in a game (ID: " +
                      std::to_string(existing->get_id()) + ")"));
            return;
        }

        // Check if already queued
        if (matchmaker_.is_queued(conn.get_fd())) {
            send_json(conn, make_error("You are already in the matchmaking queue"));
            return;
        }

        PlayerId pid = next_player_id_.fetch_add(1);
        TimeControl tc(time_base_sec * 1000, time_inc_sec * 1000);

        matchmaker_.enqueue(conn.get_fd(), pid, username, elo, tc);

        // Try to match immediately
        auto matches = matchmaker_.try_match();

        // If we got matched, notifications are sent via the match callback.
        // If not, send a "queued" confirmation.
        if (matchmaker_.is_queued(conn.get_fd())) {
            json response;
            response["type"]       = "queued";
            response["queue_size"] = static_cast<int>(matchmaker_.queue_size());
            send_json(conn, response.dump());
        }
        // If matched, the match callback (set in main.cpp) handles notifications.

    } catch (const json::exception& e) {
        send_json(conn, make_error("Invalid JSON: " + std::string(e.what())));
    }
}

// ============================================================
// cancel_queue — Player leaves the matchmaking queue
// ============================================================

void GameHandler::handle_cancel_queue(net::Connection& conn, const std::string& /*message*/) {
    if (matchmaker_.dequeue(conn.get_fd())) {
        json response;
        response["type"] = "queue_cancelled";
        send_json(conn, response.dump());
    } else {
        send_json(conn, make_error("You are not in the matchmaking queue"));
    }
}

// ============================================================
// on_player_disconnect — cleanup queue + room state
// ============================================================

void GameHandler::on_player_disconnect(int connection_fd) {
    // Remove from matchmaking queue if queued
    matchmaker_.dequeue(connection_fd);

    // Notify game room if in one
    auto room = room_mgr_.find_room_by_fd(connection_fd);
    if (room) {
        room->on_disconnect(connection_fd);
    }
}

} // namespace game
} // namespace chess
